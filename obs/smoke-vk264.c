/* Real libobs integration: raw I420 -> plugin -> encoded OBS output.
 * No desktop settings, installed plugins, or live services are modified.
 * Extended with UI visibility / status-text / validation checks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <obs.h>
#include <media-io/video-frame.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sink { obs_output_t *output; FILE *file; int count; int64_t last_pts; };
static volatile LONG packets;
static const char *output_path;
static const char *sink_name(void *p) { (void)p; return "vk264 smoke sink"; }
static void *sink_create(obs_data_t *settings, obs_output_t *output)
{
    (void)settings;
    struct sink *s = calloc(1,sizeof(*s));
    if (s) s->output = output;
    return s;
}
static void sink_destroy(void *p) { free(p); }
static bool sink_start(void *p)
{
    struct sink *s = p;
    if (!obs_output_initialize_encoders(s->output, 0)) return false;
    uint8_t *headers;
    size_t header_size;
    if (!obs_encoder_get_extra_data(obs_output_get_video_encoder(s->output), &headers, &header_size) ||
        !headers || !header_size) return false;
    s->file = fopen(output_path, "wb");
    if (!s->file) return false;
    if (obs_output_begin_data_capture(s->output, 0)) return true;
    fclose(s->file);
    s->file = NULL;
    return false;
}
static void sink_stop(void *p, uint64_t ts)
{
    (void)ts;
    struct sink *s = p;
    obs_output_end_data_capture(s->output);
    if (s->file) fclose(s->file);
    s->file = NULL;
}
static void sink_packet(void *p, struct encoder_packet *packet)
{
    struct sink *s = p;
    if (!packet || !s->file) return;
    if (packet->size == 0 || packet->data[packet->size - 1] == 0) abort();
    for (size_t i = 4; i + 4 < packet->size; ++i) {
        const uint8_t *nal = packet->data + i;
        if (nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1 && nal[-1] == 0)
            abort();
    }
    if (packet->keyframe != (s->count%300==0) || packet->pts != packet->dts ||
        (s->count && packet->pts <= s->last_pts)) abort();
    s->last_pts = packet->pts;
    fwrite(packet->data,1,packet->size,s->file);
    s->count++; InterlockedIncrement(&packets);
}
static struct obs_output_info sink_info = {
    .id="vk264_smoke_sink", .flags=OBS_OUTPUT_VIDEO|OBS_OUTPUT_ENCODED,
    .get_name=sink_name, .create=sink_create, .destroy=sink_destroy,
    .start=sink_start, .stop=sink_stop, .encoded_packet=sink_packet,
    .encoded_video_codecs="h264",
};

/* UI contract: one field per active rate-control mode, plus a mode-specific
 * explanation and an effective-settings readout. */
static int check_ui(const char *mode, bool wantBitrate, bool wantQp,
                    const char *statusNeedle, const char *effectiveNeedle)
{
    obs_data_t *ui = obs_data_create();
    obs_data_set_string(ui, "rate_control", mode);
    obs_data_set_int(ui, "bitrate", 6000);
    obs_data_set_int(ui, "qp", 34);
    obs_data_set_int(ui, "keyint_sec", 5);
    obs_data_set_string(ui, "preset", "slow");
    obs_properties_t *props = obs_get_encoder_properties("vk264_h264");
    obs_properties_apply_settings(props, ui);
    const bool bitrateVisible = obs_property_visible(obs_properties_get(props, "bitrate"));
    const bool qpVisible = obs_property_visible(obs_properties_get(props, "qp"));
    const char *status = obs_property_description(obs_properties_get(props, "status"));
    const char *effective = obs_property_description(obs_properties_get(props, "effective"));
    const char *limits = obs_property_description(obs_properties_get(props, "limits"));
    printf("UI mode=%-11s bitrate=%d qp=%d\n  status: %s\n  effective: %s\n",
           mode, bitrateVisible, qpVisible, status, effective);
    int ok = 1;
    if (bitrateVisible != wantBitrate || qpVisible != wantQp) {
        fprintf(stderr, "UI visibility bug: mode=%s bitrate=%d qp=%d\n",
                mode, bitrateVisible, qpVisible);
        ok = 0;
    }
    if (!status || !strstr(status, statusNeedle) ||
        !effective || !strstr(effective, effectiveNeedle) ||
        !limits || !strstr(limits, "no profile, tune or x264-options fields")) {
        fprintf(stderr, "UI text bug: mode=%s\n", mode);
        ok = 0;
    }
    obs_properties_destroy(props);
    obs_data_release(ui);
    return ok;
}

static int check_validation(video_t *video)
{
    obs_data_t *bad = obs_data_create();
    obs_data_set_string(bad, "rate_control", "ABR");
    obs_data_set_int(bad, "bitrate", 10);
    obs_data_set_int(bad, "keyint_sec", 5);
    obs_encoder_t *enc = obs_video_encoder_create("vk264_h264", "vk264-bad", bad, NULL);
    if (!enc) { fprintf(stderr, "bad encoder not created\n"); return 0; }
    obs_encoder_set_video(enc, video);
    const char *saved = output_path;
    output_path = "invalid-settings.h264";
    obs_output_t *out = obs_output_create("vk264_smoke_sink", "bad-test", NULL, NULL);
    obs_output_set_video_encoder(out, enc);
    const bool started = obs_output_start(out);
    const char *error = obs_encoder_get_last_error(enc);
    printf("validation bitrate=10 rejected=%d error=%s\n", !started, error ? error : "(none)");
    int ok = !started && error && strstr(error, "Target bitrate must be 64-50000");
    if (started) obs_output_stop(out);
    obs_output_release(out);
    obs_encoder_release(enc);
    obs_data_release(bad);
    output_path = saved;
    return ok;
}

