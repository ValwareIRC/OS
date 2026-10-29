#include "sys.h"
#include "keymap.h"
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Linux UAPI definitions (linux/fb.h, linux/input.h); musl's toolchain does
 * not ship kernel headers, and these layouts are fixed by the ABI. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FB_ACTIVATE_TEST 2
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
#define EV_REL 2
#define EV_ABS 3
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8
#define ABS_X 0
#define ABS_Y 1
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define KEY_ESC 1
#define KEY_SPACE 57
#define KEY_BACKSPACE 14
#define KEY_TAB 15
#define KEY_ENTER 28
#define KEY_LEFTCTRL 29
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_KPENTER 96
#define KEY_RIGHTCTRL 97
#define KEY_HOME 102
#define KEY_UP 103
#define KEY_PAGEUP 104
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_END 107
#define KEY_DOWN 108
#define KEY_PAGEDOWN 109
#define KEY_DELETE 111
#define KEY_LEFTMETA 125
#define KEY_RIGHTMETA 126
#define KEY_V 47
#define KEY_INSERT 110
#define KEY_CAPSLOCK 58
#define KEY_RIGHTALT 100
#define KEY_SYSRQ 99 /* Print Screen */

/* ---- display ---- */

static struct {
    int fd, w, h, pitch;
    uint8_t *mem;
    int rs, gs, bs; /* channel shifts */
} fb = {.fd = -1};

bool display_open(void) {
    fb.fd = open("/dev/fb0", O_RDWR);
    if (fb.fd < 0)
        return false;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fb.fd, FBIOGET_FSCREENINFO, &f) < 0)
        return false;
    if (v.bits_per_pixel != 32)
        return false;
    fb.w = (int)v.xres;
    fb.h = (int)v.yres;
    fb.pitch = (int)f.line_length;
    fb.rs = (int)v.red.offset, fb.gs = (int)v.green.offset, fb.bs = (int)v.blue.offset;
    fb.mem = mmap(NULL, (size_t)fb.pitch * (size_t)fb.h, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    return fb.mem != MAP_FAILED;
}

int display_width(void) { return fb.w; }
int display_height(void) { return fb.h; }

static bool put_mode(int w, int h, bool test) {
    struct fb_var_screeninfo v;
    if (fb.fd < 0 || ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) < 0)
        return false;
    v.xres = v.xres_virtual = (uint32_t)w;
    v.yres = v.yres_virtual = (uint32_t)h;
    v.bits_per_pixel = 32;
    v.activate = test ? FB_ACTIVATE_TEST : 0;
    return ioctl(fb.fd, FBIOPUT_VSCREENINFO, &v) == 0;
}

bool display_mode_possible(int w, int h) { return (w == fb.w && h == fb.h) || put_mode(w, h, true); }

/* A new resolution: the old mapping goes, the framebuffer is mapped again
 * at its new size. The caller reallocates whatever it draws into. */
bool display_set_mode(int w, int h) {
    if (w == fb.w && h == fb.h)
        return true;
    if (!put_mode(w, h, false))
        return false;
    munmap(fb.mem, (size_t)fb.pitch * (size_t)fb.h);
    fb.mem = MAP_FAILED;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fb.fd, FBIOGET_FSCREENINFO, &f) < 0)
        return false;
    fb.w = (int)v.xres;
    fb.h = (int)v.yres;
    fb.pitch = (int)f.line_length;
    fb.mem = mmap(NULL, (size_t)fb.pitch * (size_t)fb.h, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    return fb.mem != MAP_FAILED;
}

void display_present(const canvas *frame) {
    if (fb.mem == MAP_FAILED || !fb.mem)
        return;
    bool native = fb.rs == 16 && fb.gs == 8 && fb.bs == 0;
    for (int y = 0; y < fb.h && y < frame->h; y++) {
        const uint32_t *src = &frame->px[(size_t)y * (size_t)frame->w];
        uint32_t *dst = (uint32_t *)(fb.mem + (size_t)y * (size_t)fb.pitch);
        if (native) {
            memcpy(dst, src, (size_t)(fb.w < frame->w ? fb.w : frame->w) * 4);
        } else {
            for (int x = 0; x < fb.w && x < frame->w; x++) {
                uint32_t p = src[x];
                dst[x] = C_R(p) << fb.rs | C_G(p) << fb.gs | C_B(p) << fb.bs;
            }
        }
    }
}

/* ---- input ---- */

static struct {
    int kfd, mfd, sw, sh;
    bool down_prev;
} inp = {-1, -1, 0, 0, false};

bool input_open(int sw, int sh) {
    inp.kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    inp.mfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    inp.sw = sw;
    inp.sh = sh;
    return inp.kfd >= 0 && inp.mfd >= 0;
}

void input_set_screen(int sw, int sh) {
    inp.sw = sw;
    inp.sh = sh;
}

int input_fds(int fds[2]) {
    fds[0] = inp.kfd;
    fds[1] = inp.mfd;
    return 2;
}

static void add_key(input_state *in, int k) {
    if (in->nkeys < 32)
        in->keys[in->nkeys++] = k;
}

