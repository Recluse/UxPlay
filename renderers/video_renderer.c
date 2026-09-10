/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <stdio.h>      /* PATCH: snprintf for rotation caps */
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/videooverlay.h>  /* PATCH (Plan A): GstVideoOverlay HWND */
#ifdef __APPLE__
#include <dispatch/dispatch.h>        /* PATCH (Plan B / M3): marshal NSView overlay bind to the main thread */
#include <pthread.h>                  /* pthread_main_np: run the bind inline when we already are the main thread */
#include <objc/objc.h>                /* BOOL / YES */
#include <objc/message.h>            /* objc_msgSend */
#include <objc/runtime.h>            /* sel_registerName */
#include <CoreGraphics/CoreGraphics.h> /* CGRect for the CALayer sublayer frame */
#endif
#include "video_renderer.h"

/* PATCH (Plan B): host-provided renderer HWND, defined in uxplay.cpp.  When the
 * DLL host has set one, we render into it via GstVideoOverlay instead of the
 * UXPLAY_OVERLAY_HWND env var.  Returns NULL for the standalone exe. */
extern void *airplay_get_host_window (void);

#ifdef __APPLE__
/* PATCH (Plan B): custom macOS sink — render decoded NV12 frames into OUR OWN
 * AVSampleBufferDisplayLayer (avsample_sink.m), bypassing GStreamer's buggy
 * applemedia sinks (avsamplebufferlayersink UAF on caps-change, osxvideosink
 * teardown deadlock).  Selected by videosink == "avlayer": the launch uses an
 * appsink, and this callback pumps each NV12 frame to the layer. */
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
extern void *avlayer_sink_create(void *nsview);
extern void  avlayer_sink_enqueue_nv12(void *sink, const unsigned char *y, unsigned long ys,
                                        const unsigned char *uv, unsigned long uvs, int w, int h);
extern void  avlayer_sink_destroy(void *sink);

static GstFlowReturn avlayer_on_new_sample(GstAppSink *appsink, gpointer user_data) {
    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    if (buf && caps && gst_video_info_from_caps(&info, caps)) {
        GstVideoFrame frame;
        if (gst_video_frame_map(&frame, &info, buf, GST_MAP_READ)) {
            avlayer_sink_enqueue_nv12(user_data,
                GST_VIDEO_FRAME_PLANE_DATA(&frame, 0), GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0),
                GST_VIDEO_FRAME_PLANE_DATA(&frame, 1), GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1),
                GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame));
            gst_video_frame_unmap(&frame);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
#endif

#define SECOND_IN_NSECS 1000000000UL
#define SECOND_IN_MICROSECS 1000000
#ifdef X_DISPLAY_FIX
#include <gst/video/navigation.h>
#include "x_display_fix.h"
static bool fullscreen = false;
static bool alt_keypress = false;
static unsigned char X11_search_attempts = 0;
#endif

static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* PATCH: now used to push caps on rotation */
static int last_rotation_hint = -1;  /* PATCH: track packet[5] separately -- both landscapes have same dims */
static bool first_packet = false;
static bool vsync_prop = false;  /* PATCH (Plan B / M3): renamed from "sync" -- it collided with libc sync() pulled in by <dispatch/dispatch.h> on macOS */
static bool auto_videosink = true;
static bool hls_video = false;
#ifdef X_DISPLAY_FIX
static bool use_x11 = false;
#endif
static bool logger_debug = false;
static gint64 hls_requested_start_position = 0;
static gint64 hls_seek_start = 0;
static gint64 hls_seek_end = 0;
static gint64 hls_duration = 0;
static gboolean hls_seek_enabled = FALSE;
static gboolean hls_playing = FALSE;
static gboolean hls_buffer_empty = FALSE;
static gboolean hls_buffer_full = FALSE;
static int type_264 = 0;
static int type_265 = 0;
static int type_hls = 0;
static int type_jpeg = 0;

typedef enum {
  //GST_PLAY_FLAG_VIDEO         = (1 << 0),
  //GST_PLAY_FLAG_AUDIO         = (1 << 1),
  GST_PLAY_FLAG_TEXT          = (1 << 2),
  //GST_PLAY_FLAG_VIS           = (1 << 3),
  //GST_PLAY_FLAG_SOFT_VOLUME   = (1 << 4),
  //GST_PLAY_FLAG_NATIVE_AUDIO  = (1 << 5),
  //GST_PLAY_FLAG_NATIVE_VIDEO  = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD      = (1 << 7),
  GST_PLAY_FLAG_BUFFERING     = (1 << 8),
  //GST_PLAY_FLAG_DEINTERLACE   = (1 << 9),
  //GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
  //GST_PLAY_FLAG_FORCE_FILTERS = (1 << 11),
  //GST_PLAY_FLAG_FORCE_SW_DECODERS = (1 << 12),
} GstPlayFlags;

#define NCODECS  3   /* renderers for h264,h265, and jpeg images */

struct video_renderer_s {
    GstElement *appsrc, *pipeline, *textsrc;
    GstElement *rotator; /* PATCH: dynamic videoflip for iPhone rotation tracking */
    GstBus *bus;
    const char *codec;
    bool autovideo;
    int id;
    char *uri;
    gboolean eos;
    gint64 duration;
    gint buffering_level;
#ifdef __APPLE__
    void *avlayer; /* PATCH (Plan B): custom AVSampleBufferDisplayLayer sink handle */
#endif
#ifdef  X_DISPLAY_FIX
    bool use_x11;
    const char * server_name;
    X11_Window_t * gst_window;
#endif
};

static video_renderer_t *renderer = NULL;
static video_renderer_t *renderer_type[NCODECS] = {0};
static int n_renderers = NCODECS;
static char h264[] = "h264";
static char h265[] = "h265";
static char hls[]  = "hls";
static char jpeg[] = "jpeg";

static void append_videoflip (GString *launch, const videoflip_t *flip, const videoflip_t *rot) {
    /* videoflip image transform */
    switch (*flip) {
    case INVERT:
        switch (*rot)  {
        case LEFT:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
	    break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        default:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_180 ! ");
	    break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_HORIZ ! ");
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_VERT ! ");
	  break;
	}
        break;
    default:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
            break;
        default:
            break;
        }
        break;
    }
}

/* apple uses colorimetry that is detected as  1:3:7:1           * //previously 1:3:5:1 was seen
 * (not recognized by v4l2 plugin in Gstreamer  < 1.20.4)        *
 * See .../gst-libs/gst/video/video-color.h in gst-plugins-base  *
 * range = 1   -> GST_VIDEO_COLOR_RANGE_0_255      ("full RGB")  * 
 * matrix = 3  -> GST_VIDEO_COLOR_MATRIX_BT709                   *
 * transfer = 7 -> GST_VIDEO_TRANSFER_SRGB                       * // previously GST_VIDEO_TRANSFER_BT709
 * primaries = 1 -> GST_VIDEO_COLOR_PRIMARIES_BT709              *
 * closest used by  GStreamer < 1.20.4 is BT709, 2:3:5:1 with    * // now use sRGB = 1:1:7:1    
 * range = 2 -> GST_VIDEO_COLOR_RANGE_16_235 ("limited RGB")     */  

static const char jpeg_caps[]="image/jpeg";
static const char h264_caps[]="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[]="video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

/* PATCH (rotation fix v6): push last_rotation_hint onto the live videoflip.
 * Split out of video_renderer_size() because the hint arrives BEFORE there is a
 * rotator to set it on: the type-1 codec packet calls video_report_size() ->
 * video_renderer_size() while the global `renderer` is still NULL (cleared by
 * video_renderer_start(), re-assigned only by video_renderer_choose_codec()).
 * So last_rotation_hint is DESIRED state and this runs from both places -- the
 * hint update, and the moment the renderer becomes valid.  Without the second
 * call site a session that starts in landscape stayed 90 degrees sideways until
 * the user physically rotated the phone, and stayed sideways forever across
 * reconnects (same hint == no change == never re-applied). */
