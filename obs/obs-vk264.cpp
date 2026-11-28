/* Vk264/H.264 OBS encoder adapter.  Uses the same Vulkan shaders and presets as the Vk264 CLI encoder. */
#include <obs-module.h>
#include <util/base.h>
#include "presets.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
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

const char* validateSettings(int bitrate, int qp, int keyint, std::string_view rate) {
    if (rate != "ABR" && rate != "ABR-SMOOTH" && rate != "CQP")
        return "Vk264: Rate control must be Average bitrate, Smoothed ABR or Constant QP";
    // CQP ignores bitrate entirely (the UI hides it), so an out-of-range
    // bitrate left over from a previous mode must not block a CQP encode.
    if (rate == "CQP") {
        if (qp < 0 || qp > 51) return "Vk264: QP must be 0-51 (lower is higher quality)";
    } else if (bitrate < 64 || bitrate > 50000) {
        return "Vk264: Target bitrate must be 64-50000 kbit/s";
    }
    if (keyint < 1 || keyint > 10)
        return "Vk264: Keyframe interval must be 1-10 seconds";
    return nullptr;
}

const char* statusText(std::string_view rate) {
    if (rate == "CQP")
        return "Constant QP (CQP): Target bitrate is ignored. QP fixes quality "
               "(0 = lossless, lower = better); file size varies with content. "
               "Restart the output to apply changes.";
    if (rate == "ABR-SMOOTH")
        return "Smoothed ABR: targets Target bitrate while averaging frame "
               "activity to reduce short-term spikes. Like the CLI, it starts "
               "at the stored QP (default 34) and its EMA clamps itself to "
               "QP 18-32 on 50 fps sources. Restart the output to apply changes.";
    return "Average bitrate (ABR): targets Target bitrate. QP is not a target; "
           "ABR starts at QP 14 to protect the first keyframe. "
           "Restart the output to apply changes.";
}

// The properties panel has no reliable video_t handle (OBS video may not be
// reset yet), so show the exact formula instead of a guessed frame count.
const char* gopSummary() { return "GOP = keyint x fps"; }

std::string effectiveText(obs_data_t* settings) {
    const std::string_view rate = obs_data_get_string(settings, "rate_control");
    const int bitrate = obs_data_get_int(settings, "bitrate");
    const int qp = obs_data_get_int(settings, "qp");
    const int keyint = obs_data_get_int(settings, "keyint_sec");
    const char* preset = obs_data_get_string(settings, "preset");
    const char* gop = gopSummary();
    const int deblockValue = obs_data_get_int(settings, "deblock");
    const int deblock = (deblockValue >= 0 && deblockValue <= 2) ? deblockValue : 2;
    const bool flatIntra = obs_data_get_bool(settings, "flat_intra");
    char text[512];
    if (rate == "CQP") {
        std::snprintf(text, sizeof(text),
            "Effective: CQP | QP=%d (0=lossless) | bitrate ignored | "
            "keyint=%d s -> %s | preset=%s | deblock=%d | flat-intra=%s",
            qp, keyint, gop, preset, deblock, flatIntra ? "on" : "off");
    } else {
        std::snprintf(text, sizeof(text),
            "Effective: %s | target=%d kbit/s | starting QP=%s | "
            "keyint=%d s -> %s | preset=%s | deblock=%d | flat-intra=%s",
            rate == "ABR-SMOOTH" ? "ABR-SMOOTH" : "ABR", bitrate,
            rate == "ABR-SMOOTH" ? "stored QP (EMA clamped)" : "14 (capped)",
            keyint, gop, preset, deblock, flatIntra ? "on" : "off");
    }
    return text;
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
        if (!vi) throw std::runtime_error("Vk264 could not read the OBS video format (start the output after video is active)");
        if (const char* error = validateSettings(bitrate, qp, keyint, rate))
            throw std::runtime_error(error);

        auto config = makeFrameConfig(width, height, vi->fps_num, vi->fps_den,
                                      findPreset(obs_data_get_string(settings, "preset")));
        config.initialQp = static_cast<uint32_t>(qp);
        config.gop = framesForSeconds(config, static_cast<uint32_t>(keyint));
        config.targetBits = rate != "CQP" ? bitsPerFrame(config, static_cast<uint32_t>(bitrate)) : 0;
        config.smoothAbr = rate == "ABR-SMOOTH" ? 1u : 0u;
        // Exposed so the artifact fixes can be A/B tested without leaving OBS:
        // 2 = full-frame filtering, 1 = within slices, 0 = off; flat-intra
        // adds the DC-only Intra16 candidate for dark flat macroblocks.
        const int deblock = obs_data_get_int(settings, "deblock");
        config.deblock = (deblock >= 0 && deblock <= 2) ? static_cast<uint32_t>(deblock) : 2u;
        config.flatIntra = obs_data_get_bool(settings, "flat_intra") ? 1u : 0u;
        // Mirror the CLI: an explicit QP above 14 with ABR makes the first
        // IDR far too coarse and permanently lowers mean quality at the same
        // achieved bitrate.  Fixed QP keeps the caller's exact value.
        // ABR-SMOOTH deliberately keeps the caller's QP on the CLI; mirror
        // that here so the two entry points behave identically.
        if (config.targetBits != 0u && config.smoothAbr == 0u && config.initialQp > 14u)
            config.initialQp = 14u;
        config.videoSignal = colorSignal(1, 1, 1); // OBS adapter requests limited-range BT.709 I420 below
        // Log the EFFECTIVE settings so a UI/saved-settings mismatch is visible
        // in the OBS log instead of silently producing an unexpected bitrate.
        blog(LOG_INFO, "Vk264: settings rate=%s bitrate=%d qp=%d keyint=%d preset=%s "
             "-> initialQp=%u gop=%u targetBits=%u",
             obs_data_get_string(settings, "rate_control"), bitrate, qp, keyint,
             obs_data_get_string(settings, "preset"), config.initialQp, config.gop,
             config.targetBits);
        finalizeFrameConfig(config);
        const auto* data_path = obs_get_module_data_path(obs_current_module());
        if (!data_path || !*data_path)
            throw std::runtime_error("Vk264 module data path is missing");
        return new Encoder(owner, config, std::filesystem::u8path(data_path) / "shaders");
    } catch (const std::exception& error) {
        blog(LOG_ERROR, "%s", error.what());
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
    obs_data_set_default_int(settings, "deblock", 2);
    obs_data_set_default_bool(settings, "flat_intra", true);
}

