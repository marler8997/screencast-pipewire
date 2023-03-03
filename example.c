#include <glib-object.h>
#include <libportal/portal.h>

#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>

#include <pipewire/pipewire.h>

#define logf(fmt,...) do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)
#define errorf(fmt,...) logf("error: " fmt, ##__VA_ARGS__)
#define fatalf(fmt,...) do { errorf(fmt, ##__VA_ARGS__); exit(0xff); } while (0)

static struct {
    int no_formats;
    int no_pipewire_id;
    XdpPortal *portal;
    GMainLoop *main_loop;
    XdpSession *session;
    struct pw_thread_loop *pw_loop;
    struct pw_stream *stream;
} global;

static void on_stream_process(void *userdata)
{
    struct pw_buffer *b;
    struct spa_buffer *buf;

    if ((b = pw_stream_dequeue_buffer(global.stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if (buf->datas[0].data == NULL)
        return;

    logf("got a frame of size %d", buf->datas[0].chunk->size);

    pw_stream_queue_buffer(global.stream, b);
}

static void on_stream_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
    if (id == SPA_PARAM_Format) {
        if (param == NULL) {
            logf("video format unset");
            return;
        }

        struct spa_video_info format;
        if (spa_format_parse(param, &format.media_type, &format.media_subtype) < 0) {
            fatalf("failed to parse video format");
        }

        if (format.media_type != SPA_MEDIA_TYPE_video ||
            format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
            fatalf("expected raw video media type but got %d/%d", format.media_type, format.media_subtype);
        }
        if (spa_format_video_raw_parse(param, &format.info.raw) < 0) {
            fatalf("failed to parse raw video format");
        }

        logf("on_stram_param_changed: video format");
        logf("  format: %d (%s)", format.info.raw.format,
            spa_debug_type_find_name(spa_type_video_format, format.info.raw.format));
        logf("  size: %dx%d", format.info.raw.size.width, format.info.raw.size.height);
        logf("  framerate: %d/%d", format.info.raw.framerate.num, format.info.raw.framerate.denom);
    } else {
        logf("on_stream_param_changed id=%d", id);
    }
}

static const char *pw_stream_state_to_string(enum pw_stream_state state)
{
    switch (state) {
    case PW_STREAM_STATE_ERROR: return "error";
    case PW_STREAM_STATE_UNCONNECTED: return "unconnected";
    case PW_STREAM_STATE_CONNECTING: return "connecting";
    case PW_STREAM_STATE_PAUSED: return "paused";
    case PW_STREAM_STATE_STREAMING: return "streaming";
    default: return "?";
    }
}

static void on_stream_destroy(void *userdata)
{
    logf("on_stream_destroy");
}
static void on_stream_state_changed(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    logf("on_stream_state_changed %u (%s) > %u (%s) error='%s'",
         old, pw_stream_state_to_string(old),
         state, pw_stream_state_to_string(state), error ? error : "<null>");
}
static void on_stream_control_info(void *userdata, uint32_t id, const struct pw_stream_control *control)
{
    logf("on_stream_control_info id=%u", id);
}
static void on_stream_io_changed(void *userdata, uint32_t id, void *area, uint32_t size)
{
    logf("on_stream_io_changed id=%u size=%u", id, size);
}
static void on_stream_add_buffer(void *userdata, struct pw_buffer *buffer)
{
    logf("on_stream_add_buffer %p", (void*)buffer);
}
static void on_stream_remove_buffer(void *userdata, struct pw_buffer *buffer)
{
    logf("on_stream_remove_buffer %p", (void*)buffer);
}
static void on_stream_drained(void *userdata)
{
    logf("on_stream_drained");
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .destroy = on_stream_destroy,
    .state_changed = on_stream_state_changed,
    .control_info = on_stream_control_info,
    .io_changed = on_stream_io_changed,
    .param_changed = on_stream_param_changed,
    .add_buffer = on_stream_add_buffer,
    .remove_buffer = on_stream_remove_buffer,
    .process = on_stream_process,
    .drained = on_stream_drained,
};

void start_pipewire(guint32 node_id)
{
    const struct spa_pod *params[1];
    uint8_t param_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(param_buffer, sizeof(param_buffer));
    struct pw_properties *props;

    global.pw_loop = pw_thread_loop_new("Screecast Capture", NULL);

    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                              PW_KEY_MEDIA_CATEGORY, "Capture",
                              //PW_KEY_MEDIA_ROLE, "Camera",
                              NULL);
    global.stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(global.pw_loop),
        "video-capture",
        props,
        &stream_events,
        NULL);
    if (!global.stream) {
        fatalf("pw_stream_new failed, errno=%d", errno);
    }

    if (global.no_formats) {
        params[0] = spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            // provide a format no compositor will probably even suppor
            SPA_FORMAT_VIDEO_format,    SPA_POD_CHOICE_ENUM_Id(2,
                                            SPA_VIDEO_FORMAT_I420,
                                            SPA_VIDEO_FORMAT_I420),
            SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
                                            &SPA_RECTANGLE(320, 240),
                                            &SPA_RECTANGLE(1, 1),
                                            &SPA_RECTANGLE(4096, 4096)),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                                            &SPA_FRACTION(25, 1),
                                            &SPA_FRACTION(0, 1),
                                            &SPA_FRACTION(1000, 1)));
    } else {
        params[0] = spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,    SPA_POD_CHOICE_ENUM_Id(7,
                                            SPA_VIDEO_FORMAT_RGB,
                                            SPA_VIDEO_FORMAT_RGB,
                                            SPA_VIDEO_FORMAT_RGBA,
                                            SPA_VIDEO_FORMAT_RGBx,
                                            SPA_VIDEO_FORMAT_BGRx,
                                            SPA_VIDEO_FORMAT_YUY2,
                                            SPA_VIDEO_FORMAT_I420),
            SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
                                            &SPA_RECTANGLE(320, 240),
                                            &SPA_RECTANGLE(1, 1),
                                            &SPA_RECTANGLE(4096, 4096)),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                                            &SPA_FRACTION(25, 1),
                                            &SPA_FRACTION(0, 1),
                                            &SPA_FRACTION(1000, 1)));
    }
    pw_stream_connect(
        global.stream,
        PW_DIRECTION_INPUT,
        global.no_pipewire_id ? PW_ID_ANY : node_id,
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_DONT_RECONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1);

    logf("pipewire loop start");
    pw_thread_loop_start(global.pw_loop);
}