static void apply_rotation_hint(void) {
    if (last_rotation_hint < 0 || !renderer || !renderer->rotator) {
        return;
    }
    int method;
    const char *label;
    /* packet[5] encoding (empirical):
     *   0x00 -> portrait (right-side-up)
     *   0x04 -> landscape rotated 90 degrees clockwise  (camera/volume buttons up)
     *   0x07 -> landscape rotated 90 degrees counter-clockwise (camera/volume buttons down)
     *   0x05 / 0x06 likely upside-down variants (not confirmed yet -- maps to 180) */
    switch (last_rotation_hint) {
        case 0x00: method = 0 /* IDENTITY */;        label = "portrait";          break;
        case 0x04: method = 1 /* 90R  */;            label = "landscape-right";   break;
        case 0x07: method = 3 /* 90L  */;            label = "landscape-left";    break;
        case 0x05: method = 2 /* 180  */;            label = "upside-down? (TBD)"; break;
        case 0x06: method = 2 /* 180  */;            label = "upside-down? (TBD)"; break;
        default:
            /* fall back to aspect heuristic, default to 90R for unknown landscape */
            method = (width > height) ? 1 : 0;
            label = "unknown rotation -- aspect fallback";
            break;
    }
    g_object_set(renderer->rotator, "video-direction", method, NULL);
    logger_log(logger, LOGGER_INFO,
               "apply_rotation_hint: %ux%u rot=0x%02x -> videoflip method=%d (%s)",
               (unsigned) width, (unsigned) height, last_rotation_hint, method, label);
}

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height, int rotation_hint) {
    unsigned short new_w = (unsigned short) *f_width;
    unsigned short new_h = (unsigned short) *f_height;
    bool dims_changed = (new_w != width) || (new_h != height);
    bool rot_changed = (rotation_hint != last_rotation_hint);
    width_source = (unsigned short) *f_width_source;
    height_source = (unsigned short) *f_height_source;
    width = new_w;
    height = new_h;
    last_rotation_hint = rotation_hint;
    /* PATCH (Plan B): INFO (was DEBUG) so the embedding host can read the video
     * size for window aspect-fit WITHOUT enabling debug logging (debug floods the
     * host log callback with per-frame DEBUG lines -> latency). This fires only
     * per stream-start / rotation, not per-frame. */
    logger_log(logger, LOGGER_INFO, "begin video stream wxh = %dx%d; source %dx%d (rot=0x%02x)",
               width, height, width_source, height_source, rotation_hint);

    /* PATCH (rotation fix v5): both landscape-right (0x04) and landscape-left (0x07) report
     * the SAME width/height in the codec packet (e.g. 1920x884 either way), so the old
     * "dims_changed" gate missed direct landscape<->landscape rotations and left the picture
     * upside-down.  Now also fires when packet[5] changed. */
    if (dims_changed || rot_changed) {
        apply_rotation_hint();   /* no-op while renderer is NULL; choose_codec re-applies */
    }
}

GstElement *make_video_sink(const char *videosink, const char *videosink_options) {
    /* used to build a videosink for playbin, using the user-specified string "videosink" */ 
    GstElement *video_sink = gst_element_factory_make(videosink, "videosink");
    if (!video_sink) {
        return NULL;
    }

    /* process the video_sink_options */
    size_t len = strlen(videosink_options);
    if (!len) {
        return video_sink;
    }

    char *options  = (char *) malloc(len + 1);
    strncpy(options, videosink_options, len + 1);

    /* remove any extension begining with "!" */
    char *end = strchr(options, '!');
    if (end) {   
      *end = '\0';
    }

    /* add any fullscreen options "property=pval" included in string videosink_options*/    
    /* OK to use strtok_r in Windows with MSYS2 (POSIX); use strtok_s for MSVC */
    char *token = NULL;
    char *text = options;

    while((token = strtok_r(text, " ", &text))) {
	char *pval = strchr(token, '=');
        if (pval) {
            *pval = '\0';
            pval++;
            const gchar *property_name = (const gchar *) token;
            const gchar *value = (const gchar *) pval;
            g_print("playbin_videosink property: \"%s\" \"%s\"\n", property_name, value);
            gst_util_set_object_arg(G_OBJECT (video_sink), property_name, value);
        }
    }
    free(options);
    return video_sink;
}

