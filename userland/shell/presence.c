/* The companion's presence: a light, or a face in a round frame drawn by
 * os-figure in a process of its own (a crash there shows the light again
 * and it starts over, a little later each time).
 *
 * Frames come through shared memory (three slots): os-figure says which
 * slot holds a new picture, the shell shows it and gives the one before
 * back. Commands go the other way as lines of text (see userland/figure
 * /main.c). The shell tells her where the pointer is, when she is thinking
 * (the model is working), listening (the microphone is on) and speaking,
 * and how loud her voice is this moment and which vowel it is (os-play
 * writes that to /run/os-lip as it plays), so her mouth moves with it. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "caption.h"
#include "fm/fm.h"
#include "presence.h"
#include "voice.h"
#include "appapi.h"
#include "emblem.h"
#include "body.h"

#define D (A.d)
#define FIGURE "/usr/libexec/os-figure"
#define PRESENCE_FILE STATE_DIR "/presence"
#define LOOK_FILE STATE_DIR "/figure.xml"
#define VRM_FILE STATE_DIR "/figure.vrm" /* her own model, if they brought one */
#define MODEL_FILE STATE_DIR "/emblem-model.glb" /* a 3D model as her emblem (any object) */
#define NSLOT 3
#define MAX_SIDE 640 /* the largest picture asked for; bigger homes are scaled up */
#define LIP_PATH "/run/os-lip"
#define LIP_N 512

struct lip_entry {
    double t; /* CLOCK_MONOTONIC seconds: when this is heard */
    float level, f1, f2;
};
struct lip_file {
    uint32_t magic, next;
    struct lip_entry e[LIP_N];
};

static struct {
    int mode;
    pid_t pid;
    int to, from;
    uint8_t *ring;
    size_t slot_bytes;
    bool ready;
    int shown; /* slot on screen, -1 none */
    int fw, fh;
    float hx, hy, hr;
    char rbuf[4096];
    size_t rlen;
    uint64_t restart_at;
    int fails;
    uint64_t started_at;
    /* what she was last told */
    int view_w, view_h;
    bool hidden;
    uint64_t home_at; /* when a home was last drawn */
    rect home;
    float px, py;
    bool pointer_on;
    uint64_t pointer_moved, pointer_sent;
    int last_mx, last_my;
    bool thinking, listening, speaking;
    float lip_l, lip_1, lip_2;
    const struct lip_file *lip;
    char look[16384];
    bool look_loaded;
} P = {.pid = -1, .to = -1, .from = -1, .shown = -1};

static double mono_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void tell(const char *fmt, ...) {
    if (P.to < 0)
        return;
    char b[640];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof b - 2)
        n = (int)sizeof b - 2;
    b[n++] = '\n';
    if (write(P.to, b, (size_t)n) != n) { /* a full pipe or a dead process: the poll notices */
    }
}

/* ---- the look ---- */

const char *presence_look(void) {
    if (!P.look_loaded) {
        P.look_loaded = true;
        P.look[0] = 0;
        FILE *f = fopen(LOOK_FILE, "r");
        if (f) {
            size_t n = fread(P.look, 1, sizeof P.look - 1, f);
            P.look[n] = 0;
            fclose(f);
        }
    }
    return P.look;
}

static void send_look(void) {
    const char *lk = presence_look();
    if (!lk[0] || P.to < 0)
        return;
    tell("look-begin");
    /* line by line: the figure gathers until look-end */
    const char *s = lk;
    while (*s) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char line[600];
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), s);
        if (strcmp(line, "look-end"))
            tell("%s", line);
        s = e ? e + 1 : s + n;
    }
    tell("look-end");
}

bool presence_set_look(const char *markup) {
    if (!markup || !strstr(markup, "<figure"))
        return false;
    snprintf(P.look, sizeof P.look, "%s", markup);
    P.look_loaded = true;
    FILE *f = fopen(LOOK_FILE, "w");
    if (f) {
        fputs(P.look, f);
        fclose(f);
    }
    send_look();
    return true;
}

/* ---- the process ---- */

/* Shown as a light, but the light is a 3D model of theirs: the figure process
 * draws it (a turning object), and the emblem shows its frames. */
static bool model_mode(void) {
    return P.mode == PRESENCE_LIGHT && emblem_kind() == EM_MODEL && access(MODEL_FILE, R_OK) == 0;
}

