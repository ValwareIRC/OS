/* os-boot: the boot screen once the person's things are back (init starts it right after restoring them), until
 * the desktop's first frame. The kernel's splash goes on here in their own design: its colours and its type
 * (/etc/os/design.json), shifting smoothly from the kernel's neutral one. The steps are the kernel's
 * (/proc/bootsteps), which init marks as it goes; Esc shows them, as on the kernel's screen.
 *
 * The desktop asks it to stop when its first frame is ready (/tmp/os-boot.stop); it then marks the desktop
 * step done, says it has stopped (/tmp/os-boot.stopped) and leaves the screen as it is, for the desktop to
 * fade from. */
#define _GNU_SOURCE
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../lib/json.h"

/* Linux UAPI definitions (linux/fb.h, linux/input.h), as in shell/sys.c: musl's toolchain ships no kernel
 * headers, and these layouts are fixed by the ABI. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
struct fb_bitfield {
    uint32_t offset, length, msb_right;
};
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock, left_margin, right_margin, upper_margin,
        lower_margin, hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};
struct input_event {
    struct timeval time;
    uint16_t type, code;
    int32_t value;
};
#define EV_KEY 1
#define KEY_ESC 1

#define STOP "/tmp/os-boot.stop"
#define STOPPED "/tmp/os-boot.stopped"
#define LAST "/tmp/os-boot.last" /* its last frame, for the desktop to fade from (sys.c) */
#define RUNNING "/tmp/os-boot.running" /* (the desktop waits for it to stop only while it runs) */
#define FONTS "/usr/share/fonts"

static uint64_t now_ms(void);
static void load_neutral_faces(void);
static void draw_splash(void);
static void draw_steps(void);

/* ---- the screen ---- */

static int W, H, pitch, rs, gs, bs;
static uint8_t *fbmem;
static uint32_t *frame, *from; /* what is drawn (0xRRGGBB); what the kernel showed, faded from */

static bool screen_open(void) {
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0 ||
        v.bits_per_pixel != 32)
        return false;
    W = (int)v.xres, H = (int)v.yres, pitch = (int)f.line_length;
    rs = (int)v.red.offset, gs = (int)v.green.offset, bs = (int)v.blue.offset;
    fbmem = mmap(NULL, (size_t)pitch * (size_t)H, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbmem == MAP_FAILED)
        return false;
    frame = malloc((size_t)W * (size_t)H * 4);
    from = malloc((size_t)W * (size_t)H * 4);
    if (!frame || !from)
        return false;
    return true;
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t) { /* t of 255 towards b */
    uint32_t r = ((a >> 16 & 0xff) * (255 - t) + (b >> 16 & 0xff) * t) / 255;
    uint32_t g = ((a >> 8 & 0xff) * (255 - t) + (b >> 8 & 0xff) * t) / 255;
    uint32_t bl = ((a & 0xff) * (255 - t) + (b & 0xff) * t) / 255;
    return r << 16 | g << 8 | bl;
}

/* the frame to the screen, `fade` of 255 of the way from what was there before */
static void present(uint32_t fade) {
    for (int y = 0; y < H; y++) {
        uint32_t *dst = (uint32_t *)(fbmem + (size_t)y * (size_t)pitch);
        const uint32_t *src = frame + (size_t)y * W, *old = from + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            uint32_t p = fade >= 255 ? src[x] : mix(old[x], src[x], fade);
            dst[x] = (p >> 16 & 0xff) << rs | (p >> 8 & 0xff) << gs | (p & 0xff) << bs;
        }
    }
}

static void fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
            frame[(size_t)j * W + i] = c;
}

static void blend(int x, int y, uint32_t c, uint32_t a) {
    if (x < 0 || y < 0 || x >= W || y >= H || !a)
        return;
    uint32_t *p = &frame[(size_t)y * W + x];
    *p = mix(*p, c, a);
}

/* ---- type ---- */

struct face {
    stbtt_fontinfo info;
    unsigned char *data;
    float scale;
    int ascent;
    struct glyph {
        unsigned char *bm;
        int w, h, x, y, adv;
        bool done;
    } g[128];
};

static bool face_load(struct face *f, const char *family, float px) {
    char path[256], name[96];
    size_t k = 0;
    for (const char *s = family; *s && k + 1 < sizeof name; s++)
        if (*s != ' ')
            name[k++] = *s;
    name[k] = 0;
    snprintf(path, sizeof path, FONTS "/%s.ttf", name);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = malloc((size_t)n);
    bool ok = f->data && fread(f->data, 1, (size_t)n, fp) == (size_t)n;
    fclose(fp);
    if (!ok || !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0)))
        return false;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, px);
    int a, d, l;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &l);
    f->ascent = (int)(a * f->scale + 0.5f);
    return true;
}

