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
    uint64_t sig = tree_sig(home_dir(), tree_sig(STATE_DIR, 1469598103934665603ull));
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

void main_frame(void) {
    A.now = now_ms();
    wm_frame_begin();
    wls_dispatch();
    portal_poll(); /* os-portal: file choosers, notifications */
    appapi_poll(); /* apps using the companion: its AI, voice, face, memory */
    models_poll(); /* model downloads, wherever they were started */
    mind_builtin_tick(); /* the model that comes with it, once it has been read from the disc */
    presence_poll(); /* its frames, and what it should know */
    input_poll(&A.in);
    if (A.in.clicked || A.in.pressed || A.in.nkeys || A.in.wheel)
        nerve_person_touched(); /* what follows from this is theirs (nerve.c) */
    ui_text_menu_input(); /* a text field's right-click menu, when it is up, has the pointer */
    input_state real = A.in;
    if (modal) /* a question is up: the screen goes on underneath, its input is the question's */
        input_clear(&A.in);
    clock_frame(); /* the automatic time zone, the network's time (clock.c) */
    onboard_keep(); /* onboarding's place, kept as it changes (onboard_talk.inc) */
    if (lock_frame()) /* locked: the lock screen is everything (lock.c) */
        goto drawn;
    switch (A.screen) {
    case SCR_SPLASH: splash_draw(); break;
    case SCR_CHOOSE: choose_draw(); break;
    case SCR_MODELS: models_draw(); break;
    case SCR_CONNECT: connect_draw(); break;
    case SCR_ONBOARD: onboard_draw(); break;
    case SCR_DESIGNING: designing_draw(); break;
    case SCR_DESKTOP: desktop_draw(); break;
    }
    if (modal) {
        A.in = real;
        modal();
    }
drawn:
    ui_text_menu_draw();
    capture_frame(false); /* a recording without the pointer takes this */
    ui_cursor(A.frame, A.in.mx, A.in.my);
    wm_cursor(A.frame, A.in.mx, A.in.my); /* resize arrows on a window's border */
    capture_frame(true); /* ...with it, this */
    if (A.screen != SCR_SPLASH) /* (until there is something to show, the boot screen stays) */
        display_present(A.frame);
    wm_frame_end();
    wls_frame_done((uint32_t)A.now); /* apps draw their next frame now */
    keep_on_disk();
    keep_home_theirs();
    keep_account_name();

    /* Sleep until input arrives, an app has something to say, or the
     * next animation frame (~30 fps). */
    struct pollfd p[160] = {{.fd = frame_fds[0], .events = POLLIN}, {.fd = frame_fds[1], .events = POLLIN}};
    int wfds[158], nw = wls_fds(wfds, 30);
    nw += portal_fds(wfds + nw, 158 - nw);
    nw += presence_fds(wfds + nw, 158 - nw);
    nw += appapi_fds(wfds + nw, 158 - nw);
    for (int i = 0; i < nw; i++)
        p[2 + i] = (struct pollfd){.fd = wfds[i], .events = POLLIN};
    poll(p, (nfds_t)(2 + nw), wm_poll_ms(33));
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