void video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser, const char * rtp_pipeline,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options, 
                          bool initial_fullscreen, bool video_sync, bool h265_support, bool coverart_support, guint playbin_version, const char *uri) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    bool rtp = (bool) strlen(rtp_pipeline);
    hls_video = (uri != NULL);
    /* videosink choices that are auto */
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));

    logger = render_logger;
    logger_debug = (logger_get_level(logger) >= LOGGER_DEBUG);
    hls_seek_enabled = FALSE;
    hls_playing = FALSE;
    hls_seek_start = -1;
    hls_seek_end = -1;
    hls_duration = -1;
    hls_buffer_empty = TRUE;
    hls_buffer_empty = FALSE;
    type_hls = -1;
    type_264 = -1;
    type_265 = -1;
    type_jpeg = -1;

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;
    n_renderers = 1;
    /* the renderer for hls video will only be built if a HLS uri is provided in 
     * the call to video_renderer_init, in which case the h264/h265 mirror-mode and jpeg
     * audio-mode renderers will not be built.   This is because it appears that we cannot  
     * put playbin into GST_STATE_READY before knowing the uri (?), so cannot use a
     * unified renderer structure with h264, h265, jpeg and hls  */  
    if (hls_video) {
        type_hls = 0;
    } else {
        type_264 = 0;
        if (h265_support) {
            type_265 = n_renderers++;
        }
        if (coverart_support) {
            type_jpeg = n_renderers++;
        }
    }
    g_assert (n_renderers <= NCODECS);
    for (int i = 0; i < n_renderers; i++) {
        g_assert (i < 3);
        renderer_type[i] = (video_renderer_t *) calloc(1, sizeof(video_renderer_t));
        g_assert(renderer_type[i]);
        renderer_type[i]->autovideo = auto_videosink;
        renderer_type[i]->id = i;
        renderer_type[i]->bus = NULL;
        renderer_type[i]->appsrc = NULL;
        renderer_type[i]->textsrc = NULL;
        renderer_type[i]->uri = NULL;
        renderer_type[i]->eos = FALSE;
        if (hls_video) {
            renderer_type[i]->uri = (char *) calloc(strlen(uri) + 1, sizeof(char));
            memcpy(renderer_type[i]->uri, uri, strlen(uri));
            /* use playbin3 to play HLS video: replace "playbin3" by "playbin" to use playbin2 */
            switch (playbin_version)  {
            case 2:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin", "hls-playbin2");
                break;
            case 3:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin3", "hls-playbin3");
                break;
            default:
                logger_log(logger, LOGGER_ERR, "video_renderer_init: invalid playbin version %u", playbin_version);
                g_assert(0);
            }
            logger_log(logger, LOGGER_INFO, "Will use GStreamer playbin version %u to play HLS streamed video", playbin_version);	    
            g_assert(renderer_type[i]->pipeline);
            renderer_type[i]->codec = hls;
            /* if we are not using an autovideosink, build a videosink based on the string "videosink" */
            if (!auto_videosink) {
#ifdef __APPLE__
            /* PATCH (Plan B / macOS): "avlayer" is NOT a registered GStreamer element
             * -- it is this fork's appsink -> AVSampleBufferDisplayLayer path, and it
             * was only ever wired into the MIRROR pipeline below.  Here
             * gst_element_factory_make("avlayer") returns NULL -- nothing in this
             * fork ever calls gst_element_register(), and no bundled plugin provides
             * that factory -- so playbin logged "failed to create
             * playbin_videosink" and fell back to its own
             * autovideosink, whose only candidate in this bundle is
             * avsamplebufferlayersink -- the framework sink this fork abandoned for
             * the UAF on caps-change, and one that renders into a layer of its own
             * rather than the host NSView.  Give playbin the same appsink bin the
             * mirror pipeline uses and bind it to the host NSView.
             * sync=true (NOT the mirror's sync=false): HLS is a timestamped VOD
             * stream, so the sink -- not the arrival rate -- is the clock. */
            if (!strcmp(videosink, "avlayer")) {
                GError *berr = NULL;
                void *hostview = airplay_get_host_window();
                GstElement *sinkbin = gst_parse_bin_from_description(
                    "videoconvert ! video/x-raw,format=NV12 ! appsink name=avlayer_hls"
                    " emit-signals=false sync=true max-buffers=3 drop=true", TRUE, &berr);
                GstElement *asink = sinkbin ?
                    gst_bin_get_by_name(GST_BIN(sinkbin), "avlayer_hls") : NULL;
                if (!hostview || !asink) {
                    logger_log(logger, LOGGER_ERR,
                               "video_renderer_init: no hls avlayer sink (%s) -- playbin picks its own",
                               berr ? berr->message : (hostview ? "appsink missing" : "no host window"));
                    if (sinkbin) gst_object_unref(sinkbin);
                } else {
                    renderer_type[i]->avlayer = avlayer_sink_create(hostview);
                    GstAppSinkCallbacks cbs;
                    memset(&cbs, 0, sizeof(cbs));
                    cbs.new_sample = avlayer_on_new_sample;
                    gst_app_sink_set_callbacks(GST_APP_SINK(asink), &cbs,
                                               renderer_type[i]->avlayer, NULL);
                    g_object_set(G_OBJECT (renderer_type[i]->pipeline), "video-sink", sinkbin, NULL);
                    logger_log(logger, LOGGER_INFO,
                               "avlayer: bound hls playbin appsink to host NSView %p%s", hostview,
                               renderer_type[i]->avlayer ? "" : " (LAYER CREATE FAILED)");
                }
                if (asink) gst_object_unref(asink);
                g_clear_error(&berr);
            } else
#endif
              {
                GstElement *playbin_videosink = make_video_sink(videosink, videosink_options);
                if (!playbin_videosink) {
                    logger_log(logger, LOGGER_ERR, "video_renderer_init: failed to create playbin_videosink");
                } else {
                    logger_log(logger, LOGGER_DEBUG, "video_renderer_init: create playbin_videosink at %p", playbin_videosink);
                    g_object_set(G_OBJECT (renderer_type[i]->pipeline), "video-sink", playbin_videosink, NULL);
                }
              }
            }
            gint flags = 0;
            g_object_get(renderer_type[i]->pipeline, "flags", &flags, NULL);
            flags &= ~GST_PLAY_FLAG_DOWNLOAD;   /* an on-disk cache buys a receiver nothing */
#ifdef __APPLE__
            /* PATCH (macOS): default BUFFERING and TEXT off.  Both were measured on
             * the macOS bundle only and, until 0783c70, applied to every platform;
             * on Windows that turned a 4/4 playing configuration into black windows
             * and freezes (2026-09-11, real iPhone), so they are Apple-only now.
             *   BUFFERING: with the pre-f700f24 zero-buffer settings the macOS
             *   picture froze on its first frame while the phone's clock ran on;
             *   the default preroll reserve played.  On Windows the opposite was
             *   measured: default buffering sat below 100% and never reached
             *   PLAYING, zero-buffer played.  Whether the difference is the sink
             *   (AVSampleBufferDisplayLayer vs d3d11) or the CDN of the day is not
             *   settled; each platform keeps what it was seen playing with.
             *   TEXT: the macOS bundle has no timed-text decoder, so a selected
             *   WebVTT rendition sits at "buffering 0%" forever with only a
             *   "Missing element" WARNING.  Windows and Linux ship subparse and
             *   render the captions; there TEXT stays on. */
            flags &= ~GST_PLAY_FLAG_TEXT;
            g_object_set(renderer_type[i]->pipeline, "flags", flags, NULL);
            logger_log(logger, LOGGER_INFO,
                       "playbin%u configured for HLS: flags=0x%x (buffering left at the default)",
                       playbin_version, flags);
#else
            /* PATCH (media-mode lag-reduction): no preroll reserve -- the settings
             * the Windows 2x2 re-test played 4/4 with (2026-09-10); see above for
             * why this is not applied on macOS. */
            flags &= ~GST_PLAY_FLAG_BUFFERING;
            g_object_set(renderer_type[i]->pipeline, "flags", flags, NULL);
            g_object_set(renderer_type[i]->pipeline,
                         "buffer-duration", (gint64) 0,
                         "buffer-size",     (gint)  0,
                         NULL);
            logger_log(logger, LOGGER_INFO,
                       "playbin%u configured for low-latency HLS: flags=0x%x buffer-duration=0 buffer-size=0",
                       playbin_version, flags);
#endif
            //g_object_set (G_OBJECT (renderer_type[i]->pipeline), "uri", uri, NULL);
        } else {
            bool jpeg_pipeline = false;
            if (i == type_264) {
                renderer_type[i]->codec = h264;
                caps = gst_caps_from_string(h264_caps);
            } else if (i == type_265) {
                renderer_type[i]->codec = h265;
                caps = gst_caps_from_string(h265_caps);
            } else if (i == type_jpeg) {
                jpeg_pipeline = true;
                renderer_type[i]->codec = jpeg;
                caps = gst_caps_from_string(jpeg_caps);
            } else {
                g_assert(0);
            }
            GString *launch = g_string_new("appsrc name=video_source ! ");
            if (jpeg_pipeline) {
                g_string_append(launch, "jpegdec ");
            } else {
                /* PATCH (lag-reduction B v3): leaky=downstream max-size-buffers=1 with
                 * sink-side qos=TRUE killed all frames (window never created).  Restored
                 * default queue.  Leaving plugin-rank env var as the only B win. */
                g_string_append(launch, "queue ! ");
                g_string_append(launch, parser);
                g_string_append(launch, " ! ");
                if (!rtp) {
                    g_string_append(launch, decoder);
                } else {
                    g_string_append(launch, "rtph264pay ");
                    g_string_append(launch, rtp_pipeline);
                }
            }
            if (!rtp || jpeg_pipeline) {
                g_string_append(launch, " ! ");
                append_videoflip(launch, &videoflip[0], &videoflip[1]);
                /* PATCH: always-on videoflip "rotator" so we can flip frames as the
                 * iPhone rotates -- mirror frames are always sent in native portrait
                 * orientation, the receiver is expected to rotate them per the codec
                 * packet's logical width/height. */
                if (!jpeg_pipeline) {
                    g_string_append(launch, "videoflip name=rotator video-direction=identity ! ");
                }
                g_string_append(launch, converter);
                g_string_append(launch, " ! ");
                /* PATCH (Plan B): xvimagesink scales inside the XVideo hardware overlay,
                 * so a CPU videoscale ahead of it is redundant work and was a measured
                 * cause of 4K stutter (pairs with skipping the RGB convert in
                 * uxplay.cpp). ximagesink is a software sink and still needs it. */
                if (!strstr(videosink, "xvimagesink")) {
                    g_string_append(launch, "videoscale ! ");
                }
                if (jpeg_pipeline) {
                    g_string_append(launch, " imagefreeze allow-replace=TRUE ! textoverlay name=metadata_overlay ! ");
                }
#ifdef __APPLE__
                /* PATCH (Plan B): "avlayer" -> custom AVSampleBufferDisplayLayer sink.
                 * We force NV12 into an appsink and pump frames to our own layer
                 * (see avlayer_on_new_sample / avsample_sink.m).  drop=true +
                 * max-buffers keep latency flat; sync=false renders ASAP. */
                if (!strcmp(videosink, "avlayer") && !jpeg_pipeline) {
                    g_string_append(launch, "videoconvert ! video/x-raw,format=NV12 ! appsink name=avlayer_");
                    g_string_append(launch, renderer_type[i]->codec);
                    g_string_append(launch, " emit-signals=false sync=false max-buffers=3 drop=true");
                    vsync_prop = false;
                } else {
#endif
                g_string_append(launch, videosink);
                g_string_append(launch, " name=");
                g_string_append(launch, videosink);
                g_string_append(launch, "_");
                g_string_append(launch, renderer_type[i]->codec);
                g_string_append(launch, videosink_options);
                if (video_sync && !jpeg_pipeline) {
                    g_string_append(launch, " sync=true");
                    vsync_prop = true;
                } else {
                    g_string_append(launch, " sync=false");
                    vsync_prop = false;
                }
#ifdef __APPLE__
                }
#endif
            }
            if (!strcmp(renderer_type[i]->codec, h264)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h265))){
                    pos +=3;
                    *pos = '4';
                }
            } else if (!strcmp(renderer_type[i]->codec, h265)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h264))){
                    pos +=3;
                    *pos = '5';
                }
            }

            logger_log(logger, LOGGER_DEBUG, "GStreamer video pipeline %d:\n\"%s\"", i + 1, launch->str);
            renderer_type[i]->pipeline = gst_parse_launch(launch->str, &error);
            if (error) {
                logger_log(logger, LOGGER_ERR, "GStreamer gst_parse_launch failed to create video pipeline %d\n"
                           "*** error message from gst_parse_launch was:\n%s\n"
                           "launch string parsed was \n[%s]", i + 1, error->message, launch->str);
                if (strstr(error->message, "no element")) {
                    logger_log(logger, LOGGER_ERR, "This error usually means that a uxplay option was mistyped\n"
                               "           or some requested part of GStreamer is not installed\n");
                }
                g_clear_error (&error);
            }
            g_assert (renderer_type[i]->pipeline);
            GstClock *clock = gst_system_clock_obtain();
            g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);
            gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);
            renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "video_source");
            g_assert(renderer_type[i]->appsrc);
            /* PATCH (lag-reduction B v2): do NOT set do-timestamp=TRUE -- uxplay sets PTS
             * manually from iPhone's NTP timestamp; appsrc auto-timestamping would overwrite
             * those, sink would then mark everything "late" via qos and drop all frames
             * (which made the window never appear).  Restored to original appsrc props. */
            g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
            /* PATCH: grab the rotator videoflip if it was inserted (h264/h265 pipelines) */
            renderer_type[i]->rotator = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "rotator");

            /* PATCH (Plan A — overlay HWND): if the launcher passed
             * UXPLAY_OVERLAY_HWND in the environment, the videosink is asked
             * to render into that pre-existing HWND via GstVideoOverlay rather
             * than create its own window.  Lets the launcher own the WndProc
             * and do native borderless / drag / resize / fullscreen / aspect. */
            {
                /* PATCH (Plan B): prefer the in-process host HWND set by the DLL
                 * host; fall back to UXPLAY_OVERLAY_HWND env for the exe. */
                void *host_hwnd = airplay_get_host_window();
                const char *hwnd_env = g_getenv("UXPLAY_OVERLAY_HWND");
                guintptr hwnd = 0;
                if (host_hwnd) {
                    hwnd = (guintptr) host_hwnd;
                } else if (hwnd_env && *hwnd_env) {
                    /* Accept both decimal and 0x-prefixed hex. */
                    hwnd = (guintptr) g_ascii_strtoull(hwnd_env, NULL,
                                                       (hwnd_env[0] == '0' && (hwnd_env[1] == 'x' || hwnd_env[1] == 'X')) ? 16 : 10);
                }
                if (hwnd) {
                    /* Videosink element was named "<videosink>_<codec>"
                     * in the launch string above (line 418-421). */
                    char sink_name[64];
                    g_snprintf(sink_name, sizeof(sink_name), "%s_%s",
                               videosink, renderer_type[i]->codec);
                    GstElement *sink = gst_bin_get_by_name(
                        GST_BIN(renderer_type[i]->pipeline), sink_name);
#ifdef __APPLE__
                    /* PATCH (Plan B / M3, macOS): two sink families need binding,
                     * both via AppKit calls that MUST run on the main thread (this
                     * code runs on the engine worker).  Inline when we already are
                     * the main thread, otherwise dispatch_ASYNC -- never sync: a
                     * host that quits mid-connect blocks its main thread inside
                     * airplay_core_stop() joining this very worker, so a sync hop
                     * would deadlock both (same rule as avlayer_sink_create()).
                     * The bind may therefore land a few ms after PLAYING; both sink
                     * families tolerate that (the frames until then are dropped).
                     *   * CALayer sinks (avsamplebufferlayersink) expose a read-only
                     *     "layer" CALayer -> attach it to our NSView; it auto-scales
                     *     on window resize (no GL framebuffer realloc, which is what
                     *     corrupted glimagesink on rotation).
                     *   * GstVideoOverlay sinks (glimagesink) -> set_window_handle.
                     * We do NOT call gst_video_overlay_handle_events(): on macOS tao
                     * owns the window's events (unlike the Win32 child-HWND case). */
                    if (sink) {
                        if (!strcmp(videosink, "avlayer")) {
                            /* PATCH (Plan B): custom sink — create our own
                             * AVSampleBufferDisplayLayer in the host NSView (the
                             * .m marshals to the main thread itself) and pump the
                             * appsink's NV12 frames to it.  No gst applemedia sink
                             * => no UAF / no teardown deadlock; clean resize. */
                            renderer_type[i]->avlayer = avlayer_sink_create((void *) hwnd);
                            GstAppSinkCallbacks cbs;
                            memset(&cbs, 0, sizeof(cbs));
                            cbs.new_sample = avlayer_on_new_sample;
                            gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs,
                                                       renderer_type[i]->avlayer, NULL);
                            logger_log(logger, LOGGER_INFO,
                                       "avlayer: bound appsink %s to host NSView 0x%" G_GINTPTR_MODIFIER "x%s",
                                       sink_name, (guintptr) hwnd,
                                       renderer_type[i]->avlayer ? "" : " (LAYER CREATE FAILED)");
                        } else {
                        gboolean has_layer =
                            (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "layer") != NULL);
                        if (has_layer || GST_IS_VIDEO_OVERLAY(sink)) {
                            /* the block below can outlive this scope (async), and
                             * `sink` is unreffed as soon as we return: hold our own. */
                            GstElement *sink_el = (GstElement *) gst_object_ref(sink);
                            guintptr ov_handle = hwnd;
                            /* ...and the host NSView too.  While this was a
                             * dispatch_sync the caller guaranteed the view was
                             * alive for the whole block; async, the block can run
                             * after airplay_core_stop() has already handed the
                             * window back and the tray dropped it.  (The ARC
                             * sibling in avsample_sink.m gets this for free: its
                             * `NSView *view` local is __strong, so Block_copy
                             * retains it.  This file is not ARC.) */
                            if (ov_handle) CFRetain((CFTypeRef) ov_handle);
                            /* avsamplebufferlayersink's "layer" is NULL until the sink
                             * is realized; bring it to READY on this worker thread so
                             * the AVSampleBufferDisplayLayer exists before we attach. */
                            gpointer av_layer = NULL;
                            if (has_layer) {
                                gst_element_set_state(sink_el, GST_STATE_READY);
                                gst_element_get_state(sink_el, NULL, NULL, GST_SECOND);
                                g_object_get(sink_el, "layer", &av_layer, NULL);
                            }
                            void (^bind)(void) = ^{
                                id view = (id) ov_handle;
                                if (has_layer) {
                                    /* CALayer sink (avsamplebufferlayersink): add the
                                     * sink's CALayer as a SUBLAYER of a layer-BACKED
                                     * view (AppKit owns + sizes the backing layer). */
                                    gpointer layer = av_layer;
                                    ((void (*)(id, SEL, BOOL)) objc_msgSend)(
                                        view, sel_registerName("setWantsLayer:"), YES);
                                    id backing = ((id (*)(id, SEL)) objc_msgSend)(
                                        view, sel_registerName("layer"));
                                    CGRect b = ((CGRect (*)(id, SEL)) objc_msgSend)(
                                        view, sel_registerName("bounds"));
                                    /* Black backing so any aspect letterbox area is
                                     * black (standard video look), not the desktop. */
                                    /* constant color: nothing to release (CGColorCreate*
                                     * here leaked one CGColor per bind) */
                                    ((void (*)(id, SEL, CGColorRef)) objc_msgSend)(
                                        backing, sel_registerName("setBackgroundColor:"),
                                        CGColorGetConstantColor(kCGColorBlack));
                                    if (layer) {
                                        const char *lcn = class_getName((Class)
                                            ((id (*)(id, SEL)) objc_msgSend)((id) layer, sel_registerName("class")));
                                        logger_log(logger, LOGGER_INFO,
                                                   "CALAYER-DIAG: layer=%p class=%s view.bounds=%.0fx%.0f backing=%p",
                                                   layer, lcn, b.size.width, b.size.height, backing);
                                        ((void (*)(id, SEL, CGRect)) objc_msgSend)(
                                            (id) layer, sel_registerName("setFrame:"), b);
                                        /* kCALayerWidthSizable(2) | kCALayerHeightSizable(16) */
                                        ((void (*)(id, SEL, unsigned int)) objc_msgSend)(
                                            (id) layer, sel_registerName("setAutoresizingMask:"), 18u);
                                        ((void (*)(id, SEL, id)) objc_msgSend)(
                                            backing, sel_registerName("addSublayer:"), (id) layer);
                                    } else {
                                        logger_log(logger, LOGGER_ERR, "CALAYER-DIAG: layer property was NULL");
                                    }
                                } else {
                                    ((void (*)(id, SEL, BOOL)) objc_msgSend)(
                                        view, sel_registerName("setWantsLayer:"), YES);
                                    gst_video_overlay_set_window_handle(
                                        GST_VIDEO_OVERLAY(sink_el), ov_handle);
                                }
                                logger_log(logger, LOGGER_INFO,
                                           "overlay: bound videosink to host NSView 0x%" G_GINTPTR_MODIFIER "x (%s)",
                                           ov_handle, has_layer ? "CALayer" : "overlay");
                                gst_object_unref(sink_el);
                                if (ov_handle) CFRelease((CFTypeRef) ov_handle);
                            };
                            if (pthread_main_np()) {
                                bind();
                            } else {
                                dispatch_async(dispatch_get_main_queue(), bind);
                            }
                        } else {
                            logger_log(logger, LOGGER_ERR,
                                       "overlay: %s has neither a layer nor GstVideoOverlay", sink_name);
                        }
                        } /* end: not the "avlayer" custom sink */
                    } else {
                        logger_log(logger, LOGGER_ERR, "overlay: %s element missing", sink_name);
                    }