static struct glyph *glyph(struct face *f, int c) {
    if (c < 32 || c > 126)
        c = '?';
    struct glyph *g = &f->g[c];
    if (!g->done) {
        g->done = true;
        g->bm = stbtt_GetCodepointBitmap(&f->info, 0, f->scale, c, &g->w, &g->h, &g->x, &g->y);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f->info, c, &adv, &lsb);
        g->adv = (int)(adv * f->scale * 64 + 0.5f);
    }
    return g;
}

static int text_width(struct face *f, const char *s) {
    int w = 0;
    for (; *s; s++)
        w += glyph(f, (unsigned char)*s)->adv;
    return (w + 32) / 64;
}

static void text(struct face *f, int x, int y, const char *s, uint32_t c) {
    int pen = x * 64;
    for (; *s; s++) {
        struct glyph *g = glyph(f, (unsigned char)*s);
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++)
                blend(pen / 64 + g->x + i, y + g->y + j, c, g->bm[j * g->w + i]);
        pen += g->adv;
    }
}

/* ---- shapes ---- */

static uint32_t cover(float d, float hw) {
    float c = hw - d + 0.5f;
    return c <= 0 ? 0 : c >= 1 ? 255 : (uint32_t)(c * 255);
}

static void ring(float cx, float cy, float r, float sw, uint32_t c) {
    int R = (int)(r + sw + 2);
    for (int y = -R; y <= R; y++)
        for (int x = -R; x <= R; x++) {
            float d = sqrtf((float)(x * x + y * y));
            blend((int)cx + x, (int)cy + y, c, sw > 0 ? cover(fabsf(d - r), sw / 2) : cover(d, r));
        }
}

static void line(float x0, float y0, float x1, float y1, float w, uint32_t c) {
    int minx = (int)fminf(x0, x1) - (int)w - 1, maxx = (int)fmaxf(x0, x1) + (int)w + 1;
    int miny = (int)fminf(y0, y1) - (int)w - 1, maxy = (int)fmaxf(y0, y1) + (int)w + 1;
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            float t = l2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / l2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float qx = x - (x0 + t * dx), qy = y - (y0 + t * dy);
            blend(x, y, c, cover(sqrtf(qx * qx + qy * qy), w / 2));
        }
}

/* ---- the look: theirs, or neutral before they have one ---- */

static uint32_t bg = 0x101114, ink = 0xe8e6e3, muted = 0x85837f, accent = 0xd9cbbd, track = 0x26272b;
static struct face title, small, hint;

static uint32_t hexcolor(const char *s, uint32_t fallback) {
    if (!s || s[0] != '#' || strlen(s) < 7)
        return fallback;
    return (uint32_t)strtoul(s + 1, NULL, 16) >> (strlen(s) >= 9 ? 8 : 0) & 0xffffff;
}

static void load_neutral_faces(void) {
    face_load(&title, "Inter-Medium", 28);
    face_load(&small, "Inter", 15);
    face_load(&hint, "Inter", 13);
}

static void look_load(void) {
    memset(&title, 0, sizeof title), memset(&small, 0, sizeof small), memset(&hint, 0, sizeof hint);
    const char *heading = "Inter-Medium", *body = "Inter";
    char hbuf[64] = "", bbuf[64] = "";
    FILE *f = fopen("/etc/os/design.json", "r");
    if (f) {
        static char text_buf[64 * 1024];
        size_t n = fread(text_buf, 1, sizeof text_buf - 1, f);
        fclose(f);
        json *j = json_parse(text_buf, n);
        json *c = json_get(j, "colors");
        if (c) {
            bg = hexcolor(json_str(json_get(c, "background"), NULL), bg);
            ink = hexcolor(json_str(json_get(c, "text"), NULL), ink);
            muted = hexcolor(json_str(json_get(c, "muted"), NULL), mix(bg, ink, 140));
            accent = hexcolor(json_str(json_get(c, "accent"), NULL), accent);
            track = mix(bg, muted, 60);
        }
        json *fo = json_get(j, "fonts");
        snprintf(hbuf, sizeof hbuf, "%s", json_str(json_get(fo, "heading"), ""));
        snprintf(bbuf, sizeof bbuf, "%s", json_str(json_get(fo, "body"), ""));
        json_free(j);
    }
    if (!hbuf[0] || !face_load(&title, hbuf, 30))
        face_load(&title, heading, 28);
    if (!bbuf[0] || !face_load(&small, bbuf, 15))
        face_load(&small, body, 15);
    if (!bbuf[0] || !face_load(&hint, bbuf, 13))
        face_load(&hint, body, 13);
}

