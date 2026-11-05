/* os-shell: the whole visible the OS experience. First boot (choose a
 * mind, connect, get to know each other), the AI designing the desktop, and
 * the desktop itself, drawn straight to /dev/fb0 from the AI's design. */
#include "presence.h"
#include "app.h"
#include "lock.h"
#include "clock.h"
#include "priv.h"
#include "nerve.h"
#include "emblem.h"
#include "fm/fm.h"
#include "wallpaper.h"
#include "wl/wl.h"
#include "store.h"
#include "mind.h"
#include "wm.h"
#include "settings.h"
#include "portal.h"
#include "appapi.h"
#include "capture.h"
#include "models.h"
#include "caption.h"
#include "agent.h"
#include "voice.h"
#include "launcher.h"
#include "preview.h"
#include "body.h"
#include "alive.h"
#include "term.h"
#include "fxwin.h"

#include <dirent.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

app A;

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void set_screen(enum screen s) {
    static const char *const names[] = {"splash", "choose", "models", "connect", "onboard", "designing", "desktop"};
    LOG("screen %s\n", names[s]);
    A.screen = s;
    A.screen_since = A.now;
    A.dirty = true;
}

/* While the companion's tags are carried out, what the screen tells them is kept for it too (desktop.c): it saw
 * none of it, and tried again and again when "there's no app" was shown only to the person. */
char toast_heard[1200];
bool toast_listening;

void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof A.toast, fmt, ap);
    va_end(ap);
    if (toast_listening) {
        size_t l = strlen(toast_heard);
        snprintf(toast_heard + l, sizeof toast_heard - l, "- the screen said: \"%s\"\n", A.toast);
    }
    A.toast_until = A.now + 2500 + 45 * strlen(A.toast); /* long enough to read */
}

/* The start: nothing of its own to show (the boot screen, os-boot, is still up and the desktop fades in from it,
 * sys.c), so straight on to where they are. */
static void splash_draw(void) {
    if (A.have_design)
        desktop_enter();
    else if (A.cfg.provider[0] && A.cfg.model[0]) {
        if (!onboard_resume()) /* (the desktop program restarted mid-way: back where it was) */
            onboard_start();
    }
    else if (A.provider)
        set_screen(SCR_CONNECT);
    else
        set_screen(SCR_CHOOSE);
}

/* Their account is named after them: once the name they gave is known, it is kept (/etc/os/account: the
 * account's name, then their full name) for init, which names the account and its home after it at the next
 * start. "Valerie Liu" -> valerie. */
static void keep_account_name(void) {
    static uint64_t at;
    if (A.now - at < 5000)
        return;
    at = A.now;
    const char *full = A.d.user_name[0] ? A.d.user_name : A.prof.user_name;
    char name[32] = "";
    size_t k = 0;
    for (const char *p = full; *p && *p != ' ' && k + 1 < sizeof name; p++) {
        char c = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
        if ((c >= 'a' && c <= 'z') || (k && ((c >= '0' && c <= '9') || c == '-' || c == '_')))
            name[k++] = c;
    }
    name[k] = 0;
    if (!account_name_ok(name))
        return;
    char want[160], have[160] = "";
    snprintf(want, sizeof want, "%s\n%s\n", name, full);
    FILE *f = fopen(STATE_DIR "/account", "r");
    if (f) {
        size_t n = fread(have, 1, sizeof have - 1, f);
        have[n] = 0;
        fclose(f);
    }
    if (!strcmp(want, have))
        return;
    f = fopen(STATE_DIR "/account", "w");
    if (f) {
        fputs(want, f);
        fclose(f);
        LOG("account: %s (%s), from the next start\n", name, full);
    }
}

/* A cheap fingerprint of what is kept: names, sizes and times of the files. */
static uint64_t tree_sig(const char *path, uint64_t h) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return h;
    for (const char *p = path; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    h = (h ^ (uint64_t)st.st_size) * 1099511628211ull;
    h = (h ^ (uint64_t)st.st_mtime) * 1099511628211ull;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                char c[1024];
                snprintf(c, sizeof c, "%s/%s", path, e->d_name);
                h = tree_sig(c, h);
            }
        if (d)
            closedir(d);
    }
    return h;
}

