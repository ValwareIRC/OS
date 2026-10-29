/* os-record: turns the frames the desktop shell draws into a WebM video.
 *
 *   os-record WIDTH HEIGHT SOUND OUTFILE
 *
 * The shell shares a memfd as fd 3: NSLOT frames of WIDTH*HEIGHT pixels,
 * 32-bit BGRx, one after another. On stdin it writes one byte per frame,
 * the slot it has just filled; on stdout this answers with the slot's byte
 * when the frame has been taken in, so the shell may fill it again. When
 * stdin closes (or a 0xff byte comes) the video is finished (end of stream) and this exits 0.
 *
 * SOUND: 0 none, 1 the computer's (what the speakers play), 2 the
 * microphone, 3 both. Video is VP8, sound Opus, timed by the pipeline
 * clock (frames are stamped as they arrive), so the two stay together. */

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NSLOT 3

static uint8_t *ring;
static size_t slot_bytes;

static void slot_done(gpointer data) {
    uint8_t s = (uint8_t)(uintptr_t)data;
    if (write(1, &s, 1) != 1) { /* the shell has gone: nothing to tell */
    }
}

static const char *sound_branch(int sound) {
    switch (sound) {
    case 1:
        return " pulsesrc device=@DEFAULT_MONITOR@ ! audioconvert ! audioresample ! opusenc bitrate=128000 ! queue ! mux.";
    case 2:
        return " pulsesrc ! audioconvert ! audioresample ! opusenc bitrate=96000 ! queue ! mux.";
    case 3:
        return " audiomixer name=mix ! audioconvert ! audioresample ! opusenc bitrate=128000 ! queue ! mux."
               " pulsesrc device=@DEFAULT_MONITOR@ ! audioconvert ! audioresample ! queue ! mix."
               " pulsesrc ! audioconvert ! audioresample ! queue ! mix.";
    }
    return "";
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: os-record WIDTH HEIGHT SOUND OUTFILE\n");
        return 2;
    }
    int w = atoi(argv[1]), h = atoi(argv[2]), sound = atoi(argv[3]);
    const char *out = argv[4];
    if (w < 16 || h < 16 || (w & 1) || (h & 1)) {
        fprintf(stderr, "os-record: bad size %dx%d\n", w, h);
        return 2;
    }
    slot_bytes = (size_t)w * h * 4;
    ring = mmap(NULL, slot_bytes * NSLOT, PROT_READ, MAP_SHARED, 3, 0);
    if (ring == MAP_FAILED) {
        perror("os-record: frames");
        return 1;
    }
    gst_init(&argc, &argv);
    /* realtime VP8: fast enough for software encoding as it is drawn */
    gchar *desc = g_strdup_printf(
        "appsrc name=frames is-live=true do-timestamp=true format=time "
        "caps=video/x-raw,format=BGRx,width=%d,height=%d,framerate=0/1 "
        "! queue max-size-buffers=2 leaky=no ! videoconvert ! video/x-raw,format=I420 "
        "! vp8enc deadline=1 cpu-used=16 threads=4 end-usage=cbr target-bitrate=%d keyframe-max-dist=90 "
        "! queue ! webmmux name=mux ! filesink location=\"%s\"%s",
        w, h, w * h > 1500000 ? 8000000 : 5000000, out, sound_branch(sound));
    GError *err = NULL;
    GstElement *pipe = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!pipe) {
        fprintf(stderr, "os-record: %s\n", err ? err->message : "no pipeline");
        return 1;
    }
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipe), "frames");
    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "os-record: the pipeline wouldn't start\n");
        return 1;
    }
    fprintf(stderr, "os-record: %dx%d, sound %d, to %s\n", w, h, sound, out);

    uint8_t s;
    long frames = 0;
    while (read(0, &s, 1) == 1) {
        if (s == 0xff) /* the end, said outright */
            break;
        if (s >= NSLOT)
            continue;
        /* the shell's memory itself, no copy: the slot is handed back when
         * the frame has been converted and let go of */
        GstBuffer *buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, ring + (size_t)s * slot_bytes, slot_bytes,
                                                     0, slot_bytes, (gpointer)(uintptr_t)s, slot_done);
        if (gst_app_src_push_buffer(GST_APP_SRC(src), buf) != GST_FLOW_OK)
            break;
        frames++;
    }
    /* finish the file: end of stream through the whole pipeline */
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    if (sound) /* the sound sources don't end by themselves */
        gst_element_send_event(pipe, gst_event_new_eos());
    GstBus *bus = gst_element_get_bus(pipe);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    int rc = 0;
    if (!msg) {
        fprintf(stderr, "os-record: the file took too long to finish\n");
        rc = 1;
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *e = NULL;
        gst_message_parse_error(msg, &e, NULL);
        fprintf(stderr, "os-record: %s\n", e ? e->message : "error");
        g_clear_error(&e);
        rc = 1;
    }
    if (msg)
        gst_message_unref(msg);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(src);
    gst_object_unref(pipe);
    fprintf(stderr, "os-record: %ld frames, %s\n", frames, rc ? "failed" : "done");
    return rc;
}
