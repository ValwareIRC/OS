#define _GNU_SOURCE
#include "capture.h"
#include "app.h"
#include "controls.h"
#include "files.h"
#include "fm/fm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image_write.h"
#pragma GCC diagnostic pop

/* Screenshots and screen recording, the way GNOME (and so Zorin) and Windows
 * do it: Print Screen freezes the screen under a bar of choices.
 *
 *   Screenshot | Record      what to make
 *   Area | Screen | Window   what of the screen: a dragged area (with handles
 *                            to adjust it), all of it, or one window
 *   Pointer, Delay / Sound   whether the pointer shows; a countdown for a
 *                            screenshot; the sound a recording takes
 *
 * The big round button (or Enter) takes it; Escape leaves. A screenshot is
 * a PNG in Pictures/Screenshots, and a card in the corner shows it with Open
 * and Show in Files. A recording runs with a red pill at the top of the
 * screen (its time; a click, or Print Screen again, stops it) and becomes a
 * WebM video in Videos/Screencasts, made by os-record (GStreamer, VP8 and
 * Opus) from the frames the shell draws, passed through shared memory. */

#define SHOTS_DIR "/root/Pictures/Screenshots"
#define DATA_VIDEOS "/var/lib/os-data/Videos"
#define RECORDER "/usr/libexec/os-record"
#define NSLOT 3

static font *HF(float px) { return font_get(A.d.font_heading, px); }
static font *BF(float px) { return font_get(A.d.font_body, px); }

enum { KIND_SHOT, KIND_RECORD };
enum { MODE_AREA, MODE_SCREEN, MODE_WINDOW };

static struct {
    /* the bar */
    bool open;
    int opening;     /* frames to wait before freezing the screen (the launcher fading away) */
    int now_pending; /* the same, for a whole-screen screenshot */
    canvas *still; /* the screen as it was when Print Screen was pressed */
    int mx, my;    /* where the pointer was then */
    int kind, mode;
    bool pointer;
    int delay;  /* 0, 3 or 5 s */
    int sound;  /* 0 none, 1 the computer's, 2 the microphone, 3 both */
    rect sel;   /* the area (screen pixels); w == 0: none yet */
    rect wins[WIN_MAX];
    int nwins, win_pick; /* window mode: the windows, front first; the one chosen */
    int drag;   /* 0 none, 1 drawing, 2 moving, 3..10 a handle */
    int ax, ay;
    rect drag_from;
    input_state in; /* this frame's real input: everything else sees none */
    /* a screenshot on a countdown */
    uint64_t shot_at;
    rect shot_r;
    job *saving;
    /* recording */
    bool recording;
    rect rec;
    uint64_t rec_start;
    pid_t pid;
    int ctl, ack; /* frames announced to os-record; slots it has finished with */
    uint8_t *ring;
    size_t slot_bytes;
    bool busy[NSLOT];
    int dropped, frames;
    int skip; /* frames not to take yet (the one with the bar still on it) */
    char rec_path[512];
    bool finishing;
    uint64_t finish_by; /* os-record is stopped if the file isn't done by then */
    /* the card that shows what was made */
    canvas *thumb;
    char card_path[512];
    bool card_video;
    uint64_t card_at;
    char card_err[160];
} C = {.pointer = true, .ctl = -1, .ack = -1};

bool capture_active(void) { return C.open; }
bool capture_recording(void) { return C.recording; }

/* ---- small helpers ---- */

static void stamp(char *out, size_t n, const char *what, const char *ext, const char *dir) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H-%M-%S", &tm);
    snprintf(out, n, "%s/%s %s.%s", dir, what, when, ext);
}

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

static rect clamp_rect(rect r) {
    if (r.w < 0)
        r.x += r.w, r.w = -r.w;
    if (r.h < 0)
        r.y += r.h, r.h = -r.h;
    if (r.x < 0)
        r.w += r.x, r.x = 0;
    if (r.y < 0)
        r.h += r.y, r.y = 0;
    if (r.x + r.w > A.W)
        r.w = A.W - r.x;
    if (r.y + r.h > A.H)
        r.h = A.H - r.y;
    if (r.w < 0)
        r.w = 0;
    if (r.h < 0)
        r.h = 0;
    return r;
}

/* What will be taken: the area, the screen, or the chosen window. */
static rect target(void) {
    if (C.mode == MODE_SCREEN)
        return (rect){0, 0, A.W, A.H};
    if (C.mode == MODE_WINDOW)
        return C.win_pick >= 0 ? clamp_rect(C.wins[C.win_pick]) : (rect){0, 0, 0, 0};
    return clamp_rect(C.sel);
}

