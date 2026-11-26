#include "gpu.hpp"
#include "cavlc_codes.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
template<class T> T info(VkStructureType type) {
    T result{};
    result.sType = type;
    return result;
}

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

constexpr auto hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr auto deviceMemory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr auto transferSrc = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
constexpr auto transferDst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr std::uint32_t bindings = 14;

struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    VkDeviceSize size{};
    void* mapped{};
};

enum Stage { Pyramid, HalfPel, Motion, Spatial, OlderReferences, Residual, FlatIntra, MbFlags, Deblock, MbEntropy, Entropy, Scan, Assemble, StageCount };
constexpr std::array<const char*, StageCount> shaderNames{"pyramid", "halfpel", "motion", "spatial", "older_references", "residual", "flat_intra", "mb_flags",
    "deblock", "mb_entropy", "entropy", "scan", "assemble"};
static_assert(sizeof(FrameConfig) == 28 * sizeof(std::uint32_t));

std::uint32_t pairedSlotWords(const FrameConfig& c) {
    return macroblockSlotWords * c.mbWidth * 2;
}

// SHADER-ABI: an IDR frame is emitted as one whole-picture slice so that
// reconstructed above/above-left samples are intra predictors available to
// the decoder.  Reserve the entire frame's macroblock body capacity in that
// single slice slot.
std::uint32_t idrSlotWords(const FrameConfig& c) {
    return macroblockSlotWords * c.mbWidth * c.mbHeight;
}

constexpr std::uint32_t parallelMetaWords = 12;
std::uint32_t parallelSummaryWords(std::uint32_t slotWords) { return (slotWords + 15) / 16; }

// Extra Slices-buffer regions used by the parallel CAVLC assembly: raw RBSP,
// chunk summaries, per-slice metadata and per-macroblock bit offsets.
VkDeviceSize parallelBytes(const FrameConfig& c, std::uint32_t slotWords, std::uint32_t sliceCount) {
    std::uint64_t records = std::uint64_t(sliceCount) + 1u;
    std::uint64_t finalBytes = std::uint64_t(slotWords) * 4u * records;
    std::uint64_t extra = (std::uint64_t(parallelSummaryWords(slotWords)) * records +
                           std::uint64_t(parallelMetaWords) * records +
                           std::uint64_t(c.mbWidth) * c.mbHeight) * 4u;
    return 2u * finalBytes + extra;
}
}

struct EncoderGpu::Impl {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queueFamily{}, timestampBits{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkDescriptorPool descriptorPool{};
    std::vector<VkDescriptorSet> sets;
    VkDeviceSize frameBytes{}, referenceStride{};
    std::array<VkPipeline, StageCount> pipelines{};
    VkPipeline frameDeblock{};
    VkPipeline sliceOffsets{}, sliceBodies{}, sliceEpb[3]{};
    bool parallelCapable{};
    VkCommandPool commandPool{};
    VkCommandBuffer commands{};
    VkFence fence{};
    VkQueryPool timestamps{};

