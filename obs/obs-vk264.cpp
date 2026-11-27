/* Vk264/H.264 OBS encoder adapter.  Uses the same Vulkan shaders and presets as the Vk264 CLI encoder. */
#include <obs-module.h>
#include "presets.hpp"

#include <cstring>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-vk264", "en-US")
OBS_MODULE_AUTHOR("Vk264 contributors")

namespace {
const uint8_t* nextStartCode(const uint8_t* current, const uint8_t* end) {
    // The shader emits four-byte Annex B start codes for every NAL.
    while (end - current >= 4) {
        if (current[0] == 0 && current[1] == 0 && current[2] == 0 && current[3] == 1)
            return current;
        ++current;
    }
    return end;
}

void removeNalPadding(const EncodedFrame& frame, std::vector<uint8_t>& packet) {
    const auto* start = reinterpret_cast<const uint8_t*>(frame.annexB);
    const auto* end = start + frame.bytes;
    packet.clear();
    for (auto* nal = nextStartCode(start, end); nal != end;) {
        const auto* next = nextStartCode(nal + 4, end);
        const auto* payloadEnd = next;
        // Annex B permits trailing zero bytes between NALs. OBS may include
        // those alignment bytes in the length of the NAL when muxing MP4.
        while (payloadEnd > nal + 4 && payloadEnd[-1] == 0) --payloadEnd;
        if (payloadEnd > nal + 4) packet.insert(packet.end(), nal, payloadEnd);
        nal = next;
    }
    if (packet.empty()) throw std::runtime_error("Vk264 received an empty encoded access unit");
}

struct Encoder {
    obs_encoder_t* owner;
    FrameConfig config;
    EncoderGpu gpu;
    std::vector<uint8_t> headers;
    std::vector<uint8_t> packetBytes;