/* A copy of part of a canvas. */
static canvas *crop(const canvas *src, rect r) {
    canvas *c = canvas_new(r.w, r.h);
    if (!c)
        return NULL;
    for (int y = 0; y < r.h; y++)
        memcpy(c->px + (size_t)y * r.w, src->px + (size_t)(r.y + y) * src->w + r.x, (size_t)r.w * 4);
    return c;
}

/* A small copy for the card (box-filtered). */
static canvas *shrink(const canvas *src, int maxw, int maxh) {
    float s = (float)maxw / src->w;
    if (src->h * s > maxh)
        s = (float)maxh / src->h;
    if (s > 1)
        s = 1;
    int w = (int)(src->w * s), h = (int)(src->h * s);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    canvas *c = canvas_new(w, h);
    if (!c)
        return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int x0 = x * src->w / w, x1 = (x + 1) * src->w / w, y0 = y * src->h / h, y1 = (y + 1) * src->h / h;
            if (x1 <= x0)
                x1 = x0 + 1;
            if (y1 <= y0)
                y1 = y0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 4)
                for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 4) {
                    uint32_t p = src->px[(size_t)yy * src->w + xx];
                    r += C_R(p), g += C_G(p), b += C_B(p), n++;
                }
            c->px[(size_t)y * w + x] = RGBA(r / n, g / n, b / n, 255);
        }
    return c;
}

static void show_card(canvas *full, const char *path, bool video) {
    if (C.thumb)
        canvas_free(C.thumb);
    C.thumb = full ? shrink(full, 260, 150) : NULL;
    snprintf(C.card_path, sizeof C.card_path, "%s", path);
    C.card_video = video;
    C.card_at = A.now;
    C.card_err[0] = 0;
}

/* ---- screenshots ---- */

struct png_job {
    canvas *img;
    char path[512];
};

static void png_child(void *arg, char **result) {
    struct png_job *j = arg;
    size_t n = (size_t)j->img->w * j->img->h;
    uint8_t *rgba = malloc(n * 4);
    if (!rgba) {
        *result = strdup("out of memory");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t p = j->img->px[i];
        rgba[i * 4] = (uint8_t)C_R(p), rgba[i * 4 + 1] = (uint8_t)C_G(p), rgba[i * 4 + 2] = (uint8_t)C_B(p);
        rgba[i * 4 + 3] = 255;
    }
    mkdirs(SHOTS_DIR);
    int ok = stbi_write_png(j->path, j->img->w, j->img->h, 4, rgba, j->img->w * 4);
    *result = strdup(ok ? "ok" : "couldn't write the file");
}

static struct png_job *pending_png;

static void take_screenshot(const canvas *from, rect r, bool with_pointer, int px, int py) {
    r = clamp_rect(r);
    if (r.w < 2 || r.h < 2 || C.saving)
        return;
    canvas *img = crop(from, r);
    if (!img)
        return;
    if (with_pointer && pt_in(r, px, py))
        ui_cursor(img, px - r.x, py - r.y);
    struct png_job *j = calloc(1, sizeof *j);
    j->img = img;
    stamp(j->path, sizeof j->path, "Screenshot", "png", SHOTS_DIR);
    pending_png = j;
    C.saving = job_start(png_child, j);
    show_card(img, j->path, false);
    LOG("capture: screenshot %dx%d -> %s\n", r.w, r.h, j->path);
}

static void poll_saving(void) {
    char *res = NULL;
    if (!C.saving || !job_poll(C.saving, &res))
        return;
    job_free(C.saving);
    C.saving = NULL;
    if (res && strcmp(res, "ok")) {
        snprintf(C.card_err, sizeof C.card_err, "Couldn't save it: %s", res);
        LOG("capture: screenshot failed: %s\n", res);
    } else {
        LOG("capture: saved %s\n", pending_png ? pending_png->path : "?");
    }
    free(res);
    if (pending_png) {
        canvas_free(pending_png->img);
        free(pending_png);
        pending_png = NULL;
    }
}

/* ---- recording ---- */