// Mirror the x264 OBS plugin: only the field used by the selected rate
// control mode is shown, so a CQP recording cannot be mistaken for a missed
// bitrate target.  The mode-specific explanation and the effective-settings
// readout are refreshed for every field, not just the mode selector.
bool settingsChanged(obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    const std::string_view mode = obs_data_get_string(settings, "rate_control");
    const bool abr = mode != "CQP";
    obs_property_set_visible(obs_properties_get(props, "bitrate"), abr);
    obs_property_set_visible(obs_properties_get(props, "qp"), !abr);
    obs_property_set_description(obs_properties_get(props, "status"), statusText(mode));
    const std::string effective = effectiveText(settings);
    obs_property_set_description(obs_properties_get(props, "effective"), effective.c_str());
    return true;
}

obs_properties_t* encoder_properties(void*) {
    auto* props = obs_properties_create();
    auto* preset = obs_properties_add_list(props, "preset",
        "Quality preset (Vk264 fixed search settings)",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    for (const auto& item : presets)
        obs_property_list_add_string(preset, item.name.data(), item.name.data());
    auto* rate = obs_properties_add_list(props, "rate_control", "Rate control",
                                         OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(rate, "Average bitrate (ABR)", "ABR");
    obs_property_list_add_string(rate, "Smoothed average bitrate (ABR-SMOOTH)", "ABR-SMOOTH");
    obs_property_list_add_string(rate, "Constant QP (CQP)", "CQP");
    auto* bitrate = obs_properties_add_int(props, "bitrate", "Target bitrate (kbit/s, 64-50000)", 64, 50000, 1);
    auto* qp = obs_properties_add_int(props, "qp", "Constant QP (0-51; lower = better; 0 = lossless)", 0, 51, 1);
    auto* keyint = obs_properties_add_int(props, "keyint_sec", "Keyframe interval (seconds, 1-10)", 1, 10, 1);
    auto* deblock = obs_properties_add_list(props, "deblock", "Deblocking (artifact A/B test)",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(deblock, "Full frame (default, filters slice seams)", 2);
    obs_property_list_add_int(deblock, "Within slices only", 1);
    obs_property_list_add_int(deblock, "Off", 0);
    auto* flatIntra = obs_properties_add_bool(props, "flat_intra",
        "DC-only intra for dark flat blocks (QP 14+)");
    obs_properties_add_text(props, "status", "", OBS_TEXT_INFO);
    obs_properties_add_text(props, "effective", "", OBS_TEXT_INFO);
    obs_properties_add_text(props, "limits",
        "Fixed capabilities: High profile, 8-bit I420, BT.709 limited range, "
        "CAVLC, no B-frames. The four quality presets are Vk264's own fixed "
        "search settings (motion radius / SATD candidates / references), not "
        "x264 presets; there are no profile, tune or x264-options fields "
        "because this encoder does not implement them. Keyframe interval "
        "defaults to 5 s for quality; use 2 s for services that need shorter GOPs.",
        OBS_TEXT_INFO);
    for (auto* field : {preset, rate, bitrate, qp, keyint, deblock, flatIntra})
        obs_property_set_modified_callback(field, settingsChanged);
    // Populate the info fields even when the host does not call
    // obs_properties_apply_settings immediately after building the panel.
    obs_data_t* initial = obs_data_create();
    encoder_defaults(initial);
    settingsChanged(props, nullptr, initial);
    obs_data_release(initial);
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
