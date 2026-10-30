/* os-figure: the companion's figure, drawn in a process of its own.
 *
 *   os-figure SLOT_BYTES NSLOT
 *
 * fd 3 is shared memory the shell made: NSLOT frames of SLOT_BYTES each.
 * Commands come on stdin, one per line; frames are announced on stdout:
 *
 *   view W H SHOT            draw a W x H picture (SHOT: face, bust, waist, full)
 *   hide                     stop drawing (the figure isn't on screen)
 *   light KEY AMBIENT RIM    colours (rrggbb) from the design
 *   look-begin ... look-end  a new look (figure markup, one line at a time)
 *   pointer X Y | pointer off   where to look, -1..1 across the frame
 *   speak LEVEL F1 F2        her voice now (0..1 each), for her mouth
 *   speaking 0|1             a reply is being spoken
 *   mood NAME                calm happy sad excited sleepy curious proud worried
 *   act NAME                 a gesture (wave nod shake bow clap point ...)
 *   think 0|1, listen 0|1, doze 0|1
 *   clip-begin ... clip-end  a recorded clip (osclip text)
 *   ack SLOT                 the shell is done with that frame
 *   quit
 *
 *   ready                    built, and listening
 *   frame SLOT W H HX HY HR  a picture in SLOT (premultiplied ARGB, W x H), her
 *                            head's centre and radius in it
 *   err TEXT
 *
 * Once the figure is built and the drawing threads are started, it can't
 * open files, make connections or start programs (a seccomp filter): what
 * it is sent is parsed in here, so a bad look or clip harms nothing else. */
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* seccomp's filter language (musl has no kernel headers): classic BPF */
struct sock_filter {
    unsigned short code;
    unsigned char jt, jf;
    unsigned int k;
};
struct sock_fprog {
    unsigned short len;
    struct sock_filter *filter;
};
struct seccomp_data {
    int nr;
    unsigned int arch;
    unsigned long long instruction_pointer;
    unsigned long long args[6];
};
#define BPF_LD 0x00
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_W 0x00
#define BPF_ABS 0x20
#define BPF_JEQ 0x10
#define BPF_K 0x00
#define BPF_STMT(code, k) {(unsigned short)(code), 0, 0, (k)}
#define BPF_JUMP(code, k, jt, jf) {(unsigned short)(code), (jt), (jf), (k)}
#define AUDIT_ARCH_X86_64 0xC000003Eu
#define AUDIT_ARCH_AARCH64 0xC00000B7u
#ifdef __aarch64__
#define AUDIT_ARCH_HERE AUDIT_ARCH_AARCH64
#else
#define AUDIT_ARCH_HERE AUDIT_ARCH_X86_64
#endif
#ifndef SYS_poll /* arm64 has only ppoll */
#define SYS_poll 0xffffffffu
#endif
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_ERRNO 0x00050000u
#define SECCOMP_RET_ALLOW 0x7fff0000u
#ifndef SYS_seccomp
#define SYS_seccomp 317
#endif
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

#include "anim.h"
#include "figure.h"
#include "look.h"
#include "scene.h"

#define MAX_SLOTS 4

static struct {
    uint8_t *ring;
    size_t slot_bytes;
    int nslot;
    bool busy[MAX_SLOTS];
    figure fig;
    life L;
    scene sc;
    bool visible;
    fbuf buf;
    int bw, bh;
    char inbuf[65536];
    size_t inlen;
    /* a multi-line message being gathered (a look or a clip) */
    char *gather;
    size_t glen, gcap;
    int gathering; /* 0, 1 look, 2 clip */
    bool quit;
} G;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void say(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof b - 2)
        n = (int)sizeof b - 2;
    b[n++] = '\n';
    for (int off = 0; off < n;) {
        ssize_t w = write(1, b + off, (size_t)(n - off));
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            G.quit = true; /* the shell is gone */
            return;
        }
        off += (int)w;
    }
}