/* The person's home is theirs: anything in it not owned by them (made by the system, which runs as root, until the
 * desktop itself runs as them) is given back, a few seconds after it appears. Links are changed, not followed. */
static void own_home(const char *path, int depth) {
    struct stat st;
    if (depth > 64 || lstat(path, &st) < 0)
        return;
    if (st.st_uid != SESSION_UID || st.st_gid != SESSION_UID)
        if (lchown(path, SESSION_UID, SESSION_UID) < 0)
            return;
    if (!S_ISDIR(st.st_mode))
        return;
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            char c[1024];
            if (snprintf(c, sizeof c, "%s/%s", path, e->d_name) < (int)sizeof c)
                own_home(c, depth + 1);
        }
    if (d)
        closedir(d);
}

static void keep_home_theirs(void) {
    static uint64_t at;
    if (getuid() != 0) /* the desktop runs as them: what it makes is theirs already */
        return;
    if (A.now - at < 4000)
        return;
    at = A.now;
    own_home(home_dir(), 0);
}

static void save_child(void *arg, char **result) {
    (void)arg;
    *result = strdup(store_save() ? "saved" : "failed");
}

/* Saves to the disk a few seconds after something changes, in a child
 * process so the screen never waits for the disk. */
static void keep_on_disk(void) {
    static bool usable = true, primed;
    static uint64_t last_check, saved_sig, changed_at;
    static job *saving;
    static int saving_priv; /* asked of init (the desktop runs as the person) */
    if (!usable)
        return;
    char *res;
    bool sok;
    if (saving_priv > 0 && priv_take(saving_priv, &sok, NULL)) {
        LOG("store: %s\n", sok ? "saved" : "failed");
        saving_priv = 0;
    }
    if (saving && job_poll(saving, &res)) {
        LOG("store: %s\n", res ? res : "?");
        free(res);
        job_free(saving);
        saving = NULL;
    }
    if (A.now - last_check < 2000)
        return;
    last_check = A.now;
    if (priv_available() ? !priv_store_kept() : !store_available()) {
        usable = false;
        return;
    }
    /* (the home on the disk itself: its own file system keeps it; only the state is the store's) */
    uint64_t sig = access("/run/os/home-on-disk", F_OK) == 0 ? tree_sig(STATE_DIR, 1469598103934665603ull)
                                                             : tree_sig(home_dir(), tree_sig(STATE_DIR, 1469598103934665603ull));
    if (!primed) {
        primed = true;
        saved_sig = sig;
        return;
    }
    if (sig != saved_sig && !changed_at)
        changed_at = A.now;
    if (changed_at && A.now - changed_at > 3000 && !saving && saving_priv <= 0) {
        if (priv_available())
            saving_priv = priv_send("save");
        else
            saving = job_start(save_child, NULL);
        saved_sig = sig;
        changed_at = 0;
    }
}

/* One frame: input, drawing, presenting, then sleeping until something happens. */
static void (*modal)(void); /* a question being asked (main_modal) */
static int frame_fds[2];
static bool woke_by_app; /* the last sleep ended for an app or a service (the portal, the companion's face, the API) */
static bool last_animating;

/* Where the desktop's time goes, part by part (PROF around each step of a frame), in the log every half minute. */
static struct {
    const char *name;
    uint64_t us, calls;
} prof[48];

uint64_t prof_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

void prof_add(const char *name, uint64_t us) {
    for (int i = 0; i < 48; i++)
        if (prof[i].name == name || !prof[i].name) {
            prof[i].name = name;
            prof[i].us += us;
            prof[i].calls++;
            return;
        }
}