static void format_g_error(char *buf, size_t buf_len, GError *error)
{
    const char *domain_str = g_quark_to_string(error->domain);
    if (domain_str == NULL) {
        domain_str = "unknown";
    }
    snprintf(buf, buf_len, "%s (code=%d domain=%d '%s')",
        error->message ? error->message : "unknown error",
         error->code,
         error->domain, domain_str);
}

static void _enforce(const char *expression, const char *file, int line, const char *func)
{
    errorf("enforce failed '%s' (%s: %d function %s)", expression, file, line, func);
    abort();
}
#define enforce(expression) ((void)((expression) || (_enforce(#expression, __FILE__, __LINE__, __func__),0)))

#define SESSION_SOURCE_TYPE_SCREEN 1
#define SESSION_SOURCE_TYPE_WINDOW 2
#define SESSION_SOURCE_TYPE_VIRTUAL 4
static const char *source_type_str(uint32_t source_type)
{
    switch (source_type) {
    case SESSION_SOURCE_TYPE_SCREEN: return "screen";
    case SESSION_SOURCE_TYPE_WINDOW: return "window";
    case SESSION_SOURCE_TYPE_VIRTUAL: return "virtual";
    default: return "?";
    }
}

static void on_xdp_session_start(GObject *obj, GAsyncResult *result, void *user_data) {
    {
        GError *error = NULL;
        gboolean success = xdp_session_start_finish(global.session, result, &error);

        char error_buf[1024];
        if (error) {
            format_g_error(error_buf, sizeof(error_buf), error);
            g_clear_error(&error);
        } else {
            sprintf(error_buf, "none");
        }
        logf("screencast sesssion: started success=%d error: %s", success, error_buf);

        if (!success || error) {
            if (global.session != NULL) xdp_session_close(global.session);
            g_main_loop_quit(global.main_loop);
            return;
        }
    }

    unsigned stream_count = 0;
    guint32 last_pipewire_node_id;

    {
        GVariant *streams_variant = xdp_session_get_streams(global.session);
        if (streams_variant == NULL) {
            fatalf("xdp_session_get_streams returned null");
        }

        {
            const gchar *typestring = g_variant_get_type_string(streams_variant);
            if (0 != strcmp(typestring, "a(ua{sv})")) {
                fatalf("unexpected xdp session type '%s'", typestring);
            }
        }

        GVariantIter *streams_iter;
        g_variant_get(streams_variant, "a(ua{sv})", &streams_iter);

        GVariantIter *property_iter;
        while (g_variant_iter_loop(streams_iter, "(ua{sv})", &last_pipewire_node_id, &property_iter)) {
            logf("stream pipewire_node_id=%u", last_pipewire_node_id);
            stream_count++;

            gchar *key;
            GVariant *value;
            while (g_variant_iter_loop(property_iter, "{sv}", &key, &value)) {
                //TN_DEBUG("  key '{}' type='{}'", key, g_variant_get_type_string(value));
                if (0 == strcmp(key, "source_type")) {
                    enforce(0 == strcmp("u", g_variant_get_type_string(value)));
                    uint32_t s;
                    g_variant_get(value, "u", &s);
                    logf("  source_type=%u (%s)", s, source_type_str(s));
                    //source_type = s;
                } else if (0 == strcmp(key, "position")) {
                    enforce(0 == strcmp("(ii)", g_variant_get_type_string(value)));
                    int32_t x, y;
                    g_variant_get(value, "(ii)", &x, &y);
                    logf("  position %dx%d", x, y);
                    //position = XY<int32_t>{ .x = x, .y = y };
                } else if (0 == strcmp(key, "size")) {
                    enforce(0 == strcmp("(ii)", g_variant_get_type_string(value)));
                    int32_t x, y;
                    g_variant_get(value, "(ii)", &x, &y);
                    logf("  size %dx%d", x, y);
                    //size = XY<int32_t>{ .x = x, .y = y };
                } else if (0 == strcmp(key, "id")) {
                    enforce(0 == strcmp("s", g_variant_get_type_string(value)));
                    gchar *id;
                    g_variant_get(value, "s", &id);
                    // TODO: I don't need to free 'id' right?
                    logf("  stream id='%s'", id);
                } else {
                    errorf("unknown stream property '%s' type=%s", key, g_variant_get_type_string(value));
                }
            }
        }
        g_variant_iter_free(streams_iter);
        g_variant_unref(streams_variant);
    }

    if (stream_count == 0) {
        fatalf("xdp_session_get_streams returned no streams");
    }
    if (stream_count > 1) {
        fatalf("xdp_session_get_streams returned %u streams (expected 1)", stream_count);
    }

    start_pipewire(last_pipewire_node_id);
}