/* ---- the sandbox ---- */

static void sandbox(void) {
#define ALLOW(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_HERE, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        ALLOW(SYS_read), ALLOW(SYS_write), ALLOW(SYS_close), ALLOW(SYS_poll), ALLOW(SYS_ppoll), ALLOW(SYS_mmap),
        ALLOW(SYS_munmap), ALLOW(SYS_mremap), ALLOW(SYS_mprotect), ALLOW(SYS_madvise), ALLOW(SYS_brk), ALLOW(SYS_futex),
        ALLOW(SYS_clock_gettime), ALLOW(SYS_clock_nanosleep), ALLOW(SYS_nanosleep), ALLOW(SYS_rt_sigreturn),
        ALLOW(SYS_rt_sigprocmask), ALLOW(SYS_exit), ALLOW(SYS_exit_group), ALLOW(SYS_getpid), ALLOW(SYS_gettid),
        ALLOW(SYS_sched_yield), ALLOW(SYS_writev), ALLOW(SYS_tkill), ALLOW(SYS_set_robust_list),
        /* anything else fails: no files, sockets, programs, or new threads */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
    };
#undef ALLOW
    struct sock_fprog prog = {.len = (unsigned short)(sizeof f / sizeof *f), .filter = f};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0)
        say("err no sandbox (%d); going on without one", errno);
}

/* ---- drawing a frame ---- */

static int free_slot(void) {
    for (int i = 0; i < G.nslot; i++)
        if (!G.busy[i])
            return i;
    return -1;
}

static void draw(double now) {
    int s = free_slot();
    if (s < 0)
        return; /* the shell is behind: skip this one */
    int W = G.sc.w, H = G.sc.h, ss = G.sc.ss;
    if ((size_t)W * H * 4 > G.slot_bytes)
        return;
    if (G.bw != W * ss || G.bh != H * ss) {
        fig_buf_free(&G.buf);
        if (!fig_buf_init(&G.buf, W * ss, H * ss)) {
            G.bw = G.bh = 0;
            return;
        }
        G.bw = W * ss, G.bh = H * ss;
    } else {
        fig_buf_clear(&G.buf);
    }
    fpose pose;
    face_state fs;
    life_frame(&G.L, &G.fig, now, &pose, &fs);
    figure_face(&G.fig, &fs);
    if (G.fig.vrm)
        figure_vrm_face(&G.fig, &fs, &pose);
    /* gesturing: the camera eases back so her hands show, then in again */
    static double last;
    float dt = last > 0 ? (float)(now - last) : 0;
    last = now;
    G.sc.pull = easef(G.sc.pull, G.L.act >= 0 ? 1.0f : 0.0f, G.L.act >= 0 ? 4.0f : 1.5f, dt);
    fcam cam;
    scene_camera(&G.fig, &G.sc, &cam);
    uint32_t *out = (uint32_t *)(G.ring + (size_t)s * G.slot_bytes);
    scene_draw(&G.fig, &G.sc, &cam, &pose, &G.buf, out);
    float hx = 0, hy = 0, hr = 0;
    scene_head(&G.fig, &cam, &pose, &hx, &hy, &hr);
    G.busy[s] = true;
    say("frame %d %d %d %.1f %.1f %.1f", s, W, H, hx, hy, hr);
}

/* ---- commands ---- */

static void gather_add(const char *line) {
    size_t n = strlen(line);
    if (G.glen + n + 2 > G.gcap) {
        size_t c = G.gcap ? G.gcap * 2 : 65536;
        while (c < G.glen + n + 2)
            c *= 2;
        if (c > (size_t)16 << 20) { /* a clip of a minute at 30 fps fits easily */
            G.gathering = 0, G.glen = 0;
            say("err message too long");
            return;
        }
        char *g = realloc(G.gather, c);
        if (!g)
            return;
        G.gather = g, G.gcap = c;
    }
    memcpy(G.gather + G.glen, line, n);
    G.glen += n;
    G.gather[G.glen++] = '\n';
    G.gather[G.glen] = 0;
}