static void prof_log(uint64_t span_ms) {
    char line[800] = "";
    for (int k = 0; k < 14; k++) { /* the costliest, most first */
        int best = -1;
        for (int i = 0; i < 48; i++)
            if (prof[i].name && prof[i].calls && (best < 0 || prof[i].us > prof[best].us))
                best = i;
        if (best < 0 || prof[best].us < 1000)
            break;
        snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s %.1f%%", line[0] ? ", " : "", prof[best].name,
                 span_ms ? prof[best].us / 10.0 / (double)span_ms : 0);
        prof[best].calls = 0;
    }
    LOG("time: %s (of one core)\n", line[0] ? line : "nothing to speak of");
    extern double fm_pacc[8]; /* the renderer's own: per stage of painting a box, every document (ms) */
    if (span_ms)
        LOG("time: painting boxes: shadows %.1f%%, glows %.1f%%, glass %.1f%%, saturation %.1f%%, backgrounds %.1f%%, "
            "window frame layout %.1f%%\n", fm_pacc[0] * 100 / span_ms, fm_pacc[1] * 100 / span_ms,
            fm_pacc[2] * 100 / span_ms, fm_pacc[3] * 100 / span_ms, fm_pacc[4] * 100 / span_ms,
            fm_pacc[5] * 100 / span_ms);
    for (int i = 0; i < 8; i++)
        fm_pacc[i] = 0;
    for (int i = 0; i < 48; i++)
        prof[i].us = prof[i].calls = 0;
}

/* What the apps' windows show, in one number: a new frame from any of them changes it. */
static uint64_t views_sig(void) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < wls_view_count(); i++) {
        wl_view *v = wls_view(i);
        uint64_t parts[4] = {(uint64_t)v->id, v->version, (uint64_t)v->mapped, (uint64_t)v->w << 32 | (uint32_t)v->h};
        for (int k = 0; k < 4; k++)
            h = (h ^ parts[k]) * 1099511628211ull;
    }
    return h;
}

/* Drawing is most of what the desktop costs (the whole screen, in software, then copied out): a frame is drawn
 * when something on it may have changed, and a little after (hovers fading, a popover closing), and otherwise four
 * times a second (a clock, a blinking cursor, anything that changed without saying so). It used to be thirty
 * times a second whatever happened: half a processor core with nothing moving. */
static bool just_apps; /* this frame was wanted for nothing but an app's new picture */

static bool frame_wanted(void) {
    static uint64_t last_drawn, last_full, busy_until, last_views, since, frames, drawn, quick;
    static unsigned seen; /* which reasons kept it drawing, over the last half minute (for the log) */
    uint64_t vs = views_sig();
    bool input = A.in.moved || A.in.down || A.in.clicked || A.in.pressed || A.in.right_clicked || A.in.wheel ||
                 A.in.nkeys || A.in.nraw;
    bool why[] = {
        A.dirty || alive_prefs.always_draw, A.screen != SCR_DESKTOP || modal || lock_active(), input, vs != last_views, woke_by_app,
        false /* (an app waiting for its turn: given it without a frame, below) */, last_animating,
        wallpaper_animated(&A.d), wm_poll_ms(33) < 33,
        (A.toast[0] && A.now < A.toast_until + 500) || caption_showing(),
        A.companion.pending || agent_busy() || voice_speaking() || voice_listening() || voice_hearing(),
        capture_active() || launcher_active() || preview_showing() || portal_modal() || appapi_modal() || body_asking() ||
            A.confirm_power || actions_confirming() || mind_fallback_asking() || ctx_menu_showing(),
        term_busy(), fxwin_wake_in() <= 34, A.face_new,
    };
    static const char *const names[] = {"asked", "screen", "input", "app frame", "app message", "app waiting",
                                        "animation", "live wallpaper", "window manager", "toast or caption",
                                        "companion", "panel", "terminal", "Facet app", "its face"};
    enum { FACE = 14 };
    bool busy = false, other = false;
    for (unsigned i = 0; i < sizeof why / sizeof why[0]; i++)
        if (why[i]) {
            busy = true;
            other |= i != 3 && i != 4 && i != FACE; /* (pictures and messages: no hover fading after those) */
            seen |= 1u << i;
        }
    last_views = vs;
    woke_by_app = false;
    if (other)
        busy_until = A.now + 600;
    bool draw = busy || A.now < busy_until || A.now - last_drawn >= 250;
    /* only an app's picture changed: maybe only it is put on the screen (still a whole frame every 250 ms, for what
     * changed without saying so) */
    just_apps = draw && why[3] && !other && !why[FACE] && A.now >= busy_until && A.now - last_full < 250;
    if (draw) {
        last_drawn = A.now;
        A.dirty = false;
        A.face_new = false;
        drawn++;
        if (!just_apps)
            last_full = A.now;
    }
    frames++;
    if (!since)
        since = A.now;
    if (A.now - since >= 30000 && A.screen == SCR_DESKTOP) { /* how much it drew, and why */
        char list[256] = "";
        for (unsigned i = 0; i < sizeof why / sizeof why[0]; i++)
            if (seen & (1u << i))
                snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? ", " : "", names[i]);
        LOG("frames: %llu drawn of %llu in %llu s (kept drawing by: %s; %llu wanted only for an app's picture)\n",
            (unsigned long long)drawn, (unsigned long long)frames, (unsigned long long)((A.now - since) / 1000),
            list[0] ? list : "nothing", (unsigned long long)quick);
        prof_log(A.now - since);
        since = A.now, frames = drawn = quick = 0, seen = 0;
    }
    quick += just_apps;
    return draw;
}