#else
                    if (sink && GST_IS_VIDEO_OVERLAY(sink)) {
                        gst_video_overlay_set_window_handle(
                            GST_VIDEO_OVERLAY(sink), hwnd);
                        /* PATCH (Plan B): enable event handling so the sink
                         * forwards the GSTD3D11 child window's mouse messages to
                         * our HWND (cross-thread SendMessage -> our WndProc).
                         * Without this the host can't see clicks over the video. */
                        gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink), TRUE);
                        logger_log(logger, LOGGER_INFO,
                                   "overlay: bound videosink %s to host HWND 0x%" G_GINTPTR_MODIFIER "x (%s)",
                                   sink_name, (guintptr) hwnd,
                                   host_hwnd ? "in-process host" : "UXPLAY_OVERLAY_HWND env");
                    } else {
                        logger_log(logger, LOGGER_ERR,
                                   "overlay: %s element missing or no GstVideoOverlay support",
                                   sink_name);
                    }
#endif
                    if (sink) gst_object_unref(sink);
                } else if (hwnd_env && *hwnd_env) {
                    logger_log(logger, LOGGER_ERR,
                               "overlay: UXPLAY_OVERLAY_HWND='%s' parsed to 0", hwnd_env);
                }
                /* else: no host HWND and no env -> sink creates its own window
                 * (standalone exe default; also the B2 smoke-test NULL-hwnd case) */
            }
            g_string_free(launch, TRUE);
            gst_caps_unref(caps);
            gst_object_unref(clock);
            if (jpeg_pipeline) {
                 renderer_type[i]->textsrc = gst_bin_get_by_name(GST_BIN(renderer_type[i]->pipeline), "metadata_overlay");
                 g_object_set(G_OBJECT(renderer_type[i]->textsrc), "text", "", "shaded-background", TRUE, "font-desc", "Sans, 16",  NULL);
            }
        }	