static void command(char *line) {
    if (G.gathering) {
        if ((G.gathering == 1 && !strcmp(line, "look-end")) || (G.gathering == 2 && !strcmp(line, "clip-end"))) {
            int kind = G.gathering;
            G.gathering = 0;
            if (!G.gather)
                return;
            if (kind == 1 && G.fig.vrm) {
                say("look ok (her own model stays)");
            } else if (kind == 1) {
                look lk;
                char why[160] = "";
                if (look_parse(G.gather, &lk, why, sizeof why)) {
                    life_free(&G.L);
                    figure_free(&G.fig);
                    if (!figure_build(&G.fig, &lk, 0)) {
                        say("err couldn't build that look");
                        look_default(&lk);
                        figure_build(&G.fig, &lk, 0);
                    }
                    life_init(&G.L, &G.fig, (unsigned)(now_s() * 1000));
                    say("look ok %s", lk.name);
                } else
                    say("err look: %s", why);
            } else if (!life_add_clip(&G.L, &G.fig, G.gather))
                say("err clip not understood");
            G.glen = 0;
            return;
        }
        gather_add(line);
        return;
    }
    char word[24] = "";
    sscanf(line, "%23s", word);
    const char *arg = line + strlen(word);
    while (*arg == ' ')
        arg++;
    if (!strcmp(word, "view")) {
        char shot[16] = "bust";
        int w = 0, h = 0;
        if (sscanf(arg, "%d %d %15s", &w, &h, shot) >= 2 && w >= 16 && h >= 16 && (size_t)w * h * 4 <= G.slot_bytes) {
            G.sc.w = w, G.sc.h = h;
            G.sc.shot = scene_shot_from_name(shot);
            G.visible = true;
        } else
            say("err view %s", arg);
    } else if (!strcmp(word, "hide"))
        G.visible = false;
    else if (!strcmp(word, "light")) {
        unsigned k = 0, a = 0, r = 0;
        if (sscanf(arg, "%x %x %x", &k, &a, &r) == 3)
            G.sc.key = k, G.sc.ambient = a, G.sc.rim = r;
    } else if (!strcmp(word, "pointer")) {
        float x, y;
        if (sscanf(arg, "%f %f", &x, &y) == 2)
            G.L.look_x = x, G.L.look_y = y, G.L.look_on = true;
        else
            G.L.look_on = false;
    } else if (!strcmp(word, "speak")) {
        float l = 0, f1 = 0, f2 = 0;
        sscanf(arg, "%f %f %f", &l, &f1, &f2);
        G.L.speak = clampf(l, 0, 1), G.L.f1 = f1, G.L.f2 = f2;
    } else if (!strcmp(word, "speaking"))
        G.L.speaking = atoi(arg) != 0;
    else if (!strcmp(word, "mood")) {
        int m = life_mood_from_name(arg);
        if (m >= 0)
            G.L.mood = m;
    } else if (!strcmp(word, "act")) {
        if (!life_act(&G.L, arg))
            say("err no gesture %s", arg);
    } else if (!strcmp(word, "think"))
        G.L.thinking = atoi(arg) != 0;
    else if (!strcmp(word, "listen"))
        G.L.listening = atoi(arg) != 0;
    else if (!strcmp(word, "doze"))
        G.L.dozing = atoi(arg) != 0;
    else if (!strcmp(word, "ack")) {
        int s = atoi(arg);
        if (s >= 0 && s < G.nslot)
            G.busy[s] = false;
    } else if (!strcmp(word, "look-begin"))
        G.gathering = 1, G.glen = 0;
    else if (!strcmp(word, "clip-begin"))
        G.gathering = 2, G.glen = 0;
    else if (!strcmp(word, "quit"))
        G.quit = true;
    else if (word[0])
        say("err what is %s", word);
}