/* Key repeat is the shell's own, at the person's chosen pace (Settings >
 * Keyboard): the hardware's repeats are ignored, and a held key types
 * again after `delay` ms, then every `period` ms. */
static struct {
    int code;          /* the key held, or 0 */
    int keys[2], n;    /* what it typed */
    uint64_t next;     /* when it types again */
    int delay, period;
} rep = {0, {0, 0}, 0, 0, 500, 33};
static bool altgr, caps;

void input_set_repeat(int delay_ms, int rate_hz) {
    rep.delay = delay_ms < 150 ? 150 : delay_ms > 2000 ? 2000 : delay_ms;
    rep.period = 1000 / (rate_hz < 2 ? 2 : rate_hz > 60 ? 60 : rate_hz);
}

static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* What a key press types. Returns how many keys it added. */
static int press(input_state *in, int code) {
    int before = in->nkeys;
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: add_key(in, K_ENTER); break;
    case KEY_BACKSPACE: add_key(in, K_BACKSPACE); break;
    case KEY_ESC: add_key(in, K_ESC); break;
    case KEY_TAB: add_key(in, K_TAB); break;
    case KEY_UP: add_key(in, K_UP); break;
    case KEY_DOWN: add_key(in, K_DOWN); break;
    case KEY_LEFT: add_key(in, K_LEFT); break;
    case KEY_RIGHT: add_key(in, K_RIGHT); break;
    case KEY_DELETE: add_key(in, K_DELETE); break;
    case KEY_HOME: add_key(in, K_HOME); break;
    case KEY_END: add_key(in, K_END); break;
    case KEY_PAGEUP: add_key(in, K_PGUP); break;
    case KEY_PAGEDOWN: add_key(in, K_PGDN); break;
    case KEY_LEFTMETA: case KEY_RIGHTMETA: add_key(in, K_META); break;
    case KEY_INSERT: if (in->shift) add_key(in, K_PASTE); break;
    case KEY_SYSRQ: add_key(in, K_PRINT); break;
    default:
        if (code == KEY_SPACE && in->ctrl) {
            add_key(in, K_TALK);
        } else if (code == KEY_V && in->ctrl) {
            add_key(in, K_PASTE);
        } else {
            int out[2], n = keymap_translate(code, in->shift, altgr, caps, out);
            for (int i = 0; i < n; i++)
                add_key(in, out[i]);
        }
    }
    return in->nkeys - before;
}

/* Keys that type again while held: not the ones that do something once. */
static bool repeats(int code) {
    return code != KEY_ENTER && code != KEY_KPENTER && code != KEY_ESC && code != KEY_TAB && code != KEY_LEFTMETA &&
           code != KEY_RIGHTMETA && code != KEY_INSERT && code != KEY_CAPSLOCK && code != KEY_SYSRQ;
}

/* ---- the host clipboard (/dev/hostclip) ----
 * The VM launcher sends the host clipboard as frames: 0x02, text, 0x03. */
static int clip_fd = -2;
static char clip[4096], clip_next[4096];
static size_t clip_len;
static bool clip_in_frame;

static void clip_poll(void) {
    if (clip_fd == -2)
        clip_fd = open("/dev/hostclip", O_RDONLY | O_NONBLOCK);
    if (clip_fd < 0)
        return;
    char buf[512];
    ssize_t n;
    while ((n = read(clip_fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == 2) {
                clip_in_frame = true;
                clip_len = 0;
            } else if (c == 3 && clip_in_frame) {
                clip_in_frame = false;
                clip_next[clip_len] = 0;
                memcpy(clip, clip_next, clip_len + 1);
            } else if (clip_in_frame && clip_len < sizeof clip_next - 1) {
                clip_next[clip_len++] = c;
            }
        }
        if (n < (ssize_t)sizeof buf)
            break;
    }
}

const char *clipboard_text(void) { return clip; }
void clipboard_set(const char *text) {
    size_t n = text ? strlen(text) : 0;
    if (n > sizeof clip - 1) /* as much as fits, ending on a whole character */
        for (n = sizeof clip - 1; n && (text[n] & 0xc0) == 0x80; n--)
            ;
    memcpy(clip, text ? text : "", n);
    clip[n] = 0;
}