    Encoder(obs_encoder_t* owner, FrameConfig config, const std::filesystem::path& shaders)
        : owner(owner), config(config), gpu(config, false, shaders) {
        // A black IDR generates the SPS and PPS before the output asks for
        // them. Encoding the first live frame at index zero resets rate state.
        const auto pixels = size_t(config.width) * config.height;
        std::memset(gpu.input(), 16, pixels);
        std::memset(gpu.input() + pixels, 128, pixels / 2);
        const auto preview = gpu.encode(config);
        removeNalPadding(preview, packetBytes);
        const auto* data = packetBytes.data();
        bool sps = false, pps = false;
        size_t start = 0;
        for (size_t i = 0; i + 4 < packetBytes.size(); ++i) {
            if (data[i] || data[i+1] || data[i+2] || data[i+3] != 1) continue;
            const unsigned type = data[i+4] & 31;
            if (type == 7) { start = i; sps = true; }
            if (type == 8) pps = true;
            if (type == 5 && sps && pps) {
                headers.assign(data + start, data + i);
                break;
            }
        }
        if (headers.empty()) throw std::runtime_error("Vk264 could not generate SPS and PPS");
    }
};

const char* encoder_name(void*) { return obs_module_text("EncoderName"); }

void encoder_destroy(void* data) { delete static_cast<Encoder*>(data); }

void* encoder_create(obs_data_t* settings, obs_encoder_t* owner) {
    try {
        auto* video = obs_encoder_video(owner);
        const auto* vi = video ? video_output_get_info(video) : nullptr;
        auto width = obs_encoder_get_width(owner);
        auto height = obs_encoder_get_height(owner);
        auto bitrate = obs_data_get_int(settings, "bitrate");
        auto qp = obs_data_get_int(settings, "qp");
        auto keyint = obs_data_get_int(settings, "keyint_sec");
        std::string_view rate = obs_data_get_string(settings, "rate_control");
        if (!vi) throw std::runtime_error("Vk264 could not read the OBS video format");
        if (bitrate < 64 || bitrate > 50000 || qp < 0 || qp > 51 ||
            keyint < 1 || keyint > 10 || (rate != "ABR" && rate != "CQP"))
            throw std::runtime_error("Vk264 encoder settings are out of range");

        auto config = makeFrameConfig(width, height, vi->fps_num, vi->fps_den,
                                      findPreset(obs_data_get_string(settings, "preset")));
        config.initialQp = static_cast<uint32_t>(qp);
        config.gop = framesForSeconds(config, static_cast<uint32_t>(keyint));
        config.targetBits = rate == "ABR" ? bitsPerFrame(config, static_cast<uint32_t>(bitrate)) : 0;
        config.videoSignal = colorSignal(1, 1, 1); // OBS adapter requests limited-range BT.709 I420 below
        finalizeFrameConfig(config);
        const auto* data_path = obs_get_module_data_path(obs_current_module());
        if (!data_path || !*data_path)
            throw std::runtime_error("Vk264 module data path is missing");
        return new Encoder(owner, config, std::filesystem::u8path(data_path) / "shaders");
    } catch (const std::exception& error) {
        obs_encoder_set_last_error(owner, error.what());
        return nullptr;
    }
}

bool encoder_encode(void* data, struct encoder_frame* frame, struct encoder_packet* packet, bool* received) {
    auto& encoder = *static_cast<Encoder*>(data);
    *received = false;
    if (!frame) return true;
    try {
        const auto& config = encoder.config;
        auto* input = encoder.gpu.input();
        for (unsigned plane = 0; plane < 3; ++plane) {
            const auto width = plane == 0 ? config.width : config.width / 2;
            const auto height = plane == 0 ? config.height : config.height / 2;
            if (!frame->data[plane] || frame->linesize[plane] < width)
                throw std::runtime_error("Vk264 received an invalid I420 plane");
            for (unsigned row = 0; row < height; ++row) {
                std::memcpy(input, frame->data[plane] + size_t(row) * frame->linesize[plane], width);
                input += width;
            }
        }
        const auto result = encoder.gpu.encode(config);
        removeNalPadding(result, encoder.packetBytes);
        packet->data = encoder.packetBytes.data();
        packet->size = encoder.packetBytes.size();
        packet->type = OBS_ENCODER_VIDEO;
        packet->pts = packet->dts = frame->pts;
        packet->keyframe = config.frameIndex % config.gop == 0;
        packet->priority = packet->drop_priority = packet->keyframe ? 3 : 2;
        ++encoder.config.frameIndex;
        *received = true;
        return true;
    } catch (const std::exception& error) {
        obs_encoder_set_last_error(encoder.owner, error.what());
        return false;
    }
}

bool encoder_headers(void* data, uint8_t** headers, size_t* size) {
    auto& encoder = *static_cast<Encoder*>(data);
    *headers = encoder.headers.data();
    *size = encoder.headers.size();
    return true;
}

void encoder_video_info(void*, struct video_scale_info* info) {
    info->format = VIDEO_FORMAT_I420;
    info->range = VIDEO_RANGE_PARTIAL;
    info->colorspace = VIDEO_CS_709;
}

void encoder_defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, "qp", 34);
    obs_data_set_default_string(settings, "rate_control", "ABR");
    obs_data_set_default_string(settings, "preset", defaultPreset.name.data());
    obs_data_set_default_int(settings, "bitrate", 6000);
    obs_data_set_default_int(settings, "keyint_sec", 5);
}

obs_properties_t* encoder_properties(void*) {
    auto* props = obs_properties_create();
    auto* preset = obs_properties_add_list(props, "preset", "Preset",
                                           OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    for (const auto& item : presets)
        obs_property_list_add_string(preset, item.name.data(), item.name.data());
    auto* rate = obs_properties_add_list(props, "rate_control", "Rate control",
                                         OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(rate, "Average bitrate", "ABR");
    obs_property_list_add_string(rate, "Constant QP", "CQP");
    obs_properties_add_int(props, "bitrate", "Target bitrate (kbps)", 64, 50000, 1);
    obs_properties_add_int(props, "qp", "QP / starting QP", 0, 51, 1);
    obs_properties_add_int(props, "keyint_sec", "Keyframe interval (seconds)", 1, 10, 1);
    obs_properties_add_text(props, "status", "Restart the output to apply changes.", OBS_TEXT_INFO);
    return props;
}
}

bool obs_module_load(void) {
    obs_encoder_info info{};
    info.id = "vk264_h264";
    info.type = OBS_ENCODER_VIDEO;
    info.codec = "h264";
    info.get_name = encoder_name;
    info.create = encoder_create;
    info.destroy = encoder_destroy;
    info.encode = encoder_encode;
    info.get_defaults = encoder_defaults;
    info.get_properties = encoder_properties;
    info.get_extra_data = encoder_headers;
    info.get_video_info = encoder_video_info;
    obs_register_encoder(&info);
    return true;
}

const char* obs_module_description(void) { return "Vk264/H.264 Vulkan compute H.264 encoder"; }