static void read_commands(void) {
    for (;;) {
        if (G.inlen >= sizeof G.inbuf - 1)
            G.inlen = 0; /* a line longer than this is nonsense */
        ssize_t n = read(0, G.inbuf + G.inlen, sizeof G.inbuf - 1 - G.inlen);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            G.quit = n == 0; /* the shell closed its end */
            return;
        }
        G.inlen += (size_t)n;
        char *start = G.inbuf, *nl;
        while ((nl = memchr(start, '\n', G.inlen - (size_t)(start - G.inbuf)))) {
            *nl = 0;
            if (nl > start && nl[-1] == '\r')
                nl[-1] = 0;
            command(start);
            start = nl + 1;
        }
        G.inlen -= (size_t)(start - G.inbuf);
        memmove(G.inbuf, start, G.inlen);
        return;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: os-figure SLOT_BYTES NSLOT (frames on fd 3)\n");
        return 2;
    }
    G.slot_bytes = strtoul(argv[1], NULL, 10);
    G.nslot = atoi(argv[2]);
    if (G.nslot < 2 || G.nslot > MAX_SLOTS || G.slot_bytes < 64 * 64 * 4)
        return 2;
    G.ring = mmap(NULL, G.slot_bytes * G.nslot, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0);
    if (G.ring == MAP_FAILED)
        return 3;
    close(3);
    /* fd 4, if open: a VRM model someone brought as her body (read before
     * the sandbox closes the door) */
    bool have_vrm = false;
    {
        size_t cap = 0, n = 0;
        unsigned char *d = NULL;
        ssize_t r;
        for (;;) {
            if (n == cap) {
                if (cap >= (size_t)256 << 20) /* no VRM is this big */
                    break;
                unsigned char *d2 = realloc(d, cap ? cap * 2 : 1 << 20);
                if (!d2)
                    break;
                d = d2, cap = cap ? cap * 2 : 1 << 20;
            }
            r = read(4, d + n, cap - n);
            if (r <= 0)
                break;
            n += (size_t)r;
        }
        close(4);
        if (n > 0) {
            char why[200];
            if (figure_load_vrm(&G.fig, d, n, why, sizeof why)) {
                fpose rest;
                fig_pose_rest(&G.fig.m, &rest);
                body_pose_relaxed(&G.fig.body, &rest);
                fig_pose_update(&G.fig.m, &rest);
                fig_bake_sun(&G.fig.m, &rest, vnorm(V3(-0.5f, 0.65f, 0.75f)), 8);
                have_vrm = true;
                say("vrm %s by %s; licence: %s", G.fig.vrm_title, G.fig.vrm_author, G.fig.vrm_licence);
            } else {
                figure_free(&G.fig);
                say("err the VRM model can't be used: %s", why);
            }
        }
        free(d);
    }
    look lk;
    look_default(&lk);
    if (!have_vrm && !figure_build(&G.fig, &lk, 0)) {
        say("err couldn't build the figure");
        return 4;
    }
    life_init(&G.L, &G.fig, (unsigned)(now_s() * 1000));
    scene_defaults(&G.sc);
    fig_threads = 3;
    fig_pool_start();
    sandbox();
    say("ready");
    double next = now_s();
    while (!G.quit) {
        double now = now_s();
        int wait = -1;
        if (G.visible) {
            wait = next > now ? (int)((next - now) * 1000) : 0;
            if (wait > 1000)
                wait = 1000;
        }
        struct pollfd p = {.fd = 0, .events = POLLIN};
        int r = poll(&p, 1, wait);
        if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)))
            read_commands();
        now = now_s();
        if (G.visible && now >= next) {
            draw(now);
            double fps = G.L.dozing ? 5 : life_busy(&G.L) ? 30 : 15;
            next += 1.0 / fps;
            if (next < now)
                next = now + 1.0 / fps;
        }
    }
    return 0;
}