    enum Resource { Upload, Source, Recon, Coefficients, Slices, Records, Output, Rate, Tables,
                    Vectors, Filtered, QuarterLuma, Syntax, Bodies, ReadRecon, ResourceCount };
    std::array<Buffer, ResourceCount> buffers{};

    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (timestamps) vkDestroyQueryPool(device, timestamps, nullptr);
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            for (auto pipeline : pipelines) if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (sliceOffsets) vkDestroyPipeline(device, sliceOffsets, nullptr);
            if (sliceBodies) vkDestroyPipeline(device, sliceBodies, nullptr);
            for (auto pipeline : sliceEpb) if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (frameDeblock) vkDestroyPipeline(device, frameDeblock, nullptr);
            if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            for (const auto& b : buffers) {
                if (b.mapped) vkUnmapMemory(device, b.memory);
                if (b.handle) vkDestroyBuffer(device, b.handle, nullptr);
                if (b.memory) vkFreeMemory(device, b.memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void createBuffer(Resource resource, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags) {
        auto& buffer = buffers[resource];
        buffer.size = size;
        auto desc = info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        desc.size = size;
        desc.usage = usage;
        desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &desc, nullptr, &buffer.handle), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffer.handle, &req);
        std::uint32_t chosen = memoryProperties.memoryTypeCount;
        for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            auto type = memoryProperties.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) && (type & flags) == flags) {
                chosen = i;
                break;
            }
        }
        if (chosen == memoryProperties.memoryTypeCount) throw std::runtime_error("No compatible Vulkan memory type");
        auto allocation = info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocation.allocationSize = req.size;
        allocation.memoryTypeIndex = chosen;
        check(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped), "vkMapMemory");
    }

    void selectDevice() {
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        int highestScore = 0;
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(candidate, &p);
            if (p.apiVersion < VK_API_VERSION_1_1 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                p.limits.maxPerStageDescriptorStorageBuffers < bindings || p.limits.maxDescriptorSetStorageBuffers < bindings)
                continue;
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            for (std::uint32_t i = 0; i < families; ++i) {
                const auto& q = queues[i];
                if (!(q.queueFlags & VK_QUEUE_COMPUTE_BIT) || !q.queueCount) continue;
                int score = 1 + 4 * (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) +
                    2 * (q.timestampValidBits != 0) + ((q.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0);
                if (score > highestScore) {
                    highestScore = score;
                    physical = candidate;
                    properties = p;
                    queueFamily = i;
                    timestampBits = q.timestampValidBits;
                }
            }
        }
        if (!physical) throw std::runtime_error("No suitable Vulkan 1.1 GPU compute device");
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    }

    std::filesystem::path shaderDirectory;

    VkPipeline makePipeline(const char* name) {
        auto path = shaderDirectory / (std::string(name) + ".spv");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Could not open " + path.string());
        auto size = static_cast<std::streamoff>(file.tellg());
        if (size <= 0 || size % 4) throw std::runtime_error("Invalid SPIR-V size in " + path.string());
        std::vector<std::uint32_t> code(static_cast<std::size_t>(size / 4));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(code.data()), size)) throw std::runtime_error("Could not read " + path.string());
        auto shader = info<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        shader.codeSize = static_cast<std::size_t>(size);
        shader.pCode = code.data();
        VkShaderModule module{};
        check(vkCreateShaderModule(device, &shader, nullptr, &module), "vkCreateShaderModule");
        auto desc = info<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        desc.stage = info<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        desc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        desc.stage.module = module;
        desc.stage.pName = "main";
        desc.layout = pipelineLayout;
        VkPipeline pipeline{};
        auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &desc, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        check(result, name);
        return pipeline;
    }

    void init(const FrameConfig& c, bool readRecon) {
        auto app = info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "Wx264";
        app.apiVersion = VK_API_VERSION_1_1;
        auto instanceInfo = info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instanceInfo.pApplicationInfo = &app;
        check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        selectDevice();
        float priority = 1.0f;
        auto queueInfo = info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        auto deviceInfo = info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        VkDeviceSize sourceSize = (VkDeviceSize(c.width) * c.height * 3 / 2 + 3) & ~VkDeviceSize(3);
        VkDeviceSize mbCount = VkDeviceSize(c.mbWidth) * c.mbHeight;
        VkDeviceSize scratchSize = VkDeviceSize(c.slotWords) * 4 * (c.sliceCount + 1);
        bool paired = c.sliceRows == 2 && c.partitions == 0 && c.gop > 1;
        parallelCapable = paired;
        if (paired)
            scratchSize = std::max(scratchSize, VkDeviceSize(pairedSlotWords(c)) * 4 * ((c.mbHeight + 1) / 2 + 1));
        // SHADER-ABI: IDR frames collapse to one slice; reserve enough scratch
        // words for the whole frame in that one slot (two records: AUD/SPS/PPS
        // plus the picture slice).
        if (c.gop > 1u)
            scratchSize = std::max(scratchSize, VkDeviceSize(idrSlotWords(c)) * 4 * 2);
        if (parallelCapable) {
            scratchSize = std::max(scratchSize, parallelBytes(c, c.slotWords, c.sliceCount));
            if (paired)
                scratchSize = std::max(scratchSize, parallelBytes(c, pairedSlotWords(c), (c.mbHeight + 1) / 2));
            if (c.gop > 1u) scratchSize = std::max(scratchSize, parallelBytes(c, idrSlotWords(c), 1));
        }
        createBuffer(Upload, sourceSize, transferSrc, hostMemory);
        createBuffer(Source, sourceSize, storage | transferDst, deviceMemory);
        frameBytes = mbCount * 384;
        auto alignment = std::max<VkDeviceSize>(4, properties.limits.minStorageBufferOffsetAlignment);
        referenceStride = ((frameBytes + alignment - 1) / alignment) * alignment;
        auto referenceBytes = referenceStride * (c.references + 1);
        if (referenceBytes > properties.limits.maxStorageBufferRange)
            throw std::runtime_error("The requested reference pool exceeds this GPU's storage-buffer range");
        createBuffer(Recon, referenceBytes, storage | transferSrc, deviceMemory);
        createBuffer(Coefficients, mbCount * 432 * 4, storage, deviceMemory);
        createBuffer(Slices, scratchSize, storage | transferDst, deviceMemory);
        createBuffer(Records, 8 * (c.sliceCount + 1), storage, deviceMemory);
        createBuffer(Output, scratchSize + 16, storage, hostMemory);
        createBuffer(Rate, 12, storage | transferDst, deviceMemory);
        createBuffer(Tables, sizeof(cavlcCodes), storage, hostMemory);
        createBuffer(Vectors, mbCount * 6 * 16, storage, deviceMemory);
        createBuffer(Filtered, VkDeviceSize(c.mbWidth * 16 + 8) * (c.mbHeight * 16 + 8) * 4, storage, deviceMemory);
        createBuffer(QuarterLuma, mbCount * 16 * 4, storage, deviceMemory);
        createBuffer(Syntax, paired ? mbCount * 8 : 8, storage, deviceMemory);
        createBuffer(Bodies, paired ? mbCount * macroblockSlotWords * 4 : 4, storage, deviceMemory);
        if (readRecon) createBuffer(ReadRecon, frameBytes, transferDst, hostMemory);
        std::memset(buffers[Upload].mapped, 0, static_cast<std::size_t>(sourceSize));
        std::memcpy(buffers[Tables].mapped, cavlcCodes, sizeof(cavlcCodes));

        std::array<VkDescriptorSetLayoutBinding, bindings> layout{};
        for (std::uint32_t i = 0; i < bindings; ++i)
            layout[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        auto layoutInfo = info<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layoutInfo.bindingCount = bindings;
        layoutInfo.pBindings = layout.data();
        check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FrameConfig)};
        auto pipeLayout = info<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        pipeLayout.setLayoutCount = 1;
        pipeLayout.pSetLayouts = &setLayout;
        pipeLayout.pushConstantRangeCount = 1;
        pipeLayout.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pipeLayout, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        std::uint32_t slotCount = c.references + 1;
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slotCount * bindings};
        auto poolInfo = info<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        poolInfo.maxSets = slotCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "vkCreateDescriptorPool");
        std::vector<VkDescriptorSetLayout> layouts(slotCount, setLayout);
        sets.resize(slotCount);
        auto setInfo = info<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        setInfo.descriptorPool = descriptorPool;
        setInfo.descriptorSetCount = slotCount;
        setInfo.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(device, &setInfo, sets.data()), "vkAllocateDescriptorSets");
        for (std::uint32_t i = 0; i < slotCount; ++i) updateDescriptors(i);
        for (int i = 0; i < StageCount; ++i) {
            if ((i == FlatIntra && !c.flatIntra) || (i == Deblock && c.deblock != 1)) continue;
            pipelines[i] = makePipeline(shaderNames[i]);
        }
        if (c.deblock == 2) frameDeblock = makePipeline("deblock_frame");
        if (parallelCapable) {
            sliceOffsets = makePipeline("slice_offsets");
            sliceBodies = makePipeline("slice_bodies");
            for (int i = 0; i < 3; ++i) {
                auto name = "slice_epb" + std::to_string(i);
                sliceEpb[i] = makePipeline(name.c_str());
            }
        }

        auto commandPoolInfo = info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        commandPoolInfo.queueFamilyIndex = queueFamily;
        check(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool), "vkCreateCommandPool");
        auto commandInfo = info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &commandInfo, &commands), "vkAllocateCommandBuffers");
        auto fenceInfo = info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        if (timestampBits) {
            auto queryInfo = info<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = StageCount + 1;
            check(vkCreateQueryPool(device, &queryInfo, nullptr, &timestamps), "vkCreateQueryPool");
        }
    }

    void updateDescriptors(unsigned index) {
        const Resource order[] = {Source, Recon, Recon, Coefficients, Slices, Records, Output, Rate, Tables,
                                  Vectors, Filtered, QuarterLuma, Syntax, Bodies};
        std::array<VkDescriptorBufferInfo, bindings> desc{};
        std::array<VkWriteDescriptorSet, bindings> writes{};
        for (std::uint32_t i = 0; i < bindings; ++i) {
            const auto& b = buffers[order[i]];
            desc[i] = {b.handle, 0, b.size};
            if (i == 2) desc[i] = {b.handle, referenceStride * index, frameBytes};
            writes[i] = info<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[i].dstSet = sets[index];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &desc[i];
        }
        vkUpdateDescriptorSets(device, bindings, writes.data(), 0, nullptr);
    }

    void copy(Resource from, Resource to, VkDeviceSize sourceOffset = 0) {
        VkBufferCopy region{sourceOffset, 0, buffers[to].size};
        vkCmdCopyBuffer(commands, buffers[from].handle, buffers[to].handle, 1, &region);
    }

    void barrier(VkPipelineStageFlags before, VkAccessFlags src, VkPipelineStageFlags after, VkAccessFlags dst) {
        auto m = info<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        m.srcAccessMask = src;
        m.dstAccessMask = dst;
        vkCmdPipelineBarrier(commands, before, after, 0, 1, &m, 0, nullptr, 0, nullptr);
    }

    void dispatch(Stage stage, std::uint32_t groups, std::uint32_t rows = 1, bool stamp = true) {
        if (groups) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[stage]);
            vkCmdDispatch(commands, groups, rows, 1);
        }
        if (timestamps && stamp) vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, stage + 1);
    }

    void dispatchPipeline(VkPipeline pipeline, std::uint32_t groups, std::uint32_t rows = 1) {
        if (groups) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdDispatch(commands, groups, rows, 1);
        }
    }

    void dispatchDeblock(const FrameConfig& c) {
        if (c.deblock != 2) {
            dispatch(Deblock, c.deblock ? c.sliceCount : 0);
            return;
        }
        const auto tilesU = (c.mbWidth + c.mbHeight + 2) / 4;
        const auto tilesV = (c.mbHeight + 3) / 4;
        const auto maxDelta = (c.mbWidth + 2) / 4;
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, frameDeblock);
        for (std::uint32_t wave = 0; wave < tilesU + tilesV - 1; ++wave) {
            // A tile covers u=4*tileU+[0,3] and y=4*tileV+[0,3].
            // Its possible x values straddle 4*(tileU-tileV).
            auto first = std::max(wave >= tilesU ? wave - tilesU + 1 : 0,
                                  wave > maxDelta ? (wave - maxDelta + 1) / 2 : 0);
            auto last = std::min(tilesV - 1, wave / 2);
            if (first > last) continue;
            vkCmdPushConstants(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               offsetof(FrameConfig, deblockWave), sizeof(wave), &wave);
            vkCmdDispatch(commands, last - first + 1, 1, 1);
            if (wave < tilesU + tilesV - 2)
                barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        if (timestamps) vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, Deblock + 1);
    }

    EncodedFrame encode(FrameConfig c) {
        constexpr auto compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        constexpr auto transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        constexpr auto write = VK_ACCESS_SHADER_WRITE_BIT;
        constexpr auto read = VK_ACCESS_SHADER_READ_BIT;
        bool intra = c.frameIndex % c.gop == 0;
        bool paired = !intra && c.sliceRows == 2 && c.partitions == 0;
        bool parallelSlices = parallelCapable && (paired || (intra && c.gop > 1u));
        // SHADER-ABI: see layout.glsl::idrWholeFrame().  One whole-frame slice
        // keeps intra prediction within the slice boundary (H.264 8.3.1).
        if (intra && c.gop > 1u) {
            c.sliceCount = 1u;
            c.slotWords = idrSlotWords(c);
        }
        if (paired) {
            c.slotWords = pairedSlotWords(c);
            c.sliceCount = (c.mbHeight + 1) / 2;
        }
        check(vkResetCommandPool(device, commandPool, 0), "vkResetCommandPool");
        check(vkResetFences(device, 1, &fence), "vkResetFences");
        auto begin = info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
        barrier(compute | transfer, write | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                compute | transfer, read | write | VK_ACCESS_TRANSFER_WRITE_BIT);
        // The parallel assembly uses atomic word ORs into the Slices buffer;
        // clear all regions (final NAL, raw RBSP, summaries, metadata, offsets).
        if (parallelCapable) vkCmdFillBuffer(commands, buffers[Slices].handle, 0, VK_WHOLE_SIZE, 0);
        copy(Upload, Source);
        if (c.frameIndex == 0) vkCmdFillBuffer(commands, buffers[Rate].handle, 0, VK_WHOLE_SIZE, 0);
        barrier(transfer, VK_ACCESS_TRANSFER_WRITE_BIT, compute, read | write);
        if (timestamps) {
            vkCmdResetQueryPool(commands, timestamps, 0, StageCount + 1);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 0);
        }
        auto slotCount = c.references + 1;
        auto slot = c.frameIndex % slotCount;
        auto previousSlot = (slot + slotCount - 1) % slotCount;
        c.referenceStrideWords = static_cast<std::uint32_t>(referenceStride / 4);
        c.previousWords = previousSlot * c.referenceStrideWords;
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &sets[slot], 0, nullptr);
        vkCmdPushConstants(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(c), &c);
        auto mbCount = c.mbWidth * c.mbHeight;
        bool pyramid = !intra && c.motionRadius >= 4;
        dispatch(Pyramid, pyramid ? (c.mbWidth + 3) / 4 : 0, (c.mbHeight + 1) / 2);
        bool filter = !intra && c.subpel && c.motionRadius;
        dispatch(HalfPel, filter ? c.mbWidth + 1 : 0, 2 * c.mbHeight + 1);
        if (filter || pyramid) barrier(compute, write, compute, read);
        dispatch(Motion, intra ? 0 : mbCount, c.partitions && c.motionRadius ? 5 : 1);
        barrier(compute, write, compute, read | write);
        bool spatial = pyramid && c.partitions == 0 && c.subpel;
        dispatch(Spatial, spatial ? mbCount : 0);
        if (spatial) barrier(compute, write, compute, read | write);
        bool olderReferences = paired && c.references > 1 && c.frameIndex % c.gop > 1;
        dispatch(OlderReferences, olderReferences ? mbCount : 0);
        if (olderReferences) barrier(compute, write, compute, read | write);
        dispatch(Residual, intra ? c.sliceCount : mbCount);
        barrier(compute, write, compute, read | write);
        bool flatIntra = paired && c.flatIntra;
        dispatch(FlatIntra, flatIntra ? c.sliceCount : 0);
        if (flatIntra) barrier(compute, write, compute, read | write);
        dispatch(MbFlags, paired ? c.sliceCount : 0);
        if (paired) barrier(compute, write, compute, read | write);
        dispatchDeblock(c);
        dispatch(MbEntropy, parallelSlices ? (mbCount + 31) / 32 : 0);
        if (parallelSlices) barrier(compute, write, compute, read | write);
        // All parallel sub-passes are timed as part of the Entropy stage.
        dispatch(Entropy, (c.sliceCount + 8) / 8, 1, false);
        barrier(compute, write, compute, read | write);
        if (parallelSlices) {
            std::uint32_t chunkGroups = (parallelSummaryWords(c.slotWords) + 63) / 64;
            dispatchPipeline(sliceOffsets, c.sliceCount + 1);
            barrier(compute, write, compute, read | write);
            dispatchPipeline(sliceBodies, (mbCount + 63) / 64);
            barrier(compute, write, compute, read | write);
            dispatchPipeline(sliceEpb[0], chunkGroups, c.sliceCount + 1);
            barrier(compute, write, compute, read | write);
            dispatchPipeline(sliceEpb[1], 1, c.sliceCount + 1);
            barrier(compute, write, compute, read | write);
            dispatchPipeline(sliceEpb[2], chunkGroups, c.sliceCount + 1);
            barrier(compute, write, compute, read | write);
        }
        if (timestamps) vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, Entropy + 1);
        dispatch(Scan, 1);
        barrier(compute, write, compute, read | write);
        dispatch(Assemble, c.sliceCount + 1);
        if (buffers[ReadRecon].mapped) {
            barrier(compute, write, transfer, VK_ACCESS_TRANSFER_READ_BIT);
            copy(Recon, ReadRecon, slot * referenceStride);
        }
        barrier(compute | transfer, write | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        auto submit = info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "vkWaitForFences");

        auto* output = static_cast<const std::uint32_t*>(buffers[Output].mapped);
        if (output[1] || output[0] > buffers[Output].size - 16)
            throw std::runtime_error("Slice syntax or capacity error at frame " + std::to_string(c.frameIndex));
        EncodedFrame result{reinterpret_cast<const char*>(output + 4), output[0], output[2] & 255u,
                            output[3] & 65535u, output[2] >> 8u, output[3] >> 16u,
                            static_cast<const char*>(buffers[ReadRecon].mapped), {}};
        if (timestamps) {
            std::array<std::uint64_t, StageCount + 1> ticks{};
            check(vkGetQueryPoolResults(device, timestamps, 0, StageCount + 1, sizeof(ticks), ticks.data(),
                                        sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
            auto mask = timestampBits == 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << timestampBits) - 1;
            for (int i = 0; i < StageCount; ++i)
                result.gpuMilliseconds[i] = double((ticks[i + 1] - ticks[i]) & mask) * properties.limits.timestampPeriod / 1e6;
        }
        return result;
    }
};

EncoderGpu::EncoderGpu(const FrameConfig& c, bool readRecon, std::filesystem::path shaderDirectory)
    : impl_(std::make_unique<Impl>()) {
    impl_->shaderDirectory = shaderDirectory.empty() ? std::filesystem::path(WX_SHADER_DIR)
                                                      : std::move(shaderDirectory);
    impl_->init(c, readRecon);
}
EncoderGpu::~EncoderGpu() = default;
char* EncoderGpu::input() { return static_cast<char*>(impl_->buffers[Impl::Upload].mapped); }
EncodedFrame EncoderGpu::encode(const FrameConfig& c) { return impl_->encode(c); }
std::string EncoderGpu::deviceName() const { return impl_->properties.deviceName; }