#ifdef X_DISPLAY_FIX
        use_x11 = (strstr(videosink, "xvimagesink") || strstr(videosink, "ximagesink") || auto_videosink);
        fullscreen = initial_fullscreen;
        renderer_type[i]->server_name = server_name;
        renderer_type[i]->gst_window = NULL;
        renderer_type[i]->use_x11 = false;
        X11_search_attempts = 0;
        /* setting char *x11_display_name to NULL means the value is taken from $DISPLAY in the environment 
         * (a uxplay option to specify a different value is possible)  */
        char *x11_display_name = NULL;
        if (use_x11) {
            if (i == 0) {
                renderer_type[0]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[0]->gst_window);
                get_X11_Display(renderer_type[0]->gst_window, x11_display_name);
                if (renderer_type[0]->gst_window->display) {
                    renderer_type[0]->use_x11 = true;
                } else {
                    free(renderer_type[0]->gst_window);
                    renderer_type[0]->gst_window = NULL;
                }
            } else if (renderer_type[0]->use_x11) {
                renderer_type[i]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[i]->gst_window);
                memcpy(renderer_type[i]->gst_window, renderer_type[0]->gst_window, sizeof(X11_Window_t));
                renderer_type[i]->use_x11 = true;
            }
        }
#endif
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);	
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_READY);
        GstState state;
        GstStateChangeReturn ret = gst_element_get_state (renderer_type[i]->pipeline, &state, NULL, 100 * GST_MSECOND);
        if (ret == GST_STATE_CHANGE_SUCCESS) {
            logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer %d", i + 1);
            if (hls_video && i == 0) {
                renderer = renderer_type[i];
            }
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
            logger_log(logger, LOGGER_INFO, "\nPerhaps your GStreamer installation is missing some required plugins,"
                       "\nor your choices of video options (-vs -vd -vc -fs etc.) are incompatible on"
                       "\nthis computer architecture.  (An example: kmssink with fullscreen option -fs"
                       "\nmay work on some systems, but fail on others)");
            /* PATCH (Plan B, audit #5): was exit(1).  The engine runs inside the
               host tray process, so an unusable -vs / unavailable display must
               fail the START, not the application.  The slot is left non-READY:
               video_renderer_start() logs its state and video_renderer_choose_codec()
               then fails the PLAYING transition and returns -1 to the caller. */
        }
    }
}

void video_renderer_pause() {
    if (!renderer) {
        return;
    }
    GstStateChangeReturn ret = gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
    logger_log(logger, LOGGER_DEBUG, "video renderer pause: %s", gst_element_state_change_return_get_name(ret));
}

void video_renderer_resume() {
    if (!renderer) {
        return;
    }
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState state;
    /* wait with timeout 100 msec for pipeline to change state from PAUSED to PLAYING */
    gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
    const gchar *state_name = gst_element_state_get_name(state);
    logger_log(logger, LOGGER_DEBUG, "video renderer resumed: state %s", state_name);
    if (renderer->appsrc) {
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
}

void video_renderer_start() {
    GstState state;
    const gchar *state_name = NULL;
    /* PATCH (rotation fix v6): these statics survive a disconnect, so without a
     * reset a reconnect at the SAME orientation and size sees both "changed"
     * tests false and never re-applies the rotation to the freshly built
     * videoflip (which always launches at video-direction=identity).  Clearing
     * them here makes every session re-derive its orientation from scratch. */
    last_rotation_hint = -1;
    width = height = width_source = height_source = 0;
    if (hls_video) {
        if (!renderer) {
            /* init left the playbin non-READY (it no longer exit()s on that) */
            logger_log(logger, LOGGER_ERR, "video renderer_start: no usable hls pipeline");
            return;
        }
        g_object_set (G_OBJECT (renderer->pipeline), "uri", renderer->uri, NULL);
        gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
	gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
	state_name = gst_element_state_get_name(state);
	logger_log(logger, LOGGER_DEBUG, "video renderer_start: state %s", state_name);
        return;
    } 
    /* when not hls, start both h264 and h265 pipelines; will shut down the "wrong" one when we know the codec */
    for (int i = 0; i < n_renderers; i++) {
        if (!renderer_type[i]) continue;  /* PATCH (Plan B): never deref a NULL slot */
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_PAUSED);
        gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 1000 * GST_MSECOND);
        state_name = gst_element_state_get_name(state);
        logger_log(logger, LOGGER_DEBUG, "video renderer_start: renderer %d %p state %s", i, renderer_type[i], state_name);
    }
    renderer = NULL;
    first_packet = true;
