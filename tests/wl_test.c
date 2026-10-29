/* Runs the OS's Wayland compositor natively (on the build machine) for a few
 * seconds while real clients connect to it, then writes what each window
 * showed as a PNG-able PPM. Used by tests/wl-test.sh.
 *
 *   wl_test RUNTIME_DIR SECONDS OUT_PREFIX
 */
#include "../userland/shell/wl/wl.h"

#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void wls_set_screen(int w, int h);

void wl_host_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void wl_host_view_mapped(wl_view *v) {
    fprintf(stderr, "MAPPED view %d %dx%d title='%s' app='%s'\n", v->id, v->w, v->h, v->title, v->app_id);
}
void wl_host_view_unmapped(wl_view *v) { fprintf(stderr, "UNMAPPED view %d\n", v->id); }

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void save(wl_view *v, const char *prefix) {
    char path[300];
    snprintf(path, sizeof path, "%s-%d.ppm", prefix, v->id);
    FILE *f = fopen(path, "wb");
    if (!f || !v->px)
        return;
    fprintf(f, "P6\n%d %d\n255\n", v->w, v->h);
    for (int i = 0; i < v->w * v->h; i++) {
        uint32_t p = v->px[i];
        unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "SAVED %s (%dx%d, %llu frames)\n", path, v->w, v->h, (unsigned long long)v->version);
}

int main(int argc, char **argv) {
    if (argc < 4)
        return 2;
    wls_set_screen(1600, 900);
    if (!wls_start(argv[1]))
        return 1;
    uint32_t start = now_ms(), last_frame = 0, end = start + (uint32_t)atoi(argv[2]) * 1000;
    bool poked = false;
    while (now_ms() < end) {
        int fds[40];
        int n = wls_fds(fds, 40);
        struct pollfd p[40];
        for (int i = 0; i < n; i++)
            p[i] = (struct pollfd){fds[i], POLLIN, 0};
        poll(p, (nfds_t)n, 8);
        wls_dispatch();
        uint32_t t = now_ms();
        if (t - last_frame >= 16) {
            wls_frame_done(t);
            last_frame = t;
        }
        /* halfway: move the pointer over the first window and type a key */
        if (!poked && t - start > (end - start) / 2 && wls_view_count() > 0) {
            wl_view *v = wls_view(0);
            wls_pointer_motion(v, 20, 20, t);
            wls_pointer_button(0x110, true, t);
            wls_pointer_button(0x110, false, t);
            wls_keyboard_focus(v);
            wls_key(30, true, 0, t); /* KEY_A */
            wls_key(30, false, 0, t);
            poked = true;
            fprintf(stderr, "POKED view %d\n", v->id);
        }
    }
    for (int i = 0; i < wls_view_count(); i++)
        save(wls_view(i), argv[3]);
    fprintf(stderr, "VIEWS %d\n", wls_view_count());
    return 0;
}