static void on_xdp_session_create(GObject *obj, GAsyncResult *result, void *user_data) {
    logf("xdp session create!");
    GError *error = NULL;
    global.session = xdp_portal_create_screencast_session_finish(global.portal, result, &error);
    if (error) {
        char g_error_buf[1024];
        format_g_error(g_error_buf, sizeof(g_error_buf), error);
        fatalf("failed to create xdp session with %s", g_error_buf);
    }

    logf("xdp screencast session created");
    xdp_session_start(
        global.session,
        NULL, // parent window
        NULL, // cancellable
        on_xdp_session_start,
        NULL);
}

int main(int argc, char *argv[])
{
    pw_init(&argc, &argv);

    argv++;argc--;
    global.no_formats = 0;
    global.no_pipewire_id = 0;
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        if ((0 == strcmp(opt, "-h")) || (0 == strcmp(opt, "--help"))) {
            fprintf(stderr,
                "Usage: ./example [--no-formats] [--no-id]\n"
                "--no-formats  Test what happens if we don't supply pipewire with any video formats\n"
                "--no-id       Disables passing the pipewire node id from Xdp\n"
            );
            exit(0xff);
        } else if (0 == strcmp(opt, "--no-formats")) {
            global.no_formats = 1;
        } else if (0 == strcmp(opt, "--no-id")) {
            global.no_pipewire_id = 1;
        } else {
            fatalf("unknown cmdline option '%s'", opt);
        }
    }
    logf("no formats=%d", global.no_formats);
    logf("no id=%d", global.no_pipewire_id);

    global.portal = xdp_portal_new();
    if (!global.portal) {
        fatalf("xdp_portal_new failed, errno=%d", errno);
    }
    global.main_loop = g_main_loop_new(NULL, FALSE);
    if (!global.main_loop) {
        fatalf("g_main_loop_new failed, errno=%d", errno);
    }

    XdpOutputType output_type = XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW;
#if LIBPORTAL_VERSION >= 5
    output_type |= XDP_OUTPUT_VIRTUAL;
#endif

    xdp_portal_create_screencast_session(
        global.portal,
        output_type,
        XDP_SCREENCAST_FLAG_NONE,
#if LIBPORTAL_VERSION >= 5
        XDP_CURSOR_MODE_HIDDEN,
        XDP_PERSIST_MODE_TRANSIENT,
        NULL, // restore token
#endif
        NULL, // cancellable
        on_xdp_session_create,
        NULL);

    logf("running main loop...");
    g_main_loop_run(global.main_loop);
    logf("main loop done");
    return 0;
}