#ifdef X_DISPLAY_FIX
    X11_search_attempts = 0;
#endif
}

/* used to find any X11 Window used by the playbin (HLS) pipeline after it starts playing. 
*  if use_x11 is true, called every 100 ms after playbin state is READY until the x11 window is found*/
bool waiting_for_x11_window() {
    if (!hls_video || !renderer) {
        /* same NULL as video_renderer_eos_watch(): its 100 ms timer is armed on
           the same branch of uxplay.cpp, and a non-READY hls playbin leaves
           `renderer` NULL instead of exiting. */
        return false;
    }
#ifdef X_DISPLAY_FIX
    if (use_x11 && renderer->gst_window) {
        get_x_window(renderer->gst_window, renderer->server_name);
        if (!renderer->gst_window->window) {
	    return true;    /* window still not found */
        }
    }
    if (fullscreen) {
         set_fullscreen(renderer->gst_window, &fullscreen);
    }
#endif
    return false;
}

/* use this to cycle the jpeg renderer to remove expired coverart when no new coverart has replaced it */
int video_renderer_cycle() {
    if (!renderer || !strstr(renderer->codec, jpeg)) {
        return -1;
    }
    GstState state, pending_state, target_state;
    GstStateChangeReturn ret;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    logger_log(logger, LOGGER_DEBUG, "renderer_cycle renderer %p: initial pipeline state is %s", renderer,
               gst_element_state_get_name(state));

    for (int i = 0 ; i < 2; i++) {
        int count = 0;
        if (i == 0 ) {
            target_state = GST_STATE_NULL;
            video_renderer_stop();
        } else {
            target_state = GST_STATE_PLAYING;
            gst_element_set_state (renderer->pipeline, target_state);
        }
        while (state != target_state) {
            ret = gst_element_get_state(renderer->pipeline, &state, &pending_state, 1000 * GST_MSECOND);
            if (ret == GST_STATE_CHANGE_SUCCESS) {
                logger_log(logger, LOGGER_DEBUG, "current pipeline state is %s", gst_element_state_get_name(state));
                if (pending_state != GST_STATE_VOID_PENDING) {
                    logger_log(logger, LOGGER_DEBUG, "pending pipeline state is %s", gst_element_state_get_name(pending_state));
                }
            } else if (ret == GST_STATE_CHANGE_FAILURE) {
                logger_log(logger, LOGGER_ERR, "pipeline %s: state change to %s failed", renderer->codec, gst_element_state_get_name(target_state));
                count++;
                if (count > 10) {
                    return -1;
                }
            } else if (ret == GST_STATE_CHANGE_ASYNC) {
                logger_log(logger, LOGGER_DEBUG, "state change to %s is asynchronous, waiting for completion ...",
                           gst_element_state_get_name(target_state));
            }
        }
    }
    return 0;
}

void video_renderer_display_jpeg(const void *data, int *data_len) {
    GstBuffer *buffer = NULL;
    if (type_jpeg == -1) {
        return;
    }
    if (renderer && !strcmp(renderer->codec, jpeg)) {
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
	g_assert(buffer != NULL);
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
    }  
}

uint64_t video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (vsync_prop) {
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            // adjust timestamps to be >= gst_video_pipeline_base time
            logger_log(logger, LOGGER_DEBUG, "*** invalid ntp_time < gst_video_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            return  (uint64_t)  gst_video_pipeline_base_time - pts;
        }
    }
    g_assert(data_len != 0);
    /* first four bytes of valid  h264  video data are 0x00, 0x00, 0x00, 0x01.    *
     * nal_count is the number of NAL units in the data: short SPS, PPS, SEI NALs *
     * may  precede a VCL NAL. Each NAL starts with 0x00 0x00 0x00 0x01 and is    *
     * byte-aligned: the first byte of invalid data (decryption failed) is 0x01   */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, "Begin streaming to GStreamer video pipeline");
            first_packet = false;
        }
        if (!renderer || !(renderer->appsrc)) {
            logger_log(logger, LOGGER_DEBUG, "*** no video renderer found");
            return 0;
        }
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        //g_print("video latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
        if (vsync_prop) {
            GST_BUFFER_PTS(buffer) = pts;
        }
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window && !(renderer->gst_window->window) && renderer->use_x11) {
            X11_search_attempts++;
            logger_log(logger, LOGGER_DEBUG, "Looking for X11 UxPlay Window, attempt %d", (int) X11_search_attempts);
            get_x_window(renderer->gst_window, renderer->server_name);
            if (renderer->gst_window->window) {
                logger_log(logger, LOGGER_INFO, "\n*** X11 Windows: Use key F11 or (left Alt)+Enter to toggle full-screen mode\n");
                if (fullscreen) {
                    set_fullscreen(renderer->gst_window, &fullscreen);
                }
            }
        }
#endif
    }
    return 0;
}

void video_renderer_flush() {
}

void video_renderer_hls_ready() {
    GstState state;
    GstStateChangeReturn ret;
    if (renderer && hls_video) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_hls_ready");
        ret = gst_element_set_state (renderer->pipeline, GST_STATE_READY);
        logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                   gst_element_state_change_return_get_name(ret));
        gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
    }
}

void video_renderer_stop() {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_stop");
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        //gst_element_set_state (renderer->playbin, GST_STATE_NULL);
     }
}

void video_renderer_set_device_model(const char *model, const char *name) {
    // Device frame not supported in GStreamer renderer
    (void)model;
    (void)name;
}

void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album) {
    // Track metadata display superimposed on coverart is now supported in GStreamer renderer
    GString *metadata = g_string_new("");
    if (artist) {
        g_string_append(metadata, artist);
    }
    if (artist && title) {
        g_string_append(metadata, ": ");
    }
    if (title) {
        g_string_append(metadata, "\"");
        g_string_append(metadata, title);
        g_string_append(metadata, "\"");
    }
    
    g_string_replace (metadata, "&", "&amp;", 0);   //fix pango problem with "&" in text
    if (renderer && renderer->textsrc && (artist || title)) {
        g_object_set(G_OBJECT(renderer->textsrc), "text", metadata->str, NULL);
    }
    g_string_free(metadata, TRUE);
}

static void video_renderer_destroy_instance(video_renderer_t *renderer) {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"destroying renderer instance %p codec=%s ", renderer, renderer->codec);
        GstState state;
        GstStateChangeReturn ret;
        gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        if (state != GST_STATE_NULL) {
            if (!hls_video) {
                gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
            }
            ret = gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
            logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                       gst_element_state_change_return_get_name(ret));
            gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
            logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        }
        if (renderer->appsrc) {
            gst_object_unref (renderer->appsrc);
            renderer->appsrc = NULL;
        }
        if (renderer->textsrc) {
            gst_object_unref (renderer->textsrc);
            renderer->textsrc = NULL;
        }
        /* PATCH: rotator is transfer-full from gst_bin_get_by_name(); dropping it
         * with the struct leaked one videoflip (plus its pads/caps) per mirror
         * reconnect, since uxplay.cpp rebuilds the renderers each time. */
        if (renderer->rotator) {
            gst_object_unref (renderer->rotator);
            renderer->rotator = NULL;
        }
        gst_object_unref(renderer->bus);
        gst_object_unref(renderer->pipeline);
#ifdef __APPLE__
        /* Tear down our AVSampleBufferDisplayLayer AFTER the pipeline is NULL, so
         * no appsink new-sample callback can still be pumping frames into it. */
        if (renderer->avlayer) {
            avlayer_sink_destroy(renderer->avlayer);
            renderer->avlayer = NULL;
        }
#endif
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window){
	  // free_X11_Display(renderer->gst_window);   without this, a memory leak; with it, a coredump
            free(renderer->gst_window);
            renderer->gst_window = NULL;
        }
