/* os-shell: the whole visible the OS experience. First boot (choose a
 * mind, connect, get to know each other), the AI designing the desktop, and
 * the desktop itself, drawn straight to /dev/fb0 from the AI's design. */
#include "presence.h"
#include "app.h"
#include "fm/fm.h"
#include "wallpaper.h"
#include "wl/wl.h"
#include "store.h"
#include "mind.h"
#include "wm.h"
#include "settings.h"
#include "portal.h"
#include "capture.h"
#include "models.h"

#include <dirent.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof A.toast, fmt, ap);
    va_end(ap);
    A.toast_until = A.now + 2500 + 45 * strlen(A.toast); /* long enough to read */
}

static void splash_draw(void) {
    float t = (float)(A.now - A.screen_since) / 1000.0f;
    ui_backdrop(RGBA(14, 11, 12, 255), RGBA(120, 46, 40, 255), RGBA(232, 131, 107, 255), RGBA(44, 18, 26, 255));
    float fade = t < 1.2f ? t / 1.2f : 1;
    ui_orb(A.frame, A.W / 2.0f, A.H / 2.0f - 50, 54 * fade + 1, RGBA(232, 131, 107, 255), RGBA(255, 214, 170, 255), false);
    font *f = font_get("Fraunces", 52), *s = font_get("Inter", 18);
    draw_text_centered(A.frame, f, (rect){0, A.H / 2 + 50, A.W, 70}, "Operating System", color_alpha(RGBA(242, 233, 226, 255), fade));
    draw_text_centered(A.frame, s, (rect){0, A.H / 2 + 120, A.W, 30}, "a computer that gets to know you",
                       color_alpha(RGBA(168, 150, 142, 255), fade));
    if (t > 3.2f) {
        if (A.have_design)
            desktop_enter();
        else if (A.cfg.provider[0] && A.cfg.model[0])
            onboard_start();
        else if (A.provider)
            set_screen(SCR_CONNECT);
        else
            set_screen(SCR_CHOOSE);
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
    if (!usable)
        return;
    char *res;
    if (saving && job_poll(saving, &res)) {
        LOG("store: %s\n", res ? res : "?");
        free(res);
        job_free(saving);
        saving = NULL;
    }
    if (A.now - last_check < 2000)
        return;
    last_check = A.now;
    if (!store_available()) {
        usable = false;
        return;
    }
    uint64_t sig = tree_sig("/root", tree_sig(STATE_DIR, 1469598103934665603ull));
    if (!primed) {
        primed = true;
        saved_sig = sig;
        return;
    }
    if (sig != saved_sig && !changed_at)
        changed_at = A.now;
    if (changed_at && A.now - changed_at > 3000 && !saving) {
        saving = job_start(save_child, NULL);
        saved_sig = sig;
        changed_at = 0;
    }
}

int main(void) {
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
    mkdir(STATE_DIR, 0700);
    /* A home with somewhere to put things. */
    static const char *const home[] = {"/root/Documents", "/root/Music", "/root/Pictures", "/root/Projects", NULL};
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
    int kept = store_restore();
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
    FILE *nf = fopen("/root/notes.txt", "r");
    if (nf) {
        A.notes.len = (int)fread(A.notes.buf, 1, sizeof A.notes.buf - 1, nf);
        A.notes.buf[A.notes.len] = 0;
        fclose(nf);
    }
    settings_boot(); /* resolution, keyboard and sound as they chose them */
    portal_init();   /* where os-portal reaches us */
    presence_init(); /* the light, or her face (os-figure) */
    set_screen(SCR_SPLASH);

    int fds[2];
    input_fds(fds);
    for (;;) {
        A.now = now_ms();
        wm_frame_begin();
        wls_dispatch();
        portal_poll(); /* os-portal: file choosers, notifications */
        models_poll(); /* model downloads, wherever they were started */
        presence_poll(); /* her frames, and what she should know */
        input_poll(&A.in);
        switch (A.screen) {
        case SCR_SPLASH: splash_draw(); break;
        case SCR_CHOOSE: choose_draw(); break;
        case SCR_MODELS: models_draw(); break;
        case SCR_CONNECT: connect_draw(); break;
        case SCR_ONBOARD: onboard_draw(); break;
        case SCR_DESIGNING: designing_draw(); break;
        case SCR_DESKTOP: desktop_draw(); break;
        }
        capture_frame(false); /* a recording without the pointer takes this */
        ui_cursor(A.frame, A.in.mx, A.in.my);
        wm_cursor(A.frame, A.in.mx, A.in.my); /* resize arrows on a window's border */
        capture_frame(true); /* ...with it, this */
        display_present(A.frame);
        wm_frame_end();
        wls_frame_done((uint32_t)A.now); /* apps draw their next frame now */
        keep_on_disk();

        /* Sleep until input arrives, an app has something to say, or the
         * next animation frame (~30 fps). */
        struct pollfd p[40] = {{.fd = fds[0], .events = POLLIN}, {.fd = fds[1], .events = POLLIN}};
        int wfds[38], nw = wls_fds(wfds, 30);
        nw += portal_fds(wfds + nw, 38 - nw);
        nw += presence_fds(wfds + nw, 38 - nw);
        for (int i = 0; i < nw; i++)
            p[2 + i] = (struct pollfd){.fd = wfds[i], .events = POLLIN};
        poll(p, (nfds_t)(2 + nw), wm_poll_ms(33));
    }
}