/* ---- the steps (/proc/bootsteps) ---- */

struct step {
    char id[16], state[12], label[40], doing[64], detail[64];
    unsigned long done, total, ms;
    int weight;
};
static struct step steps[16];
static int nsteps;

static bool steps_view;

static bool steps_read(void) {
    static char last[4096];
    char buf[4096];
    int fd = open("/proc/bootsteps", O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = 0;
    if (!strcmp(buf, last))
        return false;
    memcpy(last, buf, (size_t)n + 1);
    nsteps = 0;
    for (char *l = strtok(buf, "\n"); l && nsteps < 16; l = strtok(NULL, "\n")) {
        if (!strncmp(l, "view ", 5)) { /* the kernel's view, carried on at the start */
            static bool taken;
            if (!taken)
                steps_view = !strcmp(l + 5, "steps"), taken = true;
            continue;
        }
        struct step *s = &steps[nsteps];
        char rest[200] = "";
        if (sscanf(l, "%15s %11s %lu %lu %lu %d %199[^\n]", s->id, s->state, &s->done, &s->total, &s->ms, &s->weight,
                   rest) < 6)
            continue;
        char *a = rest, *b = strchr(a, '|'), *c = b ? strchr(b + 1, '|') : NULL;
        if (b)
            *b++ = 0;
        if (c)
            *c++ = 0;
        snprintf(s->label, sizeof s->label, "%s", a);
        snprintf(s->doing, sizeof s->doing, "%s", b ? b : "");
        snprintf(s->detail, sizeof s->detail, "%s", c ? c : "");
        nsteps++;
    }
    return true;
}

static void step_mark(const char *id, const char *what) {
    int fd = open("/proc/bootsteps", O_WRONLY);
    if (fd < 0)
        return;
    char l[96];
    int n = snprintf(l, sizeof l, "%s %s\n", id, what);
    if (write(fd, l, (size_t)n) < 0) {
    }
    close(fd);
}

static int overall(void) { /* of 1000 */
    long all = 0, got = 0;
    for (int i = 0; i < nsteps; i++) {
        struct step *s = &steps[i];
        all += s->weight;
        if (!strcmp(s->state, "done"))
            got += s->weight * 1000L;
        else if (!strcmp(s->state, "active") && s->total)
            got += (long)((double)s->weight * 1000 * (double)(s->done < s->total ? s->done : s->total) / (double)s->total);
    }
    return all ? (int)(got / all) : 0;
}

/* ---- the two views ---- */

static int spin;

static void draw_splash(void) {
    fill(0, 0, W, H, bg);
    int y = H / 2;
    ring(W / 2.0f, y - 58, 13, 2, accent);
    const char *name = "Operating System";
    text(&title, (W - text_width(&title, name)) / 2, y - 6, name, ink);
    int bw = 280, bx = (W - bw) / 2;
    fill(bx, y + 22, bw, 3, track);
    fill(bx, y + 22, bw * overall() / 1000, 3, accent);
    const char *what = "";
    for (int i = 0; i < nsteps && !what[0]; i++)
        if (!strcmp(steps[i].state, "active") && steps[i].weight)
            what = steps[i].doing;
    text(&small, (W - text_width(&small, what)) / 2, y + 56, what, muted);
    const char *h = "esc   details";
    text(&hint, W - 40 - text_width(&hint, h), H - 32, h, mix(bg, muted, 170));
}

static void draw_steps(void) {
    fill(0, 0, W, H, bg);
    int x = W > 900 ? (W - 760) / 2 : 40;
    ring(x + 6, 82, 7, 2, accent);
    text(&title, x + 30, 92, "Operating System", ink);
    for (int i = 0, row = 0; i < nsteps; i++) {
        struct step *s = &steps[i];
        bool done = !strcmp(s->state, "done"), active = !strcmp(s->state, "active"), bad = !strcmp(s->state, "failed");
        if (!s->weight && !done && !active && !bad)
            continue; /* a background step not started (the built-in mind, left on the disc): not shown */
        int y = 150 + row++ * 34;
        if (done) {
            line(x, y - 6, x + 4, y - 2, 2, accent);
            line(x + 4, y - 2, x + 12, y - 11, 2, accent);
        } else if (active) {
            for (int k = 0; k < 8; k++) {
                float a = (float)k * 0.7853982f;
                ring(x + 6 + 7 * sinf(a), y - 6 - 7 * cosf(a), 1.2f, 0, mix(bg, accent, (uint32_t)(255 - ((spin - k) & 7) * 30)));
            }
        } else if (bad) {
            line(x + 1, y - 11, x + 11, y - 1, 2, 0xe0866e);
            line(x + 11, y - 11, x + 1, y - 1, 2, 0xe0866e);
        } else {
            ring(x + 6, y - 6, 2, 0, mix(bg, muted, 120));
        }
        text(&small, x + 30, y, s->label, done || active || bad ? ink : mix(bg, muted, 160));
        if (active && s->total) {
            int bw = 220;
            fill(x + 190, y - 7, bw, 3, track);
            fill(x + 190, y - 7, (int)((double)bw * (double)(s->done < s->total ? s->done : s->total) / (double)s->total), 3,
                 accent);
            char amount[48];
            snprintf(amount, sizeof amount, "%lu / %lu MB", s->done >> 20, s->total >> 20);
            text(&small, x + 190 + bw + 14, y, amount, muted);
        } else if (s->detail[0]) {
            text(&small, x + 190, y, s->detail, muted);
        }
        if ((done || active) && s->ms) {
            char t[24];
            snprintf(t, sizeof t, s->ms < 1000 ? "%lu ms" : "%lu.%lu s", s->ms < 1000 ? s->ms : s->ms / 1000,
                     s->ms / 100 % 10);
            text(&small, x + 760 - text_width(&small, t), y, t, mix(bg, muted, 190));
        }
    }
    const char *h = "esc   back";
    text(&hint, W - 40 - text_width(&hint, h), H - 32, h, mix(bg, muted, 170));
}

/* ---- Esc, from any keyboard ---- */

static int keyfds[16], nkey;

static void keys_open(void) {
    for (int i = 0; i < 16; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
            keyfds[nkey++] = fd;
    }
}

static bool esc_pressed(void) {
    bool hit = false;
    struct input_event ev[32];
    for (int i = 0; i < nkey; i++) {
        ssize_t n;
        while ((n = read(keyfds[i], ev, sizeof ev)) > 0)
            for (size_t k = 0; k < (size_t)n / sizeof ev[0]; k++)
                if (ev[k].type == EV_KEY && ev[k].code == KEY_ESC && ev[k].value == 1)
                    hit = true;
    }
    return hit;
}

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

int main(void) {
    uint64_t began = now_ms();
    if (access(STOP, F_OK) == 0) /* the desktop is already up: nothing to show */
        return 0;
    if (!screen_open()) {
        fprintf(stderr, "os-boot: no screen to draw on\n");
        return 1;
    }
    int rf = open(RUNNING, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (rf >= 0)
        close(rf);
    /* what the kernel shows now: its neutral splash, drawn the same way (look_load comes after) */
    load_neutral_faces();
    steps_read();
    steps_view ? draw_steps() : draw_splash();
    memcpy(from, frame, (size_t)W * (size_t)H * 4);
    look_load();
    keys_open();
    fprintf(stderr, "os-boot: on the screen in %llu ms (%dx%d, %s look: background #%06x, text #%06x, accent #%06x; "
            "%d keyboards)\n", (unsigned long long)(now_ms() - began), W, H,
            access("/etc/os/design.json", R_OK) == 0 ? "their" : "the neutral", bg, ink, accent, nkey);
    uint64_t start = now_ms(), last_spin = 0;
    bool dirty = true;
    for (;;) {
        uint64_t t = now_ms();
        if (access(STOP, F_OK) == 0 || t - start > 180000) { /* the desktop is ready (or never came) */
            step_mark("desktop", "done");
            int lf = open(LAST ".part", O_WRONLY | O_CREAT | O_TRUNC, 0644); /* what it showed last, in memory */
            if (lf >= 0) {
                bool ok = write(lf, frame, (size_t)W * (size_t)H * 4) == (ssize_t)((size_t)W * (size_t)H * 4);
                close(lf);
                if (ok)
                    rename(LAST ".part", LAST);
            }
            fprintf(stderr, "os-boot: the desktop takes over\n");
            int fd = open(STOPPED, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0)
                close(fd);
            unlink(RUNNING);
            return 0;
        }
        if (esc_pressed()) {
            steps_view = !steps_view, dirty = true;
            fprintf(stderr, "os-boot: %s\n", steps_view ? "the steps" : "the splash");
        }
        if (steps_read())
            dirty = true;
        if (steps_view && t - last_spin >= 90)
            spin++, last_spin = t, dirty = true;
        uint32_t fade = t - start >= 450 ? 255 : (uint32_t)((t - start) * 255 / 450);
        if (dirty || fade < 255) {
            steps_view ? draw_steps() : draw_splash();
            present(fade);
            dirty = false;
        }
        struct timespec nap = {0, 33 * 1000000L};
        nanosleep(&nap, NULL);
    }
}