#endif
        if (renderer->uri) {
            free(renderer->uri);
        }
        free (renderer);
        renderer = NULL;
        logger_log(logger, LOGGER_DEBUG,"renderer destroyed\n");	
    }
}

void video_renderer_destroy() {
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i]) {
            video_renderer_destroy_instance(renderer_type[i]);
            renderer_type[i] = NULL;  /* PATCH (Plan B): no dangling freed slot / double-free */
        }
    }
}

static void get_stream_status_name(GstStreamStatusType type, char *name, size_t len) {
    switch (type) {
    case GST_STREAM_STATUS_TYPE_CREATE:
        strncpy(name, "CREATE", len);
        return;
    case GST_STREAM_STATUS_TYPE_ENTER:
        strncpy(name, "ENTER", len);
        return;
    case GST_STREAM_STATUS_TYPE_LEAVE:
        strncpy(name, "LEAVE", len);
        return;
    case GST_STREAM_STATUS_TYPE_DESTROY:
        strncpy(name, "DESTROY", len);
        return;
    case GST_STREAM_STATUS_TYPE_START:
        strncpy(name, "START", len);
        return;
    case GST_STREAM_STATUS_TYPE_PAUSE:
        strncpy(name, "PAUSE", len);
        return;
    case GST_STREAM_STATUS_TYPE_STOP:
        strncpy(name, "STOP", len);
        return;
    default:
        strncpy(name, "", len);
        return;
    }
}

static void hls_video_seek_to_start_position(GstElement *pipeline) {
    if (hls_requested_start_position && hls_seek_enabled && hls_requested_start_position >= hls_seek_start
        && hls_requested_start_position  <= hls_seek_end) {
        g_print("***************** seek to hls_requested_start_position %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(hls_requested_start_position));
        if (gst_element_seek_simple (pipeline, GST_FORMAT_TIME,
				 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, hls_requested_start_position)) {
            hls_requested_start_position = 0;
        } else {
            g_print("*** seek to requested_start_position failed\n"); 
        }
    } 
}

static gboolean gstreamer_video_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    GstState old_state, new_state;
    const gchar no_state[] = "";
    const gchar *old_state_name = no_state, *new_state_name = no_state;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED) {
        GstState old_state, new_state;
        gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
        old_state_name = gst_element_state_get_name (old_state);
        new_state_name = gst_element_state_get_name (new_state);
    }

    /* identify which pipeline sent the message */ 
    int type = -1;
    for (int i = 0 ; i < n_renderers ; i ++ ) {
        if (renderer_type[i] && renderer_type[i]->bus == bus) {
            type = i;
            break;
        }
    }

    /* if the bus sending the message is not found, the renderer may already have been destroyed */
    if (type == -1) {
        if (logger_debug) {
            g_print("GStreamer(UNKNOWN, now destroyed?) bus message: %s %s %s %s\n",
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }     
        return TRUE;
    }

    video_renderer_t *renderer = renderer_type[type];

    gint64 pos = -1;
    if (hls_video) {
        gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos);
        if (hls_requested_start_position && pos >= hls_requested_start_position) {
            hls_requested_start_position = 0;
        }
    }

    if (logger_debug) {
        gchar *name = NULL;
        GstElement *element = NULL;
        gchar type_name[8] = { 0 };
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_STATUS) {
            GstStreamStatusType type;
            gst_message_parse_stream_status(message, &type, &element);
            name = gst_element_get_name(element);
            get_stream_status_name(type, type_name, 8);
            old_state_name = name;
            new_state_name = type_name;
        }
        if (GST_CLOCK_TIME_IS_VALID(pos)) {
            g_print("GStreamer %s  bus message %s %s %s %s; position: %" GST_TIME_FORMAT "\n" ,renderer->codec,
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name, GST_TIME_ARGS(pos));
        } else {
            g_print("GStreamer %s bus message %s %s %s %s\n", renderer->codec,
                    GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }
        if (name) {
            g_free(name);
        }
    }

    if (hls_video && !GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration);
    }

    if (hls_requested_start_position && hls_seek_enabled) {
        hls_video_seek_to_start_position(renderer->pipeline);
    }

    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_DURATION:
        hls_duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_BUFFERING:
        if (hls_video) {
            gint percent = -1;
            gst_message_parse_buffering(message, &percent);
            hls_buffer_empty = TRUE;
            hls_buffer_full = FALSE;
            if (percent > 0) {
                hls_buffer_empty = FALSE;
                renderer->buffering_level = percent;
                logger_log(logger, LOGGER_DEBUG, "Buffering :%d percent done", percent);
                if (percent < 100) {
                    gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
                } else {
                    hls_buffer_full = TRUE;
                    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
                }
            }
        }
	break;      
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gboolean flushing = FALSE;
        gboolean closed_window = FALSE;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error (video): %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        if (strstr(err->message, "Output window was closed")) {
            closed_window = TRUE;
        }
        if (!hls_video && strstr(err->message,"Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                     "*** This is a generic GStreamer error that usually means that GStreamer\n"
                     "*** was unable to construct a working video pipeline.\n\n"
                     "*** If you are letting the default autovideosink select the videosink,\n"
                     "*** GStreamer may be trying to use non-functional hardware h264 video decoding.\n"
                     "*** Try using option -avdec to force software decoding or use -vs <videosink>\n"
                     "*** to select a videosink of your choice (see \"man uxplay\").\n\n"
                     "*** Raspberry Pi models 4B and earlier using Video4Linux2 may need \"-bt709\" uxplay option");
        }
        g_error_free (err);
        g_free (debug);
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        if (!hls_video || closed_window) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            g_main_loop_quit( (GMainLoop *) loop);
        }
        break;
    }
    case GST_MESSAGE_EOS:
        /* end-of-stream */
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream (video)");
        if (hls_video) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            renderer->eos = TRUE;
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (hls_video && strstr(GST_MESSAGE_SRC_NAME(message), "hls-playbin")) {
            GstState old_state, new_state;
            gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
            if (logger_debug) {
            g_print ("****** hls_playbin: Element %s changed state from %s to %s.\n", GST_OBJECT_NAME (message->src),
                     gst_element_state_get_name (old_state),
                     gst_element_state_get_name (new_state));
            } 
            if (new_state != GST_STATE_PLAYING) {
                hls_playing = FALSE;
                break;
            }
#ifdef __APPLE__
            if (!hls_playing) {
                /* PATCH (Plan B / macOS host): the host shows + fits its mirror
                 * window on the "Begin streaming" marker, which video_renderer_render_buffer()
                 * emits on the first MIRROR packet -- a code path the HLS video
                 * protocol never touches, so the picture landed in a window that
                 * was still hidden.  playbin reaching PLAYING is the HLS equivalent
                 * of that first packet.  (No wxh line: playbin knows the size, the
                 * layer letterboxes into whatever window size the host chose.)
                 * Apple-only: the HLS sink is bound into the host view only under
                 * __APPLE__ (video_renderer_init).  Elsewhere playbin renders into
                 * the sink's own window, and this marker made the Windows and Linux
                 * hosts raise an EMPTY fullscreen window over it (0.2.14 Linux,
                 * Windows re-test 2026-09-11). */
                logger_log(logger, LOGGER_INFO, "Begin streaming HLS video to GStreamer playbin");
            }
#endif
            hls_playing = TRUE;
            GstQuery *query = NULL;
            query = gst_query_new_seeking(GST_FORMAT_TIME);
                if (gst_element_query(renderer->pipeline, query)) {
	        gst_query_parse_seeking (query, NULL, &hls_seek_enabled, &hls_seek_start, &hls_seek_end);
                if (hls_seek_enabled) {
                    g_print ("Seeking is ENABLED from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n",
			     GST_TIME_ARGS (hls_seek_start), GST_TIME_ARGS (hls_seek_end));
                } else {
                    g_print ("Seeking is DISABLED for this stream.\n");
                }
            } else {
                g_printerr ("Seeking query failed.");
            }
            gst_query_unref (query);

            if (hls_requested_start_position && hls_seek_enabled) {
                hls_video_seek_to_start_position(renderer->pipeline);
            }

        }
        if (renderer->autovideo) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
                if (strstr(GST_MESSAGE_SRC_NAME(message), renderer->codec)) {
                    logger_log(logger, LOGGER_DEBUG, "GStreamer: automatically-selected videosink"
                               " (renderer %d: %s) is \"%ssink\"", renderer->id + 1,
                               renderer->codec, sink);
#ifdef X_DISPLAY_FIX
                    renderer->use_x11 = (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
                    renderer->autovideo = false;
                }
            }
        }
        break;