/* What the pointer covers, kept from before it was drawn: a frame that puts only an app's picture on the screen
 * puts back what it covered there (outside the picture) and draws it again on top. */
#define UNDER 48
static uint32_t under[(2 * UNDER) * (2 * UNDER)];
static rect under_r;

static void pointer_save(int x, int y) {
    int x0 = x - UNDER < 0 ? 0 : x - UNDER, y0 = y - UNDER < 0 ? 0 : y - UNDER;
    int x1 = x + UNDER > A.frame->w ? A.frame->w : x + UNDER, y1 = y + UNDER > A.frame->h ? A.frame->h : y + UNDER;
    under_r = (rect){x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    for (int j = 0; j < under_r.h; j++)
        memcpy(under + (size_t)j * (size_t)under_r.w, A.frame->px + (size_t)(y0 + j) * (size_t)A.frame->w + x0,
               (size_t)under_r.w * 4);
}

static void pointer_put_back(rect app) {
    for (int j = 0; j < under_r.h; j++)
        for (int i = 0; i < under_r.w; i++) {
            int x = under_r.x + i, y = under_r.y + j;
            uint32_t *f = A.frame->px + (size_t)y * (size_t)A.frame->w + x, *u = under + (size_t)j * (size_t)under_r.w + i;
            if (x >= app.x && x < app.x + app.w && y >= app.y && y < app.y + app.h)
                *u = *f; /* the app's new picture is what lies under it now */
            else
                *f = *u;
        }
}

static rect rect_union(rect a, rect b) {
    if (a.w <= 0 || a.h <= 0)
        return b;
    if (b.w <= 0 || b.h <= 0)
        return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (rect){x0, y0, x1 - x0, y1 - y0};
}

/* When apps were last told to draw their next frame (wls_frame_done): no more often than a screen shows them (60 a
 * second). Told after every frame drawn, Prism drew as fast as the desktop could follow: 83 a second, all of a
 * core, on a screen that shows 60. */
static uint64_t frames_done_at;

/* A frame at most every FRAME_MS (60 a second, what a screen shows). Drawn back to back, the desktop took a whole
 * core with Firefox or Prism open (80 and more frames a second, all the screen each time, in software), and apps
 * waited for it: their messages were read only between frames, and an app opening sends hundreds it waits on the
 * answers to, a frame's time each (12 ms at full size: full-screen apps opened slowly, small ones at once). Now
 * between frames each message is answered as it comes, and input waits in the kernel for the next frame. */
#define FRAME_MS 16
static uint64_t paced_until;

void main_frame(void) {
    uint64_t frame_t0 = prof_now_us();
    A.now = now_ms();
    if (A.now < paced_until) { /* between two frames: only the apps answered */
        PROF("wayland", wls_dispatch());
        PROF("portal", portal_poll());
        PROF("app API", appapi_poll());
        PROF("presence", presence_poll());
        A.drawing = false;
        goto drawn;
    }
    PROF("wm begin", wm_frame_begin());
    PROF("wayland", wls_dispatch());
    PROF("portal", portal_poll()); /* os-portal: file choosers, notifications */
    PROF("app API", appapi_poll()); /* apps using the companion: its AI, voice, face, memory */
    PROF("models", models_poll()); /* model downloads, wherever they were started */
    PROF("built-in model", mind_builtin_tick()); /* the model that comes with it, once it has been read from the disc */
    PROF("presence", presence_poll()); /* its frames, and what it should know */
    PROF("input", input_poll(&A.in));
    if (A.in.clicked || A.in.pressed || A.in.nkeys || A.in.wheel)
        nerve_person_touched(); /* what follows from this is theirs (nerve.c) */
    PROF("text menu", ui_text_menu_input()); /* a text field's right-click menu, when it is up, has the pointer */
    ctx_menu_input();                         /* and anything else's */
    PROF("deciding", A.drawing = frame_wanted());
    /* only the top app's picture changed, and its window is nothing but that picture: the desktop isn't drawn
     * (what it does besides drawing still runs), the picture alone is put on (desktop.c) */
    bool quick = A.drawing && just_apps && !modal && desktop_app_frame_ready();
    rect quick_r = {0, 0, 0, 0};
    if (quick)
        A.drawing = false;
    input_state real = A.in;
    if (modal) /* a question is up: the screen goes on underneath, its input is the question's */
        input_clear(&A.in);
    PROF("clock", clock_frame()); /* the automatic time zone, the network's time (clock.c) */
    PROF("onboarding", onboard_keep()); /* onboarding's place, kept as it changes (onboard_talk.inc) */
    if (lock_frame()) /* locked: the lock screen is everything (lock.c) */
        goto drawn;
    switch (A.screen) {
    case SCR_SPLASH: splash_draw(); break;
    case SCR_CHOOSE: choose_draw(); break;
    case SCR_MODELS: models_draw(); break;
    case SCR_CONNECT: connect_draw(); break;
    case SCR_ONBOARD: onboard_draw(); break;
    case SCR_DESIGNING: designing_draw(); break;
    case SCR_DESKTOP: PROF("desktop", desktop_draw()); break;
    }
    if (modal) {
        A.in = real;
        modal();
    }
    if (quick) {
        if (desktop_app_frame_put(&quick_r))
            A.drawing = true;
        else
            quick = false, A.dirty = true; /* (it changed meanwhile: the whole frame, next) */
    }
drawn:
    if (A.drawing) {
        paced_until = A.now + FRAME_MS;
        if (!quick) {
            ui_text_menu_draw();
            ctx_menu_draw();
            desktop_note_clean(); /* (before the pointer: it isn't part of the window) */
            capture_frame(false); /* a recording without the pointer takes this */
            pointer_save(A.in.mx, A.in.my);
        } else {
            PROF("app picture", pointer_put_back(quick_r));
        }
        ui_cursor(A.frame, A.in.mx, A.in.my);
        wm_cursor(A.frame, A.in.mx, A.in.my); /* resize arrows on a window's border */
        if (!quick)
            capture_frame(true); /* ...with it, this */
        if (quick)
            PROF("present", display_present_rect(A.frame, rect_union(quick_r, under_r)));
        else if (A.screen != SCR_SPLASH) /* (until there is something to show, the boot screen stays) */
            PROF("present", display_present(A.frame));
        last_animating = fm_animating; /* (set while it was styled: @keyframes, transitions) */
        PROF("wm end", wm_frame_end());
        if (A.now - frames_done_at >= 16) { /* apps draw their next frame now: at most 60 times a second */
            PROF("frame done", wls_frame_done((uint32_t)A.now));
            frames_done_at = A.now;
        }
    } else if (wls_frames_waiting() && A.now - frames_done_at >= 16) {
        /* (their last frame is on the screen already: their turn, with nothing to draw) */
        PROF("frame done", wls_frame_done((uint32_t)A.now));
        frames_done_at = A.now;
    }
    PROF("save", keep_on_disk());
    PROF("home owner", keep_home_theirs());
    PROF("account", keep_account_name());

    /* Sleep until input arrives, an app has something to say, or the
     * next animation frame (~30 fps). */
    uint64_t t_poll = now_ms();
    bool pacing = t_poll < paced_until; /* (input waits for the next frame: not woken for it before) */
    struct pollfd p[160] = {{.fd = pacing ? -1 : frame_fds[0], .events = POLLIN},
                            {.fd = pacing ? -1 : frame_fds[1], .events = POLLIN}};
    int wfds[158], nw = wls_fds(wfds, 30);
    nw += portal_fds(wfds + nw, 158 - nw);
    nw += presence_fds(wfds + nw, 158 - nw);
    nw += appapi_fds(wfds + nw, 158 - nw);
    for (int i = 0; i < nw; i++)
        p[2 + i] = (struct pollfd){.fd = wfds[i], .events = POLLIN};
    prof_add("whole frame", prof_now_us() - frame_t0);
    uint64_t slept = prof_now_us();
    int wait_ms = wm_poll_ms(33);
    if (pacing && (int)(paced_until - t_poll) < wait_ms)
        wait_ms = (int)(paced_until - t_poll);
    if (wls_frames_waiting()) { /* an app's next turn, when its 16 ms are up */
        int left = 16 - (int)(now_ms() - frames_done_at);
        left = left < 1 ? 1 : left;
        if (left < wait_ms)
            wait_ms = left;
    }
    int woke = poll(p, (nfds_t)(2 + nw), wait_ms);
    prof_add("asleep", prof_now_us() - slept);
    if (woke > 0)
        for (int i = 2; i < 2 + nw; i++)
            if (p[i].revents)
                woke_by_app = true;
}

/* Runs frames, with `draw` over everything taking the input, until *done. Code
 * waiting for an answer (a Facet app's ask) calls this and carries on after. */
void main_modal(void (*draw)(void), const bool *done) {
    void (*outer)(void) = modal;
    modal = draw;
    while (!*done)
        main_frame();
    modal = outer;
}

/* Its log, as it goes to the console, also kept in /tmp/os-shell.log, where the companion can read it (the last
 * 4 MB: the file starts over past that). */
static void log_to_file_too(void) {
    int p[2];
    if (pipe(p) != 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[1]);
        int console = dup(2);
        int f = open("/tmp/os-shell.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char buf[8192];
        off_t size = 0;
        for (;;) {
            ssize_t n = read(p[0], buf, sizeof buf);
            if (n <= 0)
                _exit(0);
            if (console >= 0 && write(console, buf, (size_t)n) < 0) {
            }
            if (f >= 0) {
                if (size > (4 << 20)) {
                    if (ftruncate(f, 0) == 0)
                        lseek(f, 0, SEEK_SET);
                    size = 0;
                }
                if (write(f, buf, (size_t)n) > 0)
                    size += n;
            }
        }
    }
    close(p[0]);
    if (pid > 0)
        dup2(p[1], 2);
    close(p[1]);
}

int main(void) {
    log_to_file_too();
    LOG("gfx: drawing with the %s loops\n", gfx_init());
    if (!display_open()) {
        fprintf(stderr, "os-shell: no framebuffer\n");
        return 1;
    }
    A.W = display_width();
    A.H = display_height();
    A.frame = canvas_new(A.W, A.H);
    input_open(A.W, A.H);
    A.in.mx = A.W / 2;
    A.in.my = A.H / 2;
    fonts_init("/usr/share/fonts");
    LOG("shell: running as %s\n", getuid() == 0 ? "root (an older init)" : "the person");
    mkdir(STATE_DIR, 0700);
    /* the system's state (keys, the companion's memory, app grants): root's alone; the home: the person's */
    chmod(STATE_DIR, 0700);
    { /* this boot's key for the local model servers (ai_local_key) */
        unsigned char r[24];
        int fd = open("/dev/urandom", O_RDONLY);
        bool got = fd >= 0 && read(fd, r, sizeof r) == (ssize_t)sizeof r;
        if (fd >= 0)
            close(fd);
        FILE *kf = got ? fopen(AI_LOCAL_KEY_FILE, "w") : NULL;
        if (kf) {
            fchmod(fileno(kf), 0600);
            for (size_t i = 0; i < sizeof r; i++)
                fprintf(kf, "%02x", r[i]);
            fputc('\n', kf);
            fclose(kf);
        } else {
            LOG("shell: no key for the local model servers\n");
        }
    }
    if (getuid() == 0 && (chown(home_dir(), SESSION_UID, SESSION_UID) < 0 || chmod(home_dir(), 0700) < 0))
        LOG("shell: the home couldn't be given to the person\n"); /* (as the person, init has given it) */
    /* A home with somewhere to put things. */
    const char *const home[] = {home_path("/Documents"), home_path("/Music"), home_path("/Pictures"), home_path("/Projects"), NULL};
    for (int i = 0; home[i]; i++)
        mkdir(home[i], 0755);
    /* Linux desktop apps find the compositor through these (and so do the
     * ones started from the terminal, which inherits them). */
    mkdir("/run", 0755);
    mkdir("/run/user", 0755);
    wls_set_screen(A.W, A.H);
    mkdir(SESSION_RUNTIME, 0700);
    if (chown(SESSION_RUNTIME, SESSION_UID, SESSION_UID) < 0) { /* only ownership */
    }
    if (wls_start(SESSION_RUNTIME)) {
        setenv("XDG_RUNTIME_DIR", SESSION_RUNTIME, 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1); /* how toolkits (Electron's "auto") pick Wayland */
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("SDL_VIDEODRIVER", "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);
    }
    A.drag_win = -1;
    A.now = now_ms();

    /* Restore an existing mind and design (persisted under /etc/os). */
    /* Whatever was kept on the disk comes back first: the mind, the design,
     * the portrait, their files. */
    /* as the person, init has put it back already (the raw disk is root's); as root (an older init), here */
    int kept = priv_available() ? priv_store_restored() : store_restore();
    if (kept > 0)
        LOG("store: restored %d files from the disk\n", kept);
    else if (kept == 0)
        LOG("store: a new disk\n");
    else
        LOG("store: no disk; nothing will be kept\n");
    if (ai_config_load(&A.cfg) == 0)
        A.provider = ai_find_provider(A.cfg.provider);
    if (A.provider) {
        /* A config from boot parameters may leave the address and model out. */
        if (!A.cfg.base_url[0] && A.provider->base_url)
            snprintf(A.cfg.base_url, sizeof A.cfg.base_url, "%s", A.provider->base_url);
        if (!A.cfg.model[0] && A.provider->default_model)
            snprintf(A.cfg.model, sizeof A.cfg.model, "%s", A.provider->default_model);
        /* No model known: open the connect screen ready to pick one. */
        snprintf(A.url.buf, sizeof A.url.buf, "%s", A.cfg.base_url);
        A.url.len = (int)strlen(A.url.buf);
        snprintf(A.key.buf, sizeof A.key.buf, "%s", A.cfg.api_key);
        A.key.len = (int)strlen(A.key.buf);
        A.key.secret = true;
        A.focus = 1;
        LOG("mind %s (%s)\n", A.provider->id, A.cfg.model[0] ? A.cfg.model : "model not chosen");
        if (!strcmp(A.provider->id, "builtin"))
            mind_builtin_start(); /* their own mind, waking with the computer */
    }
    if (A.provider)
        portrait_load(&A.prof, STATE_DIR "/portrait.txt");
    if (A.provider && design_load(&A.d, STATE_DIR "/design.json", &A.prof))
        A.have_design = true;
    /* The desktop document itself (Facet markup), if there is one. */
    FILE *ff = A.provider ? fopen(STATE_DIR "/facet.xml", "r") : NULL;
    if (ff) {
        static char doc[262144];
        size_t n = fread(doc, 1, sizeof doc - 1, ff);
        doc[n] = 0;
        fclose(ff);
        fm_doc *ui = fm_parse(doc);
        design fd;
        if (design_from_facet(&fd, ui, &A.prof, NULL, 0)) {
            if (!A.have_design)
                A.d = fd;
            A.ui = ui;
            A.have_design = true;
        } else {
            fm_free(ui);
        }
    }
    /* A desktop they already had: pick up the conversation where it was. */
    if (A.have_design) {
        convo_load(&A.companion, STATE_DIR "/companion.txt");
        A.returning = true;
    }
    FILE *nf = fopen(home_path("/notes.txt"), "r");
    if (nf) {
        A.notes.len = (int)fread(A.notes.buf, 1, sizeof A.notes.buf - 1, nf);
        A.notes.buf[A.notes.len] = 0;
        fclose(nf);
    }
    settings_boot(); /* resolution, keyboard and sound as they chose them */
    portal_init();   /* where os-portal reaches us */
    appapi_init();   /* where apps reach the companion */
    presence_init(); /* the light, or its face (os-figure) */
    facet_reference_write();
    set_screen(SCR_SPLASH);
    lock_boot(); /* with a password: signed in first */

    input_fds(frame_fds);
    for (;;)
        main_frame();
}