static void videos_dir(char *out, size_t n) {
    /* on the disk's file system when there is one: videos are big */
    struct stat st;
    if (stat("/var/lib/os-data/lost+found", &st) == 0 || stat("/var/lib/os-data/flatpak", &st) == 0) {
        mkdirs(DATA_VIDEOS "/Screencasts");
        if (chown(DATA_VIDEOS, SESSION_UID, SESSION_UID) < 0 ||
            chown(DATA_VIDEOS "/Screencasts", SESSION_UID, SESSION_UID) < 0) {
        }
        if (lstat("/root/Videos", &st) != 0) {
            if (symlink(DATA_VIDEOS, "/root/Videos") < 0) {
            }
        }
        snprintf(out, n, "%s/Screencasts", DATA_VIDEOS);
        return;
    }
    mkdirs("/root/Videos/Screencasts");
    if (chown("/root/Videos/Screencasts", SESSION_UID, SESSION_UID) < 0) {
    }
    snprintf(out, n, "/root/Videos/Screencasts");
}

static void stop_recording(void);

static bool start_recording(rect r) {
    r = clamp_rect(r);
    r.w &= ~1, r.h &= ~1; /* the encoder wants even sizes */
    if (r.w < 16 || r.h < 16)
        return false;
    if (access(RECORDER, X_OK) != 0) {
        toast("Recording needs os-record, which isn't in this system.");
        return false;
    }
    C.slot_bytes = (size_t)r.w * r.h * 4;
    int mfd = memfd_create("os-record-frames", MFD_CLOEXEC);
    if (mfd < 0 || ftruncate(mfd, (off_t)(C.slot_bytes * NSLOT)) < 0) {
        if (mfd >= 0)
            close(mfd);
        toast("Couldn't start recording (no memory for frames).");
        return false;
    }
    C.ring = mmap(NULL, C.slot_bytes * NSLOT, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (C.ring == MAP_FAILED) {
        C.ring = NULL;
        close(mfd);
        return false;
    }
    int to[2], from[2];
    if (pipe2(to, O_CLOEXEC) < 0 || pipe2(from, O_CLOEXEC) < 0) {
        munmap(C.ring, C.slot_bytes * NSLOT);
        C.ring = NULL;
        close(mfd);
        return false;
    }
    char dir[256];
    videos_dir(dir, sizeof dir);
    stamp(C.rec_path, sizeof C.rec_path, "Screencast", "webm", dir);
    char w[16], h[16], snd[8];
    snprintf(w, sizeof w, "%d", r.w);
    snprintf(h, sizeof h, "%d", r.h);
    snprintf(snd, sizeof snd, "%d", C.sound);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        dup2(mfd, 3);
        for (int fd = 4; fd < 256; fd++)
            close(fd);
        become_session_user();
        /* GStreamer's plugin cache somewhere it may write */
        setenv("GST_REGISTRY_1_0", SESSION_RUNTIME "/gst-registry.bin", 1);
        execl(RECORDER, "os-record", w, h, snd, C.rec_path, (char *)NULL);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    close(mfd);
    if (pid < 0) {
        close(to[1]);
        close(from[0]);
        munmap(C.ring, C.slot_bytes * NSLOT);
        C.ring = NULL;
        return false;
    }
    fcntl(from[0], F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    C.pid = pid;
    C.ctl = to[1];
    C.ack = from[0];
    memset(C.busy, 0, sizeof C.busy);
    C.dropped = C.frames = 0;
    C.skip = 1;
    C.rec = r;
    C.rec_start = A.now;
    C.recording = true;
    LOG("capture: recording %dx%d at %d,%d -> %s (sound %d)\n", r.w, r.h, r.x, r.y, C.rec_path, C.sound);
    return true;
}

/* os-record says which slots it has finished with (a byte each). */
static void read_acks(void) {
    uint8_t b[32];
    ssize_t n;
    if (!C.recording && !C.finishing)
        return;
    while (C.ack >= 0 && (n = read(C.ack, b, sizeof b)) > 0)
        for (ssize_t i = 0; i < n; i++)
            if (b[i] < NSLOT)
                C.busy[b[i]] = false;
}

/* This frame, into a free slot (or dropped when the encoder is behind). */
static void record_frame(void) {
    read_acks();
    if (C.skip > 0) {
        C.skip--;
        return;
    }
    int s = -1;
    for (int i = 0; i < NSLOT; i++)
        if (!C.busy[i]) {
            s = i;
            break;
        }
    if (s < 0) {
        C.dropped++;
        return;
    }
    uint8_t *dst = C.ring + (size_t)s * C.slot_bytes;
    for (int y = 0; y < C.rec.h; y++)
        memcpy(dst + (size_t)y * C.rec.w * 4, A.frame->px + (size_t)(C.rec.y + y) * A.frame->w + C.rec.x,
               (size_t)C.rec.w * 4);
    uint8_t msg = (uint8_t)s;
    if (write(C.ctl, &msg, 1) == 1) {
        C.busy[s] = true;
        C.frames++;
        if (C.frames == 1) { /* the card's picture */
            canvas *first = crop(A.frame, C.rec);
            if (first) {
                if (C.thumb)
                    canvas_free(C.thumb);
                C.thumb = shrink(first, 260, 150);
                canvas_free(first);
            }
        }
    } else {
        LOG("capture: the recorder stopped taking frames\n");
        stop_recording();
    }
}

static void stop_recording(void) {
    if (!C.recording)
        return;
    C.recording = false;
    /* end of frames, said outright: programs started meanwhile may hold the
     * pipe open too, so its closing alone wouldn't reach os-record */
    uint8_t end = 0xff;
    if (write(C.ctl, &end, 1) != 1) {
    }
    close(C.ctl);
    C.ctl = -1;
    LOG("capture: recording stopped after %llu ms, %d frames (%d dropped)\n",
        (unsigned long long)(A.now - C.rec_start), C.frames, C.dropped);
    C.finishing = true;
    C.finish_by = A.now + 20000;
}

/* Waits, a frame at a time, for os-record to finish the file (only the shell,
 * which started it, can wait for it). */
static void poll_finishing(void) {
    if (!C.finishing)
        return;
    read_acks();
    int st = 0;
    const char *res;
    if (waitpid(C.pid, &st, WNOHANG) == C.pid) {
        res = WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "ok" : "failed";
    } else if (A.now > C.finish_by) {
        kill(C.pid, SIGKILL);
        waitpid(C.pid, &st, 0);
        res = "timed out";
    } else {
        return;
    }
    C.finishing = false;
    if (C.ack >= 0)
        close(C.ack);
    C.ack = -1;
    if (C.ring)
        munmap(C.ring, C.slot_bytes * NSLOT);
    C.ring = NULL;
    canvas *keep = C.thumb;
    C.thumb = NULL;
    show_card(NULL, C.rec_path, true);
    C.thumb = keep;
    if (strcmp(res, "ok"))
        snprintf(C.card_err, sizeof C.card_err, "The recording didn't finish properly (%s).", res);
    LOG("capture: recording saved %s (%s)\n", C.rec_path, res);
}

/* ---- the bar ---- */

void capture_open(int kind) {
    if (C.open || C.recording || A.screen != SCR_DESKTOP)
        return;
    C.opening = 4;
    if (kind >= 0)
        C.kind = kind;
}

static void close_bar(void) {
    C.open = false;
    C.drag = 0;
    if (C.still) {
        canvas_free(C.still);
        C.still = NULL;
    }
}

/* Windows on screen now, front first. */
static void list_windows(void) {
    int order[WIN_MAX], n = 0;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized)
            order[n++] = w;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && A.win[order[j - 1]].z < A.win[order[j]].z; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    C.nwins = n;
    for (int i = 0; i < n; i++)
        C.wins[i] = A.win[order[i]].r;
    C.win_pick = n ? 0 : -1;
}

static void go(void) {
    rect r = target();
    if (r.w < 2 || r.h < 2) {
        toast(C.mode == MODE_WINDOW ? "Choose a window first." : "Drag over the part of the screen you want.");
        return;
    }
    bool pointer = C.pointer;
    if (C.kind == KIND_SHOT) {
        if (C.delay > 0) {
            C.shot_at = A.now + (uint64_t)C.delay * 1000;
            C.shot_r = r;
        } else {
            take_screenshot(C.still, r, pointer, C.mx, C.my);
        }
        close_bar();
    } else {
        close_bar();
        if (!start_recording(r))
            toast("Couldn't start recording.");
    }
}

/* Which handle of the area the pointer is on (3..10), or 2 inside, or 0. */
static int handle_at(rect s, int x, int y) {
    int hx[3] = {s.x, s.x + s.w / 2, s.x + s.w}, hy[3] = {s.y, s.y + s.h / 2, s.y + s.h};
    int k = 3;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            if (i == 1 && j == 1)
                continue;
            if (abs(x - hx[i]) <= 10 && abs(y - hy[j]) <= 10)
                return k;
            k++;
        }
    return pt_in(s, x, y) ? 2 : 0;
}