#ifdef  X_DISPLAY_FIX
    case GST_MESSAGE_ELEMENT:
        if (renderer->gst_window && renderer->gst_window->window) {
            GstNavigationMessageType message_type = gst_navigation_message_get_type (message);
            if (message_type == GST_NAVIGATION_MESSAGE_EVENT) {
                GstEvent *event = NULL;
                if (gst_navigation_message_parse_event (message, &event)) {
                    GstNavigationEventType event_type = gst_navigation_event_get_type (event);
                    const gchar *key = NULL;
                    switch (event_type) {
                    case GST_NAVIGATION_EVENT_KEY_PRESS:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if ((strcmp (key, "F11") == 0) || (alt_keypress && strcmp (key, "Return") == 0)) {
                                fullscreen = !(fullscreen);
                                set_fullscreen(renderer->gst_window, &fullscreen);
                            } else if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = true;
                            }
                        }
                        break;
                    case GST_NAVIGATION_EVENT_KEY_RELEASE:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = false;
                            }
                        }
                    default:
                        break;
                    }
                }
                if (event) {
                    gst_event_unref (event);
                }
            }
        }
        break;
#endif
    default:
      /* unhandled message */
        break;
    }
    return TRUE;
}

int video_renderer_choose_codec (bool video_is_jpeg, bool video_is_h265) {
    video_renderer_t *renderer_used = NULL;
    g_assert(!hls_video);
    if (video_is_jpeg) {
        g_assert(type_jpeg != -1);
        renderer_used = renderer_type[type_jpeg];
    } else {
        if (video_is_h265) {
            if (type_265 == -1) {
                logger_log(logger, LOGGER_ERR, "video is h265 but the -h265 option was not used");
                return -1;
            }
            renderer_used = renderer_type[type_265];
        } else {
            g_assert(type_264 != -1);
            renderer_used = renderer_type[type_264];
        }
    }
    if (renderer_used == NULL) {
        return -1;
    } else if (renderer_used == renderer) {
        return 0;
    } else if (renderer) {
        return -1;
    }
    renderer = renderer_used;
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState old_state, new_state;
    if (gst_element_get_state(renderer->pipeline, &old_state, &new_state, 100 * GST_MSECOND) == GST_STATE_CHANGE_FAILURE) {
        /* PATCH (Plan B, audit #5): was g_error(), i.e. G_LOG_LEVEL_ERROR, which is
           ALWAYS fatal -- it abort()ed the host tray process and made the return
           below dead code.  Drop back to "no renderer" instead: the caller already
           treats -1 as "this codec cannot be played". */
        logger_log(logger, LOGGER_ERR, "video pipeline failed to go into playing state");
        renderer = NULL;
        return -1;
    }
    logger_log(logger, LOGGER_DEBUG, "video_pipeline state change from %s to %s\n",
               gst_element_state_get_name (old_state),gst_element_state_get_name (new_state));
    gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    if (strstr(renderer->codec, h265)) {
        logger_log(logger, LOGGER_INFO, "*** video format is h265 high definition (HD/4K) video %dx%d", width, height);
    }
    /* destroy unused renderers */
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i] == renderer) {
            continue;
        }
	if (renderer_type[i]) {
            video_renderer_t *renderer_unused = renderer_type[i];
            renderer_type[i] = NULL;
            video_renderer_destroy_instance(renderer_unused);
        }
    }
    /* PATCH (rotation fix v6): `renderer` only just became non-NULL, so this is the
     * first moment the orientation reported by the codec packet (video_renderer_size,
     * which ran earlier) can actually reach a videoflip.  A landscape-first connect
     * rendered sideways for the whole session without this. */
    apply_rotation_hint();
    return 0;
}
    

bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    gint64 pos = 0;
    GstState state;
    *duration = 0.0;
    *position = -1.0;
    *seek_start = 0.0;
    *seek_duration = 0.0;
    *rate = 0.0f;
    if (!renderer) {
        return true;
    }

    if (hls_seek_enabled) {
        *seek_start = ((double) hls_seek_start) / GST_SECOND;
        *seek_duration = ((double) (hls_seek_end - hls_seek_start)) / GST_SECOND;     
    }

    *buffer_empty = (bool) hls_buffer_empty;
    *buffer_full = (bool) hls_buffer_full;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    *rate = 0.0f;
    switch (state) {
    case GST_STATE_PLAYING:
        *rate = 1.0f;
    default:
        break;
    }

    if (!GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        if (!gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration)) {
            return true;
        }
    }
    *duration = ((double) hls_duration) / GST_SECOND;
    if (*duration) {
        if (gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos) &&
                                        GST_CLOCK_TIME_IS_VALID(pos)) {
            *position = ((double) pos) / GST_SECOND;
        }
    }

    logger_log(logger, LOGGER_DEBUG, "******* video_get_playback_info: position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s rate %f *****",
               GST_TIME_ARGS (pos), GST_TIME_ARGS (hls_duration), gst_element_state_get_name(state), *rate);

    return true;
}

void video_renderer_set_start(float position) {
    int pos_in_micros = (int) (position * SECOND_IN_MICROSECS);
    hls_requested_start_position = (gint64) (pos_in_micros * GST_USECOND);
    logger_log(logger, LOGGER_DEBUG, "register HLS video start position %f %lld", position,
               hls_requested_start_position);    
}

void video_renderer_seek(float position) {
    int pos_in_micros = (int) (position * SECOND_IN_MICROSECS);
    gint64 seek_position = (gint64) (pos_in_micros * GST_USECOND);
    /* don't seek to within 1  microsecond  of beginning or end of video */
    if (hls_duration < 2000) return;
    seek_position =  seek_position < 1000 ? 1000 : seek_position;
    seek_position =  seek_position > hls_duration  - 1000 ? hls_duration - 1000 : seek_position;
    g_print("SCRUB: seek to %f secs =  %" GST_TIME_FORMAT ", duration = %" GST_TIME_FORMAT "\n", position,
            GST_TIME_ARGS(seek_position),  GST_TIME_ARGS(hls_duration));
    gboolean result = gst_element_seek_simple(renderer->pipeline, GST_FORMAT_TIME,
                                              (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                              seek_position);
    if (result) {
        g_print("seek succeeded\n");
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);	
    } else {
        g_print("seek failed\n");
    }
}

unsigned int video_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < n_renderers);
    /* PATCH (Plan B): after a connection reaches PLAYING, video_renderer_start()
     * destroys the UNUSED renderers and sets renderer_type[i] = NULL.  On a
     * reconnect that does NOT re-init the renderers (e.g. -nc / no full reset),
     * main_loop() still calls listen() for every slot 0..n_renderers, so the
     * NULLed slots used to trip g_assert and abort the whole process (observed as
     * a crash on the 2nd/3rd device disconnect on macOS).  Skip absent renderers
     * instead: main_loop() already treats a 0 watch-id as "no watch". */
    if (!renderer_type[id] || !renderer_type[id]->bus) {
        return 0;
    }
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,(GstBusFunc)
                                            gstreamer_video_pipeline_bus_callback, (gpointer) loop);
}

bool video_renderer_eos_watch() {
    /* PATCH (Plan B): `renderer` is assigned only when the hls playbin reaches
       READY (video_renderer_init).  Before audit #5 a failed READY exit(1)'d, so
       this could never see NULL; now the slot survives non-READY and uxplay.cpp
       has already armed a 100 ms timeout on this function.  Guard, or the fix for
       a controlled exit turns into a SIGSEGV in the host tray. */
    if (hls_video && renderer && renderer->eos) {
        renderer->eos = FALSE;
	return true;
    }
    return false; 
}