static void stop_figure(void) {
    if (P.pid > 0) {
        kill(P.pid, SIGTERM);
        waitpid(P.pid, NULL, 0);
    }
    P.pid = -1;
    if (P.to >= 0)
        close(P.to);
    if (P.from >= 0)
        close(P.from);
    P.to = P.from = -1;
    if (P.ring)
        munmap(P.ring, P.slot_bytes * NSLOT);
    P.ring = NULL;
    P.ready = false;
    P.shown = -1;
    P.rlen = 0;
    P.view_w = P.view_h = 0;
}

static bool start_figure(void) {
    if (access(FIGURE, X_OK) != 0)
        return false;
    P.slot_bytes = (size_t)MAX_SIDE * MAX_SIDE * 4;
    int mfd = memfd_create("os-figure-frames", MFD_CLOEXEC);
    if (mfd < 0 || ftruncate(mfd, (off_t)(P.slot_bytes * NSLOT)) < 0) {
        if (mfd >= 0)
            close(mfd);
        return false;
    }
    P.ring = mmap(NULL, P.slot_bytes * NSLOT, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (P.ring == MAP_FAILED) {
        P.ring = NULL;
        close(mfd);
        return false;
    }
    int to[2], from[2];
    if (pipe2(to, O_CLOEXEC) < 0) {
        close(mfd);
        stop_figure();
        return false;
    }
    if (pipe2(from, O_CLOEXEC) < 0) {
        close(to[0]), close(to[1]), close(mfd);
        stop_figure();
        return false;
    }
    char sb[32], ns[8];
    snprintf(sb, sizeof sb, "%zu", P.slot_bytes);
    snprintf(ns, sizeof ns, "%d", NSLOT);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        dup2(mfd, 3);
        int vrm = open(model_mode() ? MODEL_FILE : VRM_FILE, O_RDONLY); /* her own body, or the emblem's model */
        if (vrm >= 0 && vrm != 4) {
            dup2(vrm, 4);
            close(vrm);
        }
        for (int fd = vrm >= 0 ? 5 : 4; fd < 256; fd++)
            close(fd);
        /* nobody needs it to be root */
        if (setgid(SESSION_UID) < 0 || setuid(SESSION_UID) < 0) {
        }
        execl(FIGURE, "os-figure", sb, ns, (char *)NULL);
        _exit(127);
    }
    close(to[0]), close(from[1]), close(mfd);
    if (pid < 0) {
        close(to[1]), close(from[0]);
        munmap(P.ring, P.slot_bytes * NSLOT);
        P.ring = NULL;
        return false;
    }
    fcntl(from[0], F_SETFL, O_NONBLOCK);
    fcntl(to[1], F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    P.pid = pid, P.to = to[1], P.from = from[0];
    P.started_at = A.now;
    P.thinking = P.listening = P.speaking = false;
    P.pointer_on = false;
    LOG("presence: os-figure started (pid %d)\n", pid);
    return true;
}

static int read_mode(void) {
    FILE *f = fopen(PRESENCE_FILE, "r");
    char w[16] = "";
    if (f) {
        if (!fgets(w, sizeof w, f))
            w[0] = 0;
        fclose(f);
    }
    if (!strncmp(w, "face", 4))
        return PRESENCE_FACE;
    if (!strncmp(w, "wander", 6))
        return PRESENCE_WANDER;
    if (!w[0] && access(VRM_FILE, R_OK) == 0) /* they brought a body and chose nothing else */
        return PRESENCE_FACE;
    return PRESENCE_LIGHT;
}

void presence_init(void) {
    P.mode = read_mode();
    LOG("presence: %s\n", P.mode == PRESENCE_LIGHT ? "a light" : P.mode == PRESENCE_FACE ? "a face" : "a face that wanders");
    if (P.mode != PRESENCE_LIGHT || model_mode())
        P.restart_at = A.now; /* started by the first poll */
}

int presence_mode(void) { return P.mode; }

void presence_set_mode(int mode) {
    if (mode == P.mode)
        return;
    P.mode = mode;
    FILE *f = fopen(PRESENCE_FILE, "w");
    if (f) {
        fputs(mode == PRESENCE_FACE ? "face\n" : mode == PRESENCE_WANDER ? "wander\n" : "light\n", f);
        fclose(f);
    }
    if (mode == PRESENCE_LIGHT)
        stop_figure();
    if (P.pid < 0 && (mode != PRESENCE_LIGHT || model_mode()))
        P.fails = 0, P.restart_at = A.now;
}

void presence_model_changed(void) {
    stop_figure(); /* it starts again with the new one (or not at all) */
    P.fails = 0;
    P.restart_at = model_mode() || P.mode != PRESENCE_LIGHT ? A.now : 0;
}

bool presence_set_body(const char *vrm) {
    if (vrm) { /* their own model: copied in whole, then swapped in */
        int in = open(vrm, O_RDONLY | O_CLOEXEC), out = open(VRM_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        static char buf[1 << 16];
        ssize_t n = 0;
        bool ok = in >= 0 && out >= 0;
        while (ok && (n = read(in, buf, sizeof buf)) > 0)
            ok = write(out, buf, (size_t)n) == n;
        ok = ok && n == 0;
        if (in >= 0)
            close(in);
        if (out >= 0)
            close(out);
        if (!ok || rename(VRM_FILE ".new", VRM_FILE) < 0) {
            unlink(VRM_FILE ".new");
            LOG("presence: couldn't take %s\n", vrm);
            return false;
        }
        LOG("presence: her body is now %s\n", vrm);
    } else {
        unlink(VRM_FILE); /* the face the OS makes for her */
        LOG("presence: her body is the one the OS makes\n");
    }
    if (P.pid > 0) /* she comes back in her new body */
        stop_figure();
    P.fails = 0, P.restart_at = A.now;
    presence_set_mode(PRESENCE_FACE);
    return true;
}

/* ---- what she says back ---- */

static void heard(char *line) {
    int s, w, h;
    float hx, hy, hr;
    if (sscanf(line, "frame %d %d %d %f %f %f", &s, &w, &h, &hx, &hy, &hr) == 6) {
        if (s < 0 || s >= NSLOT || (size_t)w * h * 4 > P.slot_bytes)
            return;
        if (P.shown >= 0 && P.shown != s)
            tell("ack %d", P.shown);
        P.shown = s, P.fw = w, P.fh = h, P.hx = hx, P.hy = hy, P.hr = hr;
        A.dirty = true;
        return;
    }
    if (!strncmp(line, "ready", 5)) {
        P.ready = true;
        P.fails = 0;
        send_look();
        return;
    }
    LOG("presence: os-figure: %s\n", line);
}

static void read_figure(void) {
    for (;;) {
        if (P.rlen >= sizeof P.rbuf - 1)
            P.rlen = 0;
        ssize_t n = read(P.from, P.rbuf + P.rlen, sizeof P.rbuf - 1 - P.rlen);
        if (n <= 0)
            return;
        P.rlen += (size_t)n;
        char *start = P.rbuf, *nl;
        while ((nl = memchr(start, '\n', P.rlen - (size_t)(start - P.rbuf)))) {
            *nl = 0;
            heard(start);
            start = nl + 1;
        }
        P.rlen -= (size_t)(start - P.rbuf);
        memmove(P.rbuf, start, P.rlen);
    }
}

/* ---- her voice, as os-play measured it ---- */

static void lip_update(void) {
    if (!P.speaking) {
        if (P.lip_l > 0)
            tell("speak 0 0 0");
        P.lip_l = 0;
        return;
    }
    if (!P.lip) {
        int fd = open(LIP_PATH, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return;
        void *m = mmap(NULL, sizeof(struct lip_file), PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED)
            return;
        P.lip = m;
    }
    if (P.lip->magic != 0x4c495031u) /* "LIP1" */
        return;
    double now = mono_s();
    uint32_t next = P.lip->next;
    const struct lip_entry *best = NULL;
    for (uint32_t k = 1; k <= 64 && k <= next; k++) {
        const struct lip_entry *e = &P.lip->e[(next - k) % LIP_N];
        if (e->t <= now && now - e->t < 0.05) {
            best = e;
            break;
        }
    }
    float l = best ? best->level : 0, f1 = best ? best->f1 : 0, f2 = best ? best->f2 : 0;
    if (fabsf(l - P.lip_l) > 0.03f || fabsf(f1 - P.lip_1) > 0.05f || fabsf(f2 - P.lip_2) > 0.05f) {
        tell("speak %.2f %.2f %.2f", l, f1, f2);
        P.lip_l = l, P.lip_1 = f1, P.lip_2 = f2;
    }
}

/* ---- every pass ---- */

void presence_poll(void) {
    if (P.mode == PRESENCE_LIGHT && !model_mode())
        return;
    if (P.pid > 0) {
        int st;
        if (waitpid(P.pid, &st, WNOHANG) == P.pid) {
            LOG("presence: os-figure stopped (%s %d); the light for now\n", WIFSIGNALED(st) ? "signal" : "status",
                WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
            P.pid = -1;
            stop_figure();
            P.fails++;
            uint64_t wait = 2000u << (P.fails < 5 ? P.fails : 5); /* 4 s, 8 s ... about a minute */
            P.restart_at = A.now + wait;
        }
    }
    if (P.pid < 0) {
        if (P.restart_at && A.now >= P.restart_at) {
            P.restart_at = 0;
            if (!start_figure()) {
                P.fails++;
                P.restart_at = A.now + 60000;
            }
        }
        return;
    }
    read_figure();
    if (!P.ready)
        return;
    /* not on screen for a moment: she rests */
    bool on_screen = A.screen == SCR_DESKTOP && A.now - P.home_at < 600;
    if (!on_screen && !P.hidden) {
        tell("hide");
        P.hidden = true;
        P.view_w = 0;
    }
    if (!on_screen)
        return;
    /* the pointer: she follows it while it moves, then looks around again */
    if (A.in.mx != P.last_mx || A.in.my != P.last_my)
        P.pointer_moved = A.now, P.last_mx = A.in.mx, P.last_my = A.in.my;
    bool follow = A.now - P.pointer_moved < 6000;
    if (follow && P.home.w > 0 && A.now - P.pointer_sent > 40) {
        float r = P.home.w * 0.5f;
        float x = (A.in.mx - (P.home.x + r)) / (r * 3), y = ((P.home.y + r) - A.in.my) / (r * 3);
        x = x < -1.2f ? -1.2f : x > 1.2f ? 1.2f : x;
        y = y < -1.2f ? -1.2f : y > 1.2f ? 1.2f : y;
        if (!P.pointer_on || fabsf(x - P.px) > 0.02f || fabsf(y - P.py) > 0.02f) {
            tell("pointer %.2f %.2f", x, y);
            appapi_gaze(x, y);
            P.px = x, P.py = y, P.pointer_on = true, P.pointer_sent = A.now;
        }
    } else if (!follow && P.pointer_on) {
        tell("pointer off");
        P.pointer_on = false;
    }
    bool thinking = A.companion.pending != NULL, listening = voice_listening(), speaking = voice_speaking();
    if (thinking != P.thinking)
        tell("think %d", thinking), P.thinking = thinking;
    if (listening != P.listening)
        tell("listen %d", listening), P.listening = listening;
    if (speaking != P.speaking)
        tell("speaking %d", speaking), P.speaking = speaking;
    lip_update();
}

int presence_fds(int *fds, int max) {
    if (P.from < 0 || max < 1)
        return 0;
    fds[0] = P.from;
    return 1;
}

/* ---- drawing her home ---- */

static inline uint32_t sample(const uint32_t *px, int w, int h, float x, float y) {
    /* bilinear, premultiplied */
    x -= 0.5f, y -= 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    uint32_t c[4];
    for (int k = 0; k < 4; k++) {
        int sx = x0 + (k & 1), sy = y0 + (k >> 1);
        c[k] = sx < 0 || sy < 0 || sx >= w || sy >= h ? 0 : px[sy * w + sx];
    }
    float wt[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    float a = 0, r = 0, g = 0, b = 0;
    for (int k = 0; k < 4; k++)
        a += C_A(c[k]) * wt[k], r += C_R(c[k]) * wt[k], g += C_G(c[k]) * wt[k], b += C_B(c[k]) * wt[k];
    return (uint32_t)(a + 0.5f) << 24 | (uint32_t)(r + 0.5f) << 16 | (uint32_t)(g + 0.5f) << 8 | (uint32_t)(b + 0.5f);
}

static uint64_t orb_home_at; /* when a design's orb last held her */

enum { HOME_ROUND, HOME_RECT, HOME_FREE };
enum { TINT_NONE, TINT_CODEC };

static bool draw_home(canvas *c, rect r, bool active, int shape, int tint, const char *shot);

bool presence_draw_home(canvas *c, rect r, bool active) {
    if (P.mode == PRESENCE_LIGHT || P.pid < 0 || !P.ready)
        return false;
    int side = r.w < r.h ? r.w : r.h;
    if (side < 64)
        return false; /* too small for a face: a light */
    orb_home_at = A.now;
    return draw_home(c, r, active, HOME_ROUND, TINT_NONE, NULL);
}

bool presence_draw_avatar(canvas *c, rect r, bool active, const char *shape, const char *tint, const char *shot) {
    if (P.mode == PRESENCE_LIGHT || P.pid < 0 || !P.ready || r.w < 48 || r.h < 48)
        return false;
    orb_home_at = A.now;
    return draw_home(c, r, active, shape && !strcasecmp(shape, "circle") ? HOME_ROUND : HOME_RECT,
                     tint && !strcasecmp(tint, "codec") ? TINT_CODEC : TINT_NONE, shot);
}

/* her own frame, when the design has none big enough: in a corner at
 * first, then wherever it was dragged to (kept on the disk) */
#define HOME_POS_FILE STATE_DIR "/presence-home"
static struct {
    bool loaded, have;
    int x, y;            /* centre, as a share of the screen x10000 */
    bool dragging, moved;
    int gx, gy;          /* where in the frame it was taken hold of */
    int px, py;          /* the press */
} H;

void presence_draw_corner(canvas *c) {
    if (P.mode == PRESENCE_LIGHT || P.pid < 0 || !P.ready || A.now - orb_home_at < 400)
        return;
    int side = A.H >= 900 ? 220 : A.H >= 700 ? 180 : 140;
    if (!H.loaded) {
        H.loaded = true;
        FILE *f = fopen(HOME_POS_FILE, "r");
        if (f) {
            H.have = fscanf(f, "%d %d", &H.x, &H.y) == 2;
            fclose(f);
        }
    }
    rect r = {A.W - side - 28, A.H - side - 72, side, side};
    if (H.have)
        r.x = (int)((long)H.x * A.W / 10000) - side / 2, r.y = (int)((long)H.y * A.H / 10000) - side / 2;
    /* taken hold of: dragged about; a click without moving opens the conversation */
    float cx = r.x + side * 0.5f, cy = r.y + side * 0.5f;
    float dx = A.in.mx - cx, dy = A.in.my - cy;
    bool over = dx * dx + dy * dy <= side * side * 0.25f;
    if (A.in.pressed && over && !H.dragging) {
        H.dragging = true, H.moved = false;
        H.gx = A.in.mx - r.x, H.gy = A.in.my - r.y, H.px = A.in.mx, H.py = A.in.my;
        A.in.pressed = false; /* ours, not the desktop's */
    }
    if (H.dragging) {
        if (abs(A.in.mx - H.px) + abs(A.in.my - H.py) > 4)
            H.moved = true;
        if (H.moved) {
            r.x = A.in.mx - H.gx, r.y = A.in.my - H.gy;
            if (r.x < 0) r.x = 0;
            if (r.y < 0) r.y = 0;
            if (r.x > A.W - side) r.x = A.W - side;
            if (r.y > A.H - side) r.y = A.H - side;
            H.have = true;
            H.x = (int)((long)(r.x + side / 2) * 10000 / A.W), H.y = (int)((long)(r.y + side / 2) * 10000 / A.H);
        }
        if (!A.in.down) {
            H.dragging = false;
            A.in.clicked = false;
            if (H.moved) {
                FILE *f = fopen(HOME_POS_FILE, "w");
                if (f) {
                    fprintf(f, "%d %d\n", H.x, H.y);
                    fclose(f);
                }
            } else
                fm_host_action(A.ui, NULL, "open:companion");
        }
    }
    draw_home(c, r, A.companion.pending != NULL || H.dragging, HOME_ROUND, TINT_NONE, NULL);
}

static bool draw_home(canvas *c, rect r, bool active, int shape, int tint, const char *want_shot) {
    rect sq = r;
    if (shape == HOME_ROUND) {
        int side = r.w < r.h ? r.w : r.h;
        sq = (rect){r.x + (r.w - side) / 2, r.y + (r.h - side) / 2, side, side};
    }
    P.home = sq;
    P.home_at = A.now;
    /* the picture asked for: the frame's size, within what a slot holds */
    int ww = sq.w, wh = sq.h;
    if (ww > MAX_SIDE || wh > MAX_SIDE) {
        float k = (float)MAX_SIDE / (ww > wh ? ww : wh);
        ww = (int)(ww * k), wh = (int)(wh * k);
    }
    static char last_shot[12];
    const char *ws = want_shot && (!strcasecmp(want_shot, "face") || !strcasecmp(want_shot, "bust") ||
                                   !strcasecmp(want_shot, "waist") || !strcasecmp(want_shot, "full"))
                         ? want_shot
                         : "";
    if (P.hidden || ww != P.view_w || wh != P.view_h || strcmp(ws, last_shot)) {
        snprintf(last_shot, sizeof last_shot, "%s", ws);
        int small = ww < wh ? ww : wh;
        /* chosen by the design (shot=), else close in: head and shoulders */
        const char *shot = ws[0] ? ws : small < 140 ? "face" : "bust";
        /* the design's colours light her: its accent at her edges */
        tell("light fff6ee %06x %06x", D.bg & 0xffffff, D.accent & 0xffffff);
        tell("view %d %d %s", ww, wh, shot);
        P.view_w = ww, P.view_h = wh;
        P.hidden = false;
    }
    if (P.shown < 0)
        return true; /* the first frame is on its way: an empty frame for a moment */
    const uint32_t *px = (const uint32_t *)(P.ring + (size_t)P.shown * P.slot_bytes);
    float cx = sq.x + sq.w * 0.5f, cy = sq.y + sq.h * 0.5f, rad = (sq.w < sq.h ? sq.w : sq.h) * 0.5f;
    float sx = (float)P.fw / sq.w, sy = (float)P.fh / sq.h;
    if (shape == HOME_FREE) { /* an object on its own: only its pixels, over whatever is there */
        for (int y = sq.y; y < sq.y + sq.h; y++) {
            if (y < c->cy0 || y >= c->cy1)
                continue;
            for (int x = sq.x; x < sq.x + sq.w; x++) {
                if (x < c->cx0 || x >= c->cx1)
                    continue;
                uint32_t f = sample(px, P.fw, P.fh, (x - sq.x + 0.5f) * sx, (y - sq.y + 0.5f) * sy);
                uint32_t a = C_A(f);
                if (!a)
                    continue;
                uint32_t *dst = &c->px[(size_t)y * c->w + x], d = *dst;
                int rr = (int)(C_R(f) + C_R(d) * (255 - a) / 255), gg = (int)(C_G(f) + C_G(d) * (255 - a) / 255),
                    bb = (int)(C_B(f) + C_B(d) * (255 - a) / 255);
                *dst = RGBA(rr > 255 ? 255 : rr, gg > 255 ? 255 : gg, bb > 255 ? 255 : bb, 255);
            }
        }
        return true;
    }
    /* behind her: a deep glow in the design's colours (a codec screen's dark green) */
    color deep = color_mix(D.bg, RGBA(10, 8, 18, 255), 0.55f), glow = color_mix(D.accent, D.bg, 0.55f);
    if (tint == TINT_CODEC)
        deep = RGBA(4, 18, 12, 255), glow = RGBA(16, 60, 42, 255);
    color ink_dark = RGBA(3, 16, 10, 255), ink_light = RGBA(176, 255, 214, 255);
    for (int y = sq.y; y < sq.y + sq.h; y++) {
        if (y < c->cy0 || y >= c->cy1)
            continue;
        for (int x = sq.x; x < sq.x + sq.w; x++) {
            if (x < c->cx0 || x >= c->cx1)
                continue;
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, cov = 1;
            float t, up = (dy / (sq.h * 0.5f) + 1) * 0.5f;
            if (shape == HOME_ROUND) {
                float d = sqrtf(dx * dx + dy * dy);
                cov = rad - d + 0.5f;
                if (cov <= 0)
                    continue;
                if (cov > 1)
                    cov = 1;
                t = d / rad;
            } else
                t = fmaxf(fabsf(dx) / (sq.w * 0.5f), fabsf(dy) / (sq.h * 0.5f));
            color bgc = color_mix(glow, deep, t * 0.8f + up * 0.3f);
            uint32_t f = sample(px, P.fw, P.fh, (x - sq.x + 0.5f) * sx, (y - sq.y + 0.5f) * sy);
            float fa = C_A(f) / 255.0f;
            int rr = (int)(C_R(f) + C_R(bgc) * (1 - fa)), gg = (int)(C_G(f) + C_G(bgc) * (1 - fa)),
                bb = (int)(C_B(f) + C_B(bgc) * (1 - fa));
            rr = rr > 255 ? 255 : rr, gg = gg > 255 ? 255 : gg, bb = bb > 255 ? 255 : bb;
            color in = RGBA(rr, gg, bb, 255);
            if (tint == TINT_CODEC) {
                /* a codec screen: one green, by brightness, and scan lines */
                float lum = (0.3f * rr + 0.59f * gg + 0.11f * bb) / 255.0f;
                in = color_mix(ink_dark, ink_light, powf(lum, 0.85f));
                if ((y - sq.y) % 3 == 0)
                    in = color_mix(in, ink_dark, 0.35f);
            }
            uint32_t *dst = &c->px[(size_t)y * c->w + x];
            *dst = cov >= 1 ? in : color_mix(*dst | 0xff000000u, in, cov);
        }
    }
    color rim = active ? color_mix(D.accent, RGBA(255, 255, 255, 255), 0.25f) : color_mix(D.accent, D.surface, 0.35f);
    if (shape == HOME_ROUND) { /* the rim: the design's accent, brighter while she listens or thinks */
        float rimw = sq.w * 0.022f + 1.5f;
        draw_arc(c, cx, cy, rad - rimw * 0.5f, 0, 2 * 3.14159265f, rimw, rim);
    }
    caption_anchor(sq);
    return true;
}

/* ---- her tags ---- */

static void take_tag(char *text, const char *open, bool act, void (*fn)(const char *arg)) {
    char *o;
    size_t ol = strlen(open);
    while ((o = strstr(text, open))) {
        char *e = strstr(o, "]]");
        char arg[2048];
        snprintf(arg, sizeof arg, "%.*s", e ? (int)(e - o - ol) : 0, o + ol);
        char *after = e ? e + 2 : o + strlen(o);
        memmove(o, after, strlen(after) + 1);
        if (act && e && fn)
            fn(arg);
    }
}

static bool tags_live; /* her face is up to show what the tags say */

static void do_act(const char *a) {
    char w[24] = "";
    sscanf(a, "%23s", w);
    if (w[0]) {
        if (tags_live)
            tell("act %s", w);
        LOG("presence: act %s\n", w);
        appapi_avatar_event("act", w);
    }
}

static void do_mood(const char *a) {
    char w[24] = "";
    sscanf(a, "%23s", w);
    if (w[0]) {
        if (tags_live)
            tell("mood %s", w);
        appapi_avatar_event("mood", w);
    }
}

bool presence_avatar(const char *kind, const char *name) {
    char w[24] = "";
    sscanf(name, "%23[a-z_-]", w);
    if (!w[0] || P.mode == PRESENCE_LIGHT || P.pid <= 0)
        return false;
    tell("%s %s", !strcmp(kind, "act") ? "act" : "mood", w);
    return true;
}

/* [[emblem:gem]] (or a light, an 8-ball, the Earth...), [[emblem:face]]: how it looks, when they ask */
static void do_emblem(const char *a) {
    char w[40] = "";
    sscanf(a, " %39[^]]", w);
    for (char *p = w + strlen(w); p > w && p[-1] == ' '; )
        *--p = 0;
    if (!strcasecmp(w, "face") || !strcasecmp(w, "figure")) {
        presence_set_body(NULL);
        return;
    }
    /* a picture or model (a file or a link), or pictures to find online */
    static const char *const how[] = {"picture", "model", "find"};
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(how[i]);
        if (!strncasecmp(a + strspn(a, " "), how[i], l) && a[strspn(a, " ") + l] == ' ') {
            body_use(how[i], a + strspn(a, " ") + l + 1);
            return;
        }
    }
    int k = emblem_from_name(w);
    if (k < 0) {
        LOG("presence: no emblem called %s\n", w);
        return;
    }
    presence_set_mode(PRESENCE_LIGHT);
    emblem_set(k);
}

static void do_figure(const char *a) {
    if (strstr(a, "<figure"))
        presence_set_look(a);
}

void presence_tags(char *text, bool act) {
    /* [[emblem:draw <canvas>...script...</canvas>]]: a drawing of itself. The
     * script may hold "]]" of its own, so it runs to </canvas> first. */
    char *dr;
    while ((dr = strstr(text, "[[emblem:draw"))) {
        char *open = strstr(dr, "<canvas"), *close = open ? strstr(open, "</canvas>") : NULL;
        char *end = close ? strstr(close, "]]") : strstr(dr, "]]");
        if (open && close && act) {
            char *gt = strchr(open, '>');
            if (gt && gt < close) {
                size_t n = (size_t)(close - gt - 1);
                char *script = malloc(n + 1);
                if (script) {
                    memcpy(script, gt + 1, n);
                    script[n] = 0;
                    if (emblem_set_drawing(script))
                        presence_set_mode(PRESENCE_LIGHT);
                    free(script);
                }
            }
        }
        char *after = end ? end + 2 : dr + strlen(dr);
        memmove(dr, after, strlen(after) + 1);
    }
    tags_live = P.mode != PRESENCE_LIGHT && P.pid > 0;
    /* apps following her face hear them even while she is a light */
    take_tag(text, "[[act:", act, do_act);
    take_tag(text, "[[mood:", act, do_mood);
    take_tag(text, "[[figure:", act && P.mode != PRESENCE_LIGHT, do_figure);
    take_tag(text, "[[emblem:", act, do_emblem);
}

const char *presence_prompt(void) {
    static char buf[2400];
    snprintf(buf, sizeof buf,
             "\nHow you look is theirs to choose. When they ask you to look different, end your reply with one of: "
             "[[emblem:gem]] (or orb, 8-ball, earth, planet, heart, moon; face for an anime figure); "
             "[[emblem:picture https://...]] or [[emblem:picture /root/Pictures/x.jpg]] for a picture they give you; "
             "[[emblem:find steam train]] to search pictures online and let them pick one; "
             "[[emblem:model https://.../thing.glb]] for a 3D model (a single .glb file); or, for anything they describe, "
             "draw yourself: [[emblem:draw <canvas>function draw(w, h, t) ... end</canvas>]], a Lua script painting a "
             "w by h square with g.circle(x,y,r,col), g.glow(x,y,r,col), g.rect(x,y,w,h,col,radius), "
             "g.gradient(x,y,w,h,top,bottom), g.line(x0,y0,x1,y1,col,width), g.arc(x,y,r,a0,a1,col,width), "
             "g.poly({x0,y0,x1,y1,...},col), g.text(x,y,str,col,size); colours as \"#rrggbb\" or \"#rrggbbaa\". The "
             "globals state (\"idle\", \"thinking\", \"listening\", \"speaking\", \"working\", \"done\", \"error\"), "
             "level (your voice, 0..1), accent and accent2 (the design's colours) let it come alive: make it move "
             "while thinking and pulse with level while speaking. Keep it small, cute and clean. "
             "Right now you are %s%s.",
             P.mode != PRESENCE_LIGHT ? "" : emblem_kind() == EM_ORB ? "" : "a ",
             P.mode != PRESENCE_LIGHT ? "an anime figure (a face)" : emblem_kind() == EM_ORB ? "a light"
                                                                                           : emblem_name(emblem_kind()));
    if (P.mode == PRESENCE_LIGHT)
        return buf;
    static char face[3600];
    snprintf(face, sizeof face, "%s%s",
             "\nYou have a body: an anime-style figure in a round frame on their desktop, who looks at the pointer, "
             "breathes, blinks and moves her mouth as you speak. [[act:wave]] (or nod, shake, bow, clap, point, shrug, "
             "laugh, cheer, yawn, stretch, tilt) makes a gesture; [[mood:happy]] (or calm, sad, excited, sleepy, curious, "
             "proud, worried) sets your face until you change it. Where the design shows you in an <avatar>, its shot "
             "attribute is how close the camera is: [[ui:set #id shot=face]] (head and shoulders, closest), bust, waist "
             "or full. Use them now and then, when they fit what you say; never describe your own movements in words.",
             buf);
    return face;
}

bool presence_draw_model(canvas *c, rect r) {
    if (!model_mode() || P.pid < 0 || !P.ready)
        return false;
    return draw_home(c, r, false, HOME_FREE, TINT_NONE, "full");
}