int main(int argc,char **argv)
{
    if (argc != 7 && argc != 8) {
        fprintf(stderr, "Usage: vk264_obs_smoke plugin.dll data-dir input.yuv output.h264 preset i420|nv12 [ABR|ABR-SMOOTH|CQP]\n");
        return 1;
    }
    bool nv12 = !strcmp(argv[6], "nv12");
    output_path = argv[4];
    if (!obs_startup("en-US", NULL, NULL)) return 1;
    printf("libobs %s\n", obs_get_version_string());
    obs_module_t *plugin = NULL;
    if (obs_open_module(&plugin, argv[1], argv[2]) || !obs_init_module(plugin)) return 2;
    const char *name = obs_encoder_get_display_name("vk264_h264");
    if (!name || strcmp(name, "Vk264:AVC")) return 3;
    obs_register_output(&sink_info);
    video_t *video = NULL;
    struct video_output_info vi = {
        .name="vk264-test", .format=nv12?VIDEO_FORMAT_NV12:VIDEO_FORMAT_I420,
        .fps_num=60, .fps_den=1, .width=1920, .height=1080,
        .cache_size=8, .colorspace=VIDEO_CS_709, .range=VIDEO_RANGE_PARTIAL,
    };
    if (video_output_open(&video,&vi)) return 4;
    if (!check_ui("ABR", true, false, "Average bitrate (ABR)", "target=6000 kbit/s")) return 10;
    if (!check_ui("ABR-SMOOTH", true, false, "Smoothed ABR", "ABR-SMOOTH")) return 11;
    if (!check_ui("CQP", false, true, "bitrate is ignored", "QP=34")) return 12;
    if (!check_validation(video)) return 13;
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "preset", argv[5]);
    obs_data_set_string(settings, "rate_control", argc == 8 ? argv[7] : "ABR");
    obs_encoder_t *enc = obs_video_encoder_create("vk264_h264","vk264-smoke",settings,NULL);
    if (!enc) return 5;
    obs_encoder_set_video(enc,video);
    obs_output_t *output = obs_output_create("vk264_smoke_sink", "native-test", NULL, NULL);
    if (!output) return 6;
    obs_output_set_video_encoder(output,enc);
    if (!obs_output_start(output)) {
        fprintf(stderr, "output_start failed: encoder=%s output=%s\n",
                obs_encoder_get_last_error(enc), obs_output_get_last_error(output));
        return 7;
    }
    FILE *ref = fopen(argv[3], "rb");
    if (!ref) return 8;
    const size_t pixels=1920u*1080u;
    uint8_t *raw=malloc(pixels*3/2);
    HANDLE timer=CreateWaitableTimerExW(NULL,NULL,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    LARGE_INTEGER frequency,start;
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    if (!raw || !timer) return 8;
    int submitted=0;
    for (int i=0;i<240;i++) {
        if (fread(raw, 1, pixels*3/2, ref) != pixels*3/2) return 8;
        struct video_frame frame;
        if (video_output_lock_frame(video,&frame,1,UINT64_C(1000000000)+(uint64_t)i*UINT64_C(1000000000)/60)) {
            for (int plane=0;plane<3;plane++) {
                int w=plane?960:1920, h=plane?540:1080;
                const uint8_t *src=raw+(plane?pixels+(plane-1)*pixels/4:0);
                if (nv12 && plane) continue;
                for (int y=0;y<h;y++) {
                    memcpy(frame.data[plane]+y*frame.linesize[plane],src+(size_t)y*w,w);
                }
            }
            if (nv12) for (int y=0;y<540;y++) for (int x=0;x<960;x++) {
                frame.data[1][y*frame.linesize[1]+2*x]=raw[pixels+(size_t)y*960+x];
                frame.data[1][y*frame.linesize[1]+2*x+1]=raw[pixels+pixels/4+(size_t)y*960+x];
            }
            video_output_unlock_frame(video);
            submitted++;
        }
        LARGE_INTEGER now;QueryPerformanceCounter(&now);
        double wait=(i+1)/60.0-(double)(now.QuadPart-start.QuadPart)/frequency.QuadPart;
        if (wait>0) {
            LARGE_INTEGER due;due.QuadPart=-(LONGLONG)(wait*10000000);
            SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE);WaitForSingleObject(timer,INFINITE);
        }
    }
    fclose(ref);
    free(raw);CloseHandle(timer);
    Sleep(500);
    obs_output_stop(output);
    Sleep(250);
    obs_output_release(output);
    obs_encoder_release(enc);
    obs_data_release(settings);
    video_output_close(video);
    obs_shutdown();
    int packet_count = InterlockedCompareExchange(&packets, 0, 0);
    printf("submitted=%d packets=%d\n",submitted,packet_count);
    return packet_count == submitted && packet_count == 240 ? 0 : 9;
}
