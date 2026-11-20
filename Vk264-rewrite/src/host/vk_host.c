/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal Vulkan compute host: instance, device, one storage buffer, dispatch. */
#include "vk_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int find_family(vk_host_t *h)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(h->phys, &n, NULL);
    VkQueueFamilyProperties *props = malloc(n * sizeof(*props));
    vkGetPhysicalDeviceQueueFamilyProperties(h->phys, &n, props);
    int found = -1;
    for (uint32_t i = 0; i < n; ++i)
        if (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { found = (int)i; break; }
    free(props);
    return found;
}

int vk_host_init(vk_host_t *h)
{
    memset(h, 0, sizeof(*h));
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
    if (vkCreateInstance(&ici, NULL, &h->instance) != VK_SUCCESS) return -1;

    uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(h->instance, &count, &h->phys) != VK_SUCCESS || count == 0)
        return -2;
    h->queue_family = (uint32_t)find_family(h);
    if ((int)h->queue_family < 0) return -3;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = h->queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (vkCreateDevice(h->phys, &dci, NULL, &h->device) != VK_SUCCESS) return -4;
    vkGetDeviceQueue(h->device, h->queue_family, 0, &h->queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .queueFamilyIndex = h->queue_family };
    if (vkCreateCommandPool(h->device, &pci, NULL, &h->cmd_pool) != VK_SUCCESS) return -5;
    return 0;
}

void vk_host_shutdown(vk_host_t *h)
{
    if (h->cmd_pool) vkDestroyCommandPool(h->device, h->cmd_pool, NULL);
    if (h->device) vkDestroyDevice(h->device, NULL);
    if (h->instance) vkDestroyInstance(h->instance, NULL);
    memset(h, 0, sizeof(*h));
}

static VkShaderModule load_spv(VkDevice dev, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open shader %s\n", path); return VK_NULL_HANDLE; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); return VK_NULL_HANDLE; }
    fclose(f);
    VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)size,
                                    .pCode = (const uint32_t *)buf };
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(dev, &si, NULL, &m);
    free(buf);
    return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

int vk_host_dispatch(vk_host_t *h, const char *spv_path, void *data, size_t bytes,
                     uint32_t workgroups)
{
    VkResult r;
    VkBuffer buf;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = bytes,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if ((r = vkCreateBuffer(h->device, &bi, NULL, &buf)) != VK_SUCCESS) return -10;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(h->device, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(h->phys, &mp);
    uint32_t mem_type = 0xffffffffu;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mem_type = i; break;
        }
    if (mem_type == 0xffffffffu) return -14;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = mr.size, .memoryTypeIndex = mem_type };
    VkDeviceMemory mem;
    if ((r = vkAllocateMemory(h->device, &ai, NULL, &mem)) != VK_SUCCESS) return -11;
    vkBindBufferMemory(h->device, buf, mem, 0);
    void *mapped = NULL;
    if (vkMapMemory(h->device, mem, 0, bytes, 0, &mapped) != VK_SUCCESS) return -15;
    memcpy(mapped, data, bytes);
    vkUnmapMemory(h->device, mem);

    VkShaderModule sm = load_spv(h->device, spv_path);
    if (!sm) return -12;
    VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" };
    VkDescriptorSetLayoutBinding binding = { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding };
    VkDescriptorSetLayout layout;
    vkCreateDescriptorSetLayout(h->device, &li, NULL, &layout);
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &layout };
    VkPipelineLayout pl;
    vkCreatePipelineLayout(h->device, &pli, NULL, &pl);
    VkComputePipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage, .layout = pl };
    VkPipeline pipe;
    if ((r = vkCreateComputePipelines(h->device, VK_NULL_HANDLE, 1, &pi, NULL, &pipe)) != VK_SUCCESS)
        return -13;

    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool pool;
    vkCreateDescriptorPool(h->device, &dpi, NULL, &pool);
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &layout };
    VkDescriptorSet set;
    vkAllocateDescriptorSets(h->device, &dai, &set);
    VkDescriptorBufferInfo dbi = { .buffer = buf, .offset = 0, .range = bytes };
    VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(h->device, 1, &write, 0, NULL);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = h->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1 };
    vkAllocateCommandBuffers(h->device, &cai, &cmd);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &cbi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, workgroups, 1, 1);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd };
    vkQueueSubmit(h->queue, 1, &sub, VK_NULL_HANDLE);
    vkQueueWaitIdle(h->queue);

    if (vkMapMemory(h->device, mem, 0, bytes, 0, &mapped) != VK_SUCCESS) return -16;
    memcpy(data, mapped, bytes);
    vkUnmapMemory(h->device, mem);

    vkFreeCommandBuffers(h->device, h->cmd_pool, 1, &cmd);
    vkDestroyDescriptorPool(h->device, pool, NULL);
    vkDestroyDescriptorSetLayout(h->device, layout, NULL);
    vkDestroyPipeline(h->device, pipe, NULL);
    vkDestroyPipelineLayout(h->device, pl, NULL);
    vkDestroyShaderModule(h->device, sm, NULL);
    vkDestroyBuffer(h->device, buf, NULL);
    vkFreeMemory(h->device, mem, NULL);
    return 0;
}