static void drag_area(void) {
    int dx = C.in.mx - C.ax, dy = C.in.my - C.ay;
    rect f = C.drag_from;
    if (C.drag == 1) {
        C.sel = (rect){C.ax, C.ay, dx, dy};
    } else if (C.drag == 2) {
        rect m = {f.x + dx, f.y + dy, f.w, f.h};
        if (m.x < 0) m.x = 0;
        if (m.y < 0) m.y = 0;
        if (m.x + m.w > A.W) m.x = A.W - m.w;
        if (m.y + m.h > A.H) m.y = A.H - m.h;
        C.sel = m;
    } else if (C.drag >= 3) {
        /* handles in reading order: tl, t, tr, l, r, bl, b, br */
        static const int sx[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, sy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
        int h = C.drag - 3, x0 = f.x, y0 = f.y, x1 = f.x + f.w, y1 = f.y + f.h;
        if (sx[h] < 0) x0 += dx;
        if (sx[h] > 0) x1 += dx;
        if (sy[h] < 0) y0 += dy;
        if (sy[h] > 0) y1 += dy;
        C.sel = (rect){x0, y0, x1 - x0, y1 - y0};
    }
    if (!C.in.down) {
        C.sel = clamp_rect(C.sel);
        C.drag = 0;
    }
}

/* Dims everything but r. */
static void dim_outside(rect r, float a) {
    color d = RGBA(0, 0, 0, (int)(a * 255));
    fill_rect(A.frame, (rect){0, 0, A.W, r.y}, d);
    fill_rect(A.frame, (rect){0, r.y + r.h, A.W, A.H - r.y - r.h}, d);
    fill_rect(A.frame, (rect){0, r.y, r.x, r.h}, d);
    fill_rect(A.frame, (rect){r.x + r.w, r.y, A.W - r.x - r.w, r.h}, d);
}

static void draw_area(rect s, bool handles) {
    stroke_round(A.frame, rect_inset(s, -1), 0, 2, RGBA(255, 255, 255, 235));
    if (handles) {
        int hx[3] = {s.x, s.x + s.w / 2, s.x + s.w}, hy[3] = {s.y, s.y + s.h / 2, s.y + s.h};
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                if (!(i == 1 && j == 1)) {
                    fill_circle(A.frame, (float)hx[i], (float)hy[j], 7, RGBA(0, 0, 0, 90));
                    fill_circle(A.frame, (float)hx[i], (float)hy[j], 5.5f, RGBA(255, 255, 255, 255));
                }
    }
    char size[32];
    snprintf(size, sizeof size, "%d \xc3\x97 %d", s.w, s.h);
    font *f = BF(13);
    int tw = text_width(f, size) + 16, ty = s.y >= 34 ? s.y - 30 : s.y + 6;
    rect tag = {s.x, ty, tw, 24};
    fill_round(A.frame, tag, 12, RGBA(0, 0, 0, 170));
    draw_text_centered(A.frame, f, tag, size, RGBA(255, 255, 255, 240));
}

/* An icon for the mode buttons: a dashed area, a screen, a window. */
static void mode_icon(rect b, int mode, color col) {
    rect r = rect_inset(b, 5);
    if (mode == MODE_AREA) {
        for (int i = 0; i < 4; i++) {
            float x0 = r.x + i * r.w / 4.0f + 1, x1 = x0 + r.w / 8.0f;
            draw_line(A.frame, x0, (float)r.y, x1, (float)r.y, 1.6f, col);
            draw_line(A.frame, x0, (float)(r.y + r.h), x1, (float)(r.y + r.h), 1.6f, col);
            float y0 = r.y + i * r.h / 4.0f + 1, y1 = y0 + r.h / 8.0f;
            draw_line(A.frame, (float)r.x, y0, (float)r.x, y1, 1.6f, col);
            draw_line(A.frame, (float)(r.x + r.w), y0, (float)(r.x + r.w), y1, 1.6f, col);
        }
    } else if (mode == MODE_SCREEN) {
        stroke_round(A.frame, (rect){r.x, r.y, r.w, r.h - 5}, 2, 1.6f, col);
        draw_line(A.frame, r.x + r.w / 2.0f, (float)(r.y + r.h - 5), r.x + r.w / 2.0f, (float)(r.y + r.h), 1.6f, col);
        draw_line(A.frame, r.x + r.w / 4.0f, (float)(r.y + r.h), r.x + r.w * 0.75f, (float)(r.y + r.h), 1.6f, col);
    } else {
        stroke_round(A.frame, r, 3, 1.6f, col);
        draw_line(A.frame, (float)r.x, r.y + 5.0f, (float)(r.x + r.w), r.y + 5.0f, 1.6f, col);
    }
}

static rect bar_rect(void) {
    int w = 720, h = 132;
    return (rect){(A.W - w) / 2, A.H - h - 36, w, h};
}

static void draw_bar(void) {
    rect b = bar_rect();
    ctl_panel(b, 22, 1);
    palette p = pal_on(pal_sample(rect_inset(b, 10)), 1);
    font *f = BF(14), *hf = HF(15);
    input_state saved = A.in;
    A.in = C.in;

    /* top row: what of the screen, and what to make */
    static const char *const modes[] = {"  Area", "  Screen", "  Window"};
    rect mr = {b.x + 18, b.y + 16, 330, 40};
    int m = ctl_segmented(mr, modes, 3, C.mode, f, &p);
    for (int i = 0; i < 3; i++)
        mode_icon((rect){mr.x + i * mr.w / 3 + 14, mr.y + 11, 20, 18}, i, color_alpha(p.ink, 0.9f));
    if (m != C.mode) {
        C.mode = m;
        if (m == MODE_WINDOW && C.nwins == 0)
            toast("There are no windows open.");
    }
    static const char *const kinds[] = {"Screenshot", "Record"};
    C.kind = ctl_segmented((rect){b.x + b.w - 18 - 230 - 56, b.y + 16, 230, 40}, kinds, 2, C.kind, f, &p);

    /* close */
    rect xr = {b.x + b.w - 50, b.y + 18, 36, 36};
    if (ctl_button(xr, "", f, false, true, &p))
        close_bar();
    ctl_symbol(A.frame, rect_inset(xr, 10), SYM_CLOSE, 1, p.ink);
    if (!C.open) {
        A.in = saved;
        return;
    }

    /* bottom row: options, and the button that takes it */
    int y = b.y + 76;
    draw_text(A.frame, f, b.x + 22, y + 9, "Pointer", p.muted);
    if (ctl_switch((rect){b.x + 90, y + 6, 46, 26}, C.pointer, &p))
        C.pointer = !C.pointer;
    if (C.kind == KIND_SHOT) {
        static const char *const delays[] = {"Now", "3 s", "5 s"};
        static const int secs[] = {0, 3, 5};
        int sel = C.delay == 5 ? 2 : C.delay == 3 ? 1 : 0;
        draw_text(A.frame, f, b.x + 160, y + 9, "Delay", p.muted);
        sel = ctl_segmented((rect){b.x + 212, y + 2, 186, 34}, delays, 3, sel, f, &p);
        C.delay = secs[sel];
    } else {
        static const char *const sounds[] = {"None", "Computer", "Mic", "Both"};
        draw_text(A.frame, f, b.x + 160, y + 9, "Sound", p.muted);
        C.sound = ctl_segmented((rect){b.x + 214, y + 2, 300, 34}, sounds, 4, C.sound, f, &p);
    }
    /* the big round button: a white ring for a picture, a red dot to record */
    float cx = b.x + b.w - 70.0f, cy = y + 19.0f;
    bool hov = (C.in.mx - cx) * (C.in.mx - cx) + (C.in.my - cy) * (C.in.my - cy) < 30 * 30;
    fill_circle(A.frame, cx, cy, 28, RGBA(255, 255, 255, hov ? 255 : 230));
    fill_circle(A.frame, cx, cy, 23, RGBA(30, 30, 34, 255));
    if (C.kind == KIND_SHOT)
        fill_circle(A.frame, cx, cy, 19, RGBA(255, 255, 255, hov ? 255 : 235));
    else
        fill_circle(A.frame, cx, cy, 15, RGBA(232, 60, 60, 255));
    const char *hint = C.kind == KIND_SHOT ? "Take" : "Start";
    draw_text(A.frame, hf, (int)cx - 38 - text_width(hf, hint), y + 8, hint, p.ink);
    bool fire = hov && C.in.clicked;
    for (int i = 0; i < C.in.nkeys; i++)
        if (C.in.keys[i] == K_ENTER)
            fire = true;
    LOGRECT("capture-bar", b);
    LOG("rect capture-go %d %d 56 56\n", (int)cx - 28, (int)cy - 28);
    A.in = saved;
    if (fire)
        go();
}

/* ---- the card ---- */

static void draw_card(void) {
    if (!C.card_path[0] || A.now - C.card_at > 7000 || C.recording || C.opening || C.now_pending)
        return;
    int tw = C.thumb ? C.thumb->w : 200, th = C.thumb ? C.thumb->h : 0;
    int w = (tw > 300 ? tw : 300) + 28, h = th + 98;
    rect r = {A.W - w - 24, A.H - h - 70, w, h};
    float age = (float)(A.now - C.card_at);
    float op = age < 220 ? age / 220 : age > 6600 ? (7000 - age) / 400 : 1;
    if (op < 0)
        op = 0;
    ctl_panel(r, 18, op);
    palette p = pal_on(pal_sample(rect_inset(r, 8)), op);
    if (C.thumb) {
        rect tr = {r.x + (r.w - tw) / 2, r.y + 14, tw, th};
        canvas_blit(A.frame, C.thumb, tr.x, tr.y);
        stroke_round(A.frame, tr, 4, 1, color_alpha(p.line, op));
        if (C.card_video) {
            float cx = tr.x + tw / 2.0f, cy = tr.y + th / 2.0f;
            fill_circle(A.frame, cx, cy, 18, RGBA(0, 0, 0, (int)(150 * op)));
            for (int k = 0; k < 12; k++) /* a play triangle */
                draw_line(A.frame, cx - 6, cy - 9 + k * 1.5f, cx + 9, cy, 1.6f, RGBA(255, 255, 255, (int)(230 * op)));
        }
    }
    font *f = BF(13), *hf = HF(14);
    const char *title = C.card_err[0] ? C.card_err : C.card_video ? "Recording saved" : "Screenshot saved";
    ctl_text_fit(A.frame, hf, r.x + 16, r.y + th + 24, r.w - 32, title, color_alpha(p.ink, op));
    const char *name = strrchr(C.card_path, '/');
    ctl_text_fit(A.frame, f, r.x + 16, r.y + th + 44, r.w - 32, name ? name + 1 : C.card_path, color_alpha(p.muted, op));
    input_state saved = A.in;
    A.in = C.open ? (input_state){0} : saved;
    if (pt_in(r, A.in.mx, A.in.my))
        C.card_at = A.now - 300; /* stays while the pointer rests on it */
    rect open_r = {r.x + r.w - 196, r.y + r.h - 40, 80, 30}, show_r = {r.x + r.w - 110, r.y + r.h - 40, 96, 30};
    /* no video player comes with the system yet: a recording is shown in Files */
    if (!C.card_err[0] && !C.card_video && ctl_button(open_r, "Open", f, true, true, &p)) {
        files_open_path(C.card_path);
        C.card_path[0] = 0;
    }
    if (C.card_path[0] && ctl_button(show_r, "Show in Files", f, false, true, &p)) {
        files_reveal(C.card_path);
        C.card_path[0] = 0;
    }
    A.in = saved;
}

/* ---- the recording pill ---- */

static void draw_pill(void) {
    if (!C.recording)
        return;
    uint64_t s = (A.now - C.rec_start) / 1000;
    char t[48];
    snprintf(t, sizeof t, "%llu:%02llu   Stop", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
    font *f = HF(14);
    int w = text_width(f, t) + 52;
    rect r = {(A.W - w) / 2, 10, w, 34};
    fill_round(A.frame, r, 17, RGBA(200, 40, 40, 235));
    bool blink = (A.now / 600) % 2 == 0;
    fill_circle(A.frame, r.x + 20.0f, r.y + 17.0f, 6, RGBA(255, 255, 255, blink ? 255 : 120));
    draw_text(A.frame, f, r.x + 36, r.y + (34 - font_line_height(f)) / 2, t, RGBA(255, 255, 255, 255));
    LOGRECT("capture-stop", r);
    if (pt_in(r, A.in.mx, A.in.my) && A.in.clicked) {
        A.in.clicked = A.in.pressed = false;
        stop_recording();
    }
}

/* ---- the frame ---- */

void capture_input(void) {
    poll_saving();
    poll_finishing();
    bool print = false;
    for (int i = 0; i < A.in.nkeys; i++) /* ours alone: no window types it */
        if (A.in.keys[i] == K_PRINT) {
            print = true;
            memmove(&A.in.keys[i], &A.in.keys[i + 1], sizeof(int) * (size_t)(A.in.nkeys - i - 1));
            A.in.nkeys--;
            i--;
        }
    if (print && C.recording)
        stop_recording();
    else if (print && !C.open && A.screen == SCR_DESKTOP)
        capture_open(-1);
    if (!C.open)
        return;
    /* the bar has the pointer and the keys; the desktop below sees none */
    C.in = A.in;
    A.in.clicked = A.in.pressed = A.in.right_clicked = A.in.down = false;
    A.in.nkeys = 0;
    A.in.nraw = 0;
    A.in.wheel = 0;
    A.in.mx = A.in.my = -10000;
}

void capture_draw(void) {
    /* a screenshot on a countdown: the screen as it is now */
    if (C.shot_at && A.now >= C.shot_at) {
        C.shot_at = 0;
        take_screenshot(A.frame, C.shot_r, C.pointer, A.in.mx, A.in.my);
    } else if (C.shot_at) {
        char t[16];
        snprintf(t, sizeof t, "%llu", (unsigned long long)((C.shot_at - A.now + 999) / 1000));
        fill_circle(A.frame, A.W / 2.0f, A.H / 2.0f, 46, RGBA(0, 0, 0, 150));
        draw_text_centered(A.frame, HF(40), (rect){A.W / 2 - 46, A.H / 2 - 46, 92, 92}, t, RGBA(255, 255, 255, 255));
    }
    if (C.now_pending && --C.now_pending == 0)
        take_screenshot(A.frame, (rect){0, 0, A.W, A.H}, false, 0, 0);
    if (C.opening > 1) {
        C.opening--;
    } else if (C.opening == 1) {
        C.opening = 0;
        C.still = canvas_new(A.W, A.H);
        if (C.still) {
            canvas_blit(C.still, A.frame, 0, 0);
            C.open = true;
            C.mx = A.in.mx, C.my = A.in.my;
            list_windows();
            LOG("capture: bar open (%s)\n", C.kind == KIND_SHOT ? "screenshot" : "record");
        }
        return; /* from the next frame on */
    }
    if (!C.open) {
        draw_card();
        draw_pill();
        return;
    }
    A.in = C.in; /* the real input again, for the pointer drawn after us */
    canvas_blit(A.frame, C.still, 0, 0);
    rect bar = bar_rect();
    bool over_bar = pt_in(bar, C.in.mx, C.in.my);
    if (C.mode == MODE_AREA) {
        if (C.drag)
            drag_area();
        else if (C.in.pressed && !over_bar) {
            int h = C.sel.w ? handle_at(C.sel, C.in.mx, C.in.my) : 0;
            C.drag = h ? h : 1;
            C.ax = C.in.mx, C.ay = C.in.my;
            C.drag_from = C.sel;
        }
        rect s = clamp_rect(C.sel);
        if (s.w > 0 && s.h > 0) {
            dim_outside(s, 0.45f);
            draw_area(s, !C.drag || C.drag >= 2);
        } else {
            fill_rect(A.frame, (rect){0, 0, A.W, A.H}, RGBA(0, 0, 0, 115));
            draw_text_centered(A.frame, HF(22), (rect){0, A.H / 2 - 40, A.W, 40}, "Drag over the part of the screen you want",
                               RGBA(255, 255, 255, 235));
        }
    } else if (C.mode == MODE_WINDOW) {
        if (!over_bar)
            for (int i = 0; i < C.nwins; i++)
                if (pt_in(C.wins[i], C.in.mx, C.in.my)) {
                    if (C.in.clicked || C.in.mx != C.mx || C.in.my != C.my)
                        C.win_pick = i;
                    break;
                }
        if (C.win_pick >= 0) {
            rect s = clamp_rect(C.wins[C.win_pick]);
            dim_outside(s, 0.45f);
            draw_area(s, false);
        } else {
            fill_rect(A.frame, (rect){0, 0, A.W, A.H}, RGBA(0, 0, 0, 115));
            draw_text_centered(A.frame, HF(22), (rect){0, A.H / 2 - 40, A.W, 40}, "No windows are open",
                               RGBA(255, 255, 255, 235));
        }
    } else {
        stroke_round(A.frame, (rect){1, 1, A.W - 2, A.H - 2}, 0, 3, RGBA(255, 255, 255, 200));
    }
    for (int i = 0; i < C.in.nkeys; i++)
        if (C.in.keys[i] == K_ESC) {
            close_bar();
            return;
        }
    draw_bar();
}

/* The finished frame (before the pointer is drawn, and after): what a
 * recording takes, with or without the pointer as chosen. */
void capture_frame(bool pointer_drawn) {
    if (C.recording && pointer_drawn == C.pointer)
        record_frame();
}

void capture_screenshot_now(void) { C.now_pending = 4; }