void input_poll(input_state *in) {
    clip_poll();
    in->clicked = in->pressed = in->right_clicked = in->moved = false;
    in->wheel = 0;
    in->nkeys = 0;
    in->nraw = 0;
    struct input_event ev[64];
    ssize_t n;
    while (inp.kfd >= 0 && (n = read(inp.kfd, ev, sizeof ev)) > 0) {
        for (int i = 0; i < (int)(n / (ssize_t)sizeof ev[0]); i++) {
            struct input_event *e = &ev[i];
            if (e->type != EV_KEY)
                continue;
            int code = e->code;
            if (in->nraw < 32 && code < 0x100 && e->value != 2) { /* every key (apps repeat by themselves) */
                in->raw[in->nraw].code = (uint16_t)code;
                in->raw[in->nraw++].down = e->value != 0;
            }
            bool down = e->value != 0;
            if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT)
                in->shift = down;
            else if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL)
                in->ctrl = down;
            else if (code == KEY_RIGHTALT)
                altgr = down;
            if (!down) {
                if (code == rep.code)
                    rep.code = 0;
                continue;
            }
            if (code == KEY_CAPSLOCK && e->value == 1)
                caps = !caps;
            if (code == rep.code)
                continue; /* the hardware repeating a key we repeat ourselves */
            int at = in->nkeys, n = press(in, code);
            if (n > 0 && repeats(code)) {
                rep.code = code;
                rep.n = n > 2 ? 2 : n;
                memcpy(rep.keys, &in->keys[at], sizeof(int) * (size_t)rep.n);
                rep.next = ms_now() + (uint64_t)rep.delay;
            } else if (n > 0) {
                rep.code = 0;
            }
        }
    }
    if (rep.code) {
        uint64_t t = ms_now();
        for (int guard = 0; t >= rep.next && guard < 3; guard++) { /* at most a few per frame */
            for (int i = 0; i < rep.n; i++)
                add_key(in, rep.keys[i]);
            rep.next += (uint64_t)rep.period;
        }
        if (t >= rep.next)
            rep.next = t + (uint64_t)rep.period;
    }
    while (inp.mfd >= 0 && (n = read(inp.mfd, ev, sizeof ev)) > 0) {
        for (int i = 0; i < (int)(n / (ssize_t)sizeof ev[0]); i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_ABS && e->code == ABS_X) {
                in->mx = (int)((int64_t)e->value * inp.sw / 65536);
                in->moved = true;
            } else if (e->type == EV_ABS && e->code == ABS_Y) {
                in->my = (int)((int64_t)e->value * inp.sh / 65536);
                in->moved = true;
            } else if (e->type == EV_REL && e->code == REL_X) {
                in->mx += e->value;
                in->moved = true;
            } else if (e->type == EV_REL && e->code == REL_Y) {
                in->my += e->value;
                in->moved = true;
            } else if (e->type == EV_REL && e->code == REL_WHEEL) {
                in->wheel += e->value;
            } else if (e->type == EV_KEY && e->code == BTN_LEFT) {
                if (e->value && !inp.down_prev)
                    in->pressed = true;
                if (!e->value && inp.down_prev)
                    in->clicked = true;
                in->down = e->value;
                inp.down_prev = e->value;
            } else if (e->type == EV_KEY && e->code == BTN_RIGHT && !e->value) {
                in->right_clicked = true;
            }
        }
    }
    if (in->mx < 0) in->mx = 0;
    if (in->my < 0) in->my = 0;
    if (in->mx >= inp.sw) in->mx = inp.sw - 1;
    if (in->my >= inp.sh) in->my = inp.sh - 1;
}

/* ---- jobs ---- */

struct job {
    pid_t pid;
    int fd;
    char *buf;
    size_t len, cap;
    bool done;
};

job *job_start(job_fn fn, void *arg) {
    int p[2];
    if (pipe(p) < 0)
        return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return NULL;
    }
    if (pid == 0) {
        close(p[0]);
        char *result = NULL;
        fn(arg, &result);
        if (result) {
            size_t n = strlen(result), off = 0;
            while (off < n) {
                ssize_t w = write(p[1], result + off, n - off);
                if (w <= 0)
                    break;
                off += (size_t)w;
            }
        }
        _exit(0);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    job *j = calloc(1, sizeof *j);
    j->pid = pid;
    j->fd = p[0];
    return j;
}

int job_fd(job *j) { return j ? j->fd : -1; }

bool job_poll(job *j, char **result) {
    if (!j || j->done)
        return j != NULL;
    char tmp[8192];
    for (;;) {
        ssize_t n = read(j->fd, tmp, sizeof tmp);
        if (n > 0) {
            if (j->len + (size_t)n + 1 > j->cap) {
                j->cap = (j->len + (size_t)n + 1) * 2;
                j->buf = realloc(j->buf, j->cap);
            }
            memcpy(j->buf + j->len, tmp, (size_t)n);
            j->len += (size_t)n;
            j->buf[j->len] = 0;
            continue;
        }
        if (n == 0) { /* writer closed: the child is done */
            waitpid(j->pid, NULL, 0);
            j->done = true;
            *result = j->buf;
            j->buf = NULL;
            return true;
        }
        return false; /* EAGAIN: still working */
    }
}

void job_free(job *j) {
    if (!j)
        return;
    if (!j->done) {
        kill(j->pid, SIGKILL);
        waitpid(j->pid, NULL, 0);
    }
    close(j->fd);
    free(j->buf);
    free(j);
}

void become_session_user(void) {
    /* their home is where their files are kept (and saved to disk): /root, for now */
    setenv("HOME", "/root", 1);
    setenv("USER", "user", 1);
    setenv("LOGNAME", "user", 1);
    if (getuid() != 0)
        return;
    if (setgroups(0, NULL) < 0 || setgid(SESSION_UID) < 0 || setuid(SESSION_UID) < 0)
        _exit(126);
}
