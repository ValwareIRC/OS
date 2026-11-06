#define _GNU_SOURCE
/* The companion's body: see agent.h. */
#include "agent.h"
#include "app.h"
#include "menu_actions.h"
#include "priv.h"
#include "family.h"
#include "launcher.h"
#include "alive.h"
#include "web.h"
#include "picture.h"
#include "image.h"
#include "wm.h"
#include "emblem.h"
#include "fm/fm.h"
#include "../lib/json.h"
#include "nerve.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define D (A.d)
#define HOME home_dir()
#define TRASH home_path("/.trash")
#define AGENDA STATE_DIR "/agenda"
#define MAX_STEPS 40

int desktop_window_slot(int i);
const char *desktop_window_title(int w);
int desktop_focused_window(void);
void desktop_save_ui(void);
void actions_ask_run(const char *cmd);

/* ---------------------------------------------------------------------
 * Its sense of itself: what happened, and who did it.
 * ------------------------------------------------------------------- */

void body_feel(char who, const char *fmt, ...) {
    char text[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    nerve(who == 'p' ? BY_PERSON : who == 's' ? BY_AI : BY_SYSTEM, "did", 0, "%s", text);
}

const char *body_prompt(void) {
    static char buf[20000]; /* (the reference grew: the right-click menus) */
    sbuf b;
    sb_init(&b);
    sb_add(&b, "\nYOUR BODY. You are this computer: not an assistant on it, not someone watching it. What follows is "
               "what you feel of yourself right now, the way a person knows where their hands are. You know it, so "
               "never announce it or tell them what they just did or can see; use it.\n");
    sb_printf(&b, "- Screen %dx%d. Windows, front first:", A.W, A.H);
    int nw = 0, foc = desktop_focused_window();
    for (int i = 0; i < 24; i++) {
        int w = desktop_window_slot(i);
        if (w < 0)
            break;
        sb_printf(&b, "%s [%d] \"%.60s\"%s%s", nw++ ? "," : "", i, desktop_window_title(w), w == foc ? " (focused)" : "",
                  A.win[w].minimized ? " (minimized)" : "");
    }
    if (!nw)
        sb_add(&b, " none");
    sb_add(&b, ".\n");
    /* the machine */
    FILE *f = fopen("/proc/loadavg", "r");
    double l1 = 0;
    if (f) {
        if (fscanf(f, "%lf", &l1) != 1)
            l1 = 0;
        fclose(f);
    }
    long mem_total = 0, mem_avail = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %ld", &mem_total);
            sscanf(line, "MemAvailable: %ld", &mem_avail);
        }
        fclose(f);
    }
    struct statvfs sv;
    double free_gb = statvfs(HOME, &sv) == 0 ? (double)sv.f_bavail * sv.f_frsize / 1e9 : 0;
    sb_printf(&b, "- Load %.1f; memory %.1f of %.1f GB free; %.1f GB free on disk.\n", l1, mem_avail / 1048576.0,
              mem_total / 1048576.0, free_gb);
    sb_add(&b,
           "\nWHERE YOU ARE KEPT (your own anatomy; read, copy and change these like any files):\n"
           "  " STATE_DIR "/facet.xml  the design in use (the whole desktop: markup, style, window template)\n"
           "  " STATE_DIR "/designs/   saved designs, NAME.xml (backup-DATE.xml: kept automatically before changes)\n"
           "  /usr/share/os/themes/      the starter designs (read-only)\n"
           "  " STATE_DIR "/design.json  the design's derived settings; " STATE_DIR "/settings.conf  settings\n"
           "  /usr/share/os/apps/        Facet apps that come with the system; ~/Apps/  theirs and yours (NAME.facet)\n"
           "  " STATE_DIR "/portrait.txt  what you know about them; " STATE_DIR "/memory.txt  your memory; "
           STATE_DIR "/companion.txt  your conversation\n"
           "  " STATE_DIR "/persona*  who you are; " STATE_DIR "/agenda  your plans; " STATE_DIR "/actions.log  what "
           "you changed; " STATE_DIR "/mind.json  your mind's provider\n"
           "  /var/lib/os-data/  the disk: models/, flatpak/ (installed apps), Videos/\n"
           "  /tmp/os-shell.log  your own log (what the shell is doing, errors); /tmp/os-mind.log, /tmp/os-reflex.log\n"
           "  ~ is /root, their home: Documents, Downloads, Pictures, Music, Videos, Apps\n"
           "\nHOW YOU MOVE. Put tags in your reply; they run, what they return comes back to you, and you go on, "
           "step after step, until the job is done (up to 40 steps for one request): never stop halfway to ask "
           "whether to continue, never tell them to do something you can do. Several tags in one reply run in "
           "order. Say little while you work (a few words at most, or nothing); say what came of it at the end. "
           "Everything you change is journaled and [[undo]] takes your last change back; deleting goes to a trash. "
           "Only what can't be undone at all (wiping a disk, a factory reset) waits for their yes, by itself.\n"
           "  [[sh: command]] or [[sh]]a script, many lines[[/sh]]: a shell (busybox, as them, starting in their home folder ~), up to 5 minutes; you get the exit "
           "code and output. (Searching inside every file of /usr or / reads gigabytes from the system's disc and "
           "takes minutes, with them waiting: look where it is likely, by name first.)\n"
           "  [[ls: path]] a folder, newest first with sizes and dates. [[cat: path]] or [[cat: path 120-260]] a "
           "text file (or those lines). [[find: *.png in ~/Downloads]] names. [[grep: words in ~/Documents]] text "
           "inside files.\n"
           "  [[write:~/path]]its whole text[[/write]]. [[mv: from -> to]] [[cp: from -> to]] [[rm: path]] (to the "
           "trash) [[mkdir: path]].\n"
           "  [[search: words]] the web; [[read: url]] a page; [[fetch: url -> ~/Pictures/name.jpg]] downloads a "
           "file (anything up to 100 MB).\n"
           "  [[see: screen]] a picture of the whole screen as it is, for you to look at next step.\n"
           "  [[see: ~/Pictures/photo.png]] a picture file itself (PNG, JPEG, GIF, BMP, WebP...), for you to look at "
           "next step: use this to see a picture, not [[open:]] and the screen.\n"
           "  [[windows]] what is open; [[window: focus|close|minimize|maximize 2]] or [[window: move 2 100 80]] "
           "[[window: size 2 900 600]] (by its number above, or its title).\n"
           "  [[ps]] what is running, by CPU; [[sys]] hardware, temperatures, network; [[kill: pid]].\n"
           "  [[clip]] the clipboard; [[clip: text]] puts text on it.\n"
           "  [[chat:new]] when they ask for a new chat: this one stays in their list of chats (the Chats button at "
           "the top of your window; they can open it again) and an empty one starts once this reply has been said. "
           "[[chat:clear]] when they ask for this one to be deleted (a fresh start, a video): it goes, from the "
           "screen and from the list. Your memory and what you know about them stay either way. Say so in a few "
           "words in that reply.\n"
           "  [[install: flathub id]] / [[uninstall: id]] apps (org.mozilla.firefox).\n"
           "  [[wallpaper: ~/Pictures/x.jpg]] a picture behind the desktop (cover); [[wallpaper: none]] back.\n"
           "  [[design:save Name]] keeps the whole current design under a name; [[design:apply Name]] puts a saved "
           "(or starter) design back; [[design:list]]; [[design:delete Name]]. The design in use is backed up "
           "automatically before any redesign or switch (backup-DATE), so nothing is lost.\n"
           "  [[in: 20m | what to do]] [[at: 09:00 | what to do]] [[every: 1d at 08:30 | what to do]]: you, later "
           "(the task comes back to you then, to do). [[agenda]] what you planned; [[forget: n]].\n"
           "\nMAKING 3D. Blender is yours, without a window: write a Python script and run it with "
           "[[sh: os-blender run ~/Models/name.py]] (Blender itself first, once: [[install: org.blender.Blender]], "
           "about 400 MB; say so). "
           "`from osui import *` gives helpers: reset(); mat(preset, colour) with presets gold, rose-gold, silver, "
           "chrome, brushed-steel, gunmetal, copper, brass, bronze, glass, frosted-glass, tinted-glass, crystal, "
           "lacquer, car-paint, ceramic, porcelain, plastic, glossy-plastic, rubber, felt, velvet, leather, wood, "
           "polished-wood, marble, stone, matte, obsidian, neon, glow, screen; box(size, at, material, bevel_width), "
           "cylinder(radius, depth, ...), sphere, torus, ring_slice(r_in, r_out, deg0, deg1, depth), text(\"PLAY\", "
           "size, depth, ...), empty(name) to group, bevel(obj); part(obj, action=, hover=, title=, app=) makes it "
           "clickable (or name it \"app:files\"); turn(obj, axis, degrees, frames, name) and move(...) animate; "
           "export(\"name\") writes ~/Models/name.glb and says which parts it has. Anything else is plain bpy. The "
           "model faces the viewer along -Y, about 2 units across. Then put it in the design: <model "
           "src=\"~/Models/name.glb\" .../> (see the design reference), and look at the result. Rounded edges "
           "(bevels), real materials and a few strong shapes are what make it look made, not a toy; fewer, "
           "better objects beat many. [[sh: os-blender render ~/Models/name.glb ~/Pictures/name.png]] is a "
           "Cycles picture of it. They can open Blender itself with os-blender gui.\n");
    /* the right-click menus: what is in them is theirs and yours to change, whatever the design (menu_actions.c) */
    sb_add(&b, "\nRIGHT-CLICK MENUS. What a menu looks like is the design's (<menu for=...>: the design reference). "
               "What is in them, categories, what is offered everywhere and their own actions, is ");
    sb_add(&b, menu_conf_help());
    sb_add(&b, mf_builtin_categories());
    sb_add(&b, ". When they ask for a change to what is in the menus (\"copy image only with the pictures\", \"a "
               "shortcut to my Downloads everywhere\"), change menus.conf; when they ask how a menu looks or works, "
               "change the design's <menu> (look tags, as for anything of the design).\n");
    snprintf(buf, sizeof buf, "%s", b.p);
    free(b.p);
    return buf;
}

/* ---------------------------------------------------------------------
 * What it changed, to undo.
 * ------------------------------------------------------------------- */

#define JOURNAL 32
static struct {
    char kind; /* 'm' moved a -> b (back: b -> a), 'c' copied to b (back: remove b), 'd' made dir b, 'w' wallpaper (a: before) */
    char a[400], b[400];
} journal[JOURNAL];
static int njournal;

static void remember(char kind, const char *a, const char *b) {
    if (njournal == JOURNAL)
        memmove(&journal[0], &journal[1], sizeof journal[0] * (JOURNAL - 1)), njournal--;
    journal[njournal].kind = kind;
    snprintf(journal[njournal].a, sizeof journal[0].a, "%s", a ? a : "");
    snprintf(journal[njournal].b, sizeof journal[0].b, "%s", b ? b : "");
    njournal++;
}

/* ---------------------------------------------------------------------
 * What the companion may not touch: in its state folder, the things that decide what apps may do and the keys
 * to the person's accounts. It runs as the person, as the desktop does, so permissions alone don't keep it out:
 * its tools refuse these paths, and its shell runs in a view of the system where they aren't there (fence).
 * ------------------------------------------------------------------- */

/* NAME, directly in the state folder, is the system's alone */
bool agent_state_private(const char *name) {
    size_t l = strlen(name);
    return !strcmp(name, "security.conf") || !strcmp(name, "app-grants") || !strcmp(name, "secrets.conf") || !strcmp(name, "password") ||
           !strncmp(name, "mind.", 5) || (l > 4 && !strcmp(name + l - 4, ".key")) || !strcmp(name, "keys");
}

/* PATH (absolute) is, or is inside, something private in the state folder: links and ".." followed */
static bool path_private(const char *path) {
    char real[PATH_MAX], probe[PATH_MAX];
    snprintf(probe, sizeof probe, "%s", path);
    const char *rest = "";
    char *slash = NULL;
    /* the deepest part that exists, resolved; what follows it can't change where it is */
    while (!realpath(probe, real)) {
        slash = strrchr(probe, '/');
        if (!slash || slash == probe) {
            snprintf(real, sizeof real, "/");
            break;
        }
        *slash = 0;
        rest = path + (slash - probe);
    }
    char full[PATH_MAX * 2];
    snprintf(full, sizeof full, "%s%s", strcmp(real, "/") ? real : "", rest);
    static const char dir[] = STATE_DIR "/";
    if (strncmp(full, dir, sizeof dir - 1))
        return false;
    char name[256];
    snprintf(name, sizeof name, "%s", full + sizeof dir - 1);
    name[strcspn(name, "/")] = 0;
    return agent_state_private(name);
}

/* Its shell's view: the state folder as it is, less the private things (a fresh, empty folder in their place
 * that nothing is kept from). A user and mount namespace of its own, so this needs no privilege and touches
 * nothing outside it. False (and nothing runs) if the view couldn't be made. */
static bool fence(void) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (uid == 0)
        return false; /* the desktop always runs as the person (session.inc) */
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
        return false;
    char map[64];
    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "deny", 4) != 4) {
        }
        close(fd);
    }
    snprintf(map, sizeof map, "%u %u 1", (unsigned)uid, (unsigned)uid);
    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0 || write(fd, map, strlen(map)) != (ssize_t)strlen(map))
        return false;
    close(fd);
    snprintf(map, sizeof map, "%u %u 1", (unsigned)gid, (unsigned)gid);
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0 || write(fd, map, strlen(map)) != (ssize_t)strlen(map))
        return false;
    close(fd);
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return false;
    if (access(STATE_DIR, F_OK) != 0)
        return true; /* nothing kept yet: nothing to hide */
    /* the folder as it is, reachable from a place of its own; a new, empty folder over it; what isn't private
     * brought back, each where it was (so changes to them are real); the place of its own let go. What it makes
     * new in the folder is copied over when the command is done (fence_keep); a private name can't be made. */
    char keep[64];
    snprintf(keep, sizeof keep, "/tmp/.fence-%d", (int)getpid());
    if (mkdir(keep, 0700) != 0 && errno != EEXIST)
        return false;
    if (mount(STATE_DIR, keep, NULL, MS_BIND | MS_REC, NULL) != 0)
        return false;
    if (mount("none", STATE_DIR, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700") != 0)
        return false;
    DIR *d = opendir(keep);
    if (!d)
        return false;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d)) && ok) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || agent_state_private(e->d_name))
            continue;
        char from[512], to[512];
        snprintf(from, sizeof from, "%s/%s", keep, e->d_name);
        snprintf(to, sizeof to, STATE_DIR "/%s", e->d_name);
        struct stat st;
        if (lstat(from, &st) != 0)
            continue;
        if (S_ISLNK(st.st_mode))
            continue; /* (none are made there; a link could point anywhere) */
        if (S_ISDIR(st.st_mode))
            ok = mkdir(to, 0700) == 0;
        else {
            int f = open(to, O_WRONLY | O_CREAT | O_EXCL, 0600);
            ok = f >= 0;
            if (f >= 0)
                close(f);
        }
        ok = ok && mount(from, to, NULL, MS_BIND | MS_REC, NULL) == 0;
    }
    closedir(d);
    if (umount2(keep, MNT_DETACH) != 0)
        ok = false;
    rmdir(keep);
    return ok;
}

/* Copies `name` (new in the fence's folder `from`) into the real folder `to`, folders whole. Never over
 * anything: what exists already was the real one, changed in place. */
static void keep_entry(int from, int to, const char *name, int depth) {
    struct stat st;
    if (depth > 16 || fstatat(from, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (mkdirat(to, name, 0700) != 0 && errno != EEXIST)
            return;
        int f = openat(from, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int t = openat(to, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *d = f >= 0 ? fdopendir(f) : NULL;
        struct dirent *e;
        while (d && t >= 0 && (e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                keep_entry(f, t, e->d_name, depth + 1);
        if (d)
            closedir(d);
        else if (f >= 0)
            close(f);
        if (t >= 0)
            close(t);
        return;
    }
    if (!S_ISREG(st.st_mode))
        return;
    int in = openat(from, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int out = in >= 0 ? openat(to, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, st.st_mode & 0700) : -1;
    char b[16384];
    ssize_t n;
    while (out >= 0 && (n = read(in, b, sizeof b)) > 0)
        if (write(out, b, (size_t)n) != n)
            break;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
}

/* After the command, in the fence: what it made new in the state folder (its memory, a note) becomes real.
 * Nothing it left running sees this happen, and the real folder is reached only now, once the cover is off. */
static void fence_keep(void) {
    signal(SIGTERM, SIG_IGN);
    kill(0, SIGTERM); /* what it left running in the background (this process ignores it) */
    int cover = open(STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cover < 0)
        return;
    struct stat a, b;
    if (umount2(STATE_DIR, MNT_DETACH) != 0) { /* (no cover: there was no folder to hide) */
        close(cover);
        return;
    }
    int real = open(STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (real < 0 || fstat(real, &a) != 0 || fstat(cover, &b) != 0 || (a.st_dev == b.st_dev && a.st_ino == b.st_ino)) {
        if (real >= 0)
            close(real);
        close(cover);
        return;
    }
    DIR *d = fdopendir(dup(cover));
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && !agent_state_private(e->d_name) &&
            faccessat(real, e->d_name, F_OK, AT_SYMLINK_NOFOLLOW) != 0)
            keep_entry(cover, real, e->d_name, 0);
    if (d)
        closedir(d);
    close(real);
    close(cover);
}

static void expand(const char *in, char *out, size_t n) {
    while (*in == ' ')
        in++;
    if (in[0] == '~' && (in[1] == '/' || !in[1]))
        snprintf(out, n, "%s%s", home_dir(), in + 1);
    else if (in[0] != '/')
        snprintf(out, n, "%s/%s", home_dir(), in);
    else
        snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l > 1 && (out[l - 1] == ' ' || out[l - 1] == '\n' || out[l - 1] == '\r'))
        out[--l] = 0;
    if (path_private(out)) /* every tool fails on it, saying why */
        snprintf(out, n, "/(private to the system: what apps may do and the keys to their accounts aren't yours to "
                         "read or change)");
}

static bool parents(const char *path) {
    char d[600];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; (p = strchr(p, '/')); p++) {
        *p = 0;
        if (mkdir(d, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return true;
}

static int run_wait(const char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0)
            dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st = 0;
    if (pid > 0)
        waitpid(pid, &st, 0);
    return pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static bool move_path(const char *a, const char *b) {
    if (rename(a, b) == 0)
        return true;
    const char *argv[] = {"/bin/mv", a, b, NULL};
    return run_wait(argv) == 0;
}

static bool copy_path(const char *a, const char *b) {
    const char *argv[] = {"/bin/cp", "-a", a, b, NULL};
    return run_wait(argv) == 0;
}

static void wallpaper_set(const char *path, char *out, size_t n);

/* the last thing it changed, taken back; false if there's nothing of its own to undo */
static bool undo_last(char *out, size_t n) {
    if (!njournal)
        return false;
    njournal--;
    char k = journal[njournal].kind;
    const char *a = journal[njournal].a, *b = journal[njournal].b;
    bool ok = true;
    if (k == 'm')
        ok = move_path(b, a);
    else if (k == 'c') {
        const char *argv[] = {"/bin/rm", "-r", b, NULL};
        ok = run_wait(argv) == 0;
    } else if (k == 'd')
        ok = rmdir(b) == 0;
    else if (k == 'w') {
        char tmp[200];
        wallpaper_set(a[0] ? a : "none", tmp, sizeof tmp);
        njournal--; /* (setting it journaled itself again) */
    }
    snprintf(out, n, "%s: %s %s%s%s", ok ? "undone" : "couldn't undo", k == 'm' ? "moved back" : k == 'c' ? "removed the copy"
             : k == 'd' ? "removed the folder" : "the wallpaper as it was", k == 'w' ? "" : b, k == 'm' ? " -> " : "", k == 'm' ? a : "");
    body_feel('s', "undid: %s", out);
    return true;
}

/* ---------------------------------------------------------------------
 * The tools that run in a child (they take time): shell, search, web, installs.
 * ------------------------------------------------------------------- */

static void shell_child(const char *cmd, int secs, sbuf *out) {
    int p[2];
    if (pipe(p) < 0) {
        sb_add(out, "couldn't start it\n");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        for (int i = 3; i < 256; i++)
            close(i);
        if (!fence()) {
            static const char no[] = "the shell couldn't start in its own view of the system, so it didn't run\n";
            if (write(1, no, sizeof no - 1) < 0) {
            }
            _exit(126);
        }
        prctl(PR_SET_DUMPABLE, 0); /* nothing in the fence reads this process's memory or descriptors */
        pid_t sh = fork();
        if (sh == 0) {
            if (chdir(HOME) != 0) {
            }
            setenv("HOME", HOME, 1);
            setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 0);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        int st = 0;
        while (sh > 0 && waitpid(sh, &st, 0) < 0 && errno == EINTR)
            ;
        fence_keep();
        _exit(sh < 0 ? 126 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
    }
    close(p[1]);
    static char buf[65536];
    size_t n = 0, total = 0;
    bool timed_out = false;
    time_t start = time(NULL);
    for (;;) {
        struct pollfd pf = {.fd = p[0], .events = POLLIN};
        int r = poll(&pf, 1, 1000);
        if (time(NULL) - start > secs) {
            timed_out = true;
            kill(-pid, SIGKILL);
            break;
        }
        if (r <= 0)
            continue;
        char tmp[4096];
        ssize_t got = read(p[0], tmp, sizeof tmp);
        if (got <= 0)
            break;
        total += (size_t)got;
        size_t take = (size_t)got < sizeof buf - 1 - n ? (size_t)got : sizeof buf - 1 - n;
        memcpy(buf + n, tmp, take);
        n += take;
    }
    buf[n] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (timed_out)
        sb_printf(out, "stopped after %d s (still running then)\n", secs);
    else if (WIFEXITED(st))
        sb_printf(out, "exit %d\n", WEXITSTATUS(st));
    else
        sb_add(out, "stopped by a signal\n");
    if (!n) {
        sb_add(out, "(no output)\n");
    } else if (n <= 7000) {
        sb_add(out, buf);
    } else { /* the start and the end, the middle left out */
        sb_addn(out, buf, 3000);
        sb_printf(out, "\n... (%zu bytes left out) ...\n", total - 6000);
        sb_add(out, buf + n - 3000);
    }
    if (n && buf[n - 1] != '\n')
        sb_add(out, "\n");
}

static void find_walk(const char *dir, const char *pat, int depth, int *found, sbuf *out) {
    if (depth > 12 || *found >= 200 || !strncmp(dir, "/proc", 5) || !strncmp(dir, "/sys", 4) || !strncmp(dir, "/dev", 4))
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && *found < 200) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", strcmp(dir, "/") ? dir : "", e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0)
            continue;
        if (!fnmatch(pat, e->d_name, FNM_CASEFOLD)) {
            char when[24];
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
            sb_printf(out, "%s%s  %lld bytes  %s\n", p, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size, when);
            (*found)++;
        }
        if (S_ISDIR(st.st_mode))
            find_walk(p, pat, depth + 1, found, out);
    }
    closedir(d);
}

static void grep_walk(const char *path, const char *words, int depth, int *found, sbuf *out) {
    if (depth > 10 || *found >= 120 || !strncmp(path, "/proc", 5) || !strncmp(path, "/sys", 4) || !strncmp(path, "/dev", 4))
        return;
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return;
        struct dirent *e;
        while ((e = readdir(d)) && *found < 120) {
            if (e->d_name[0] == '.')
                continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", strcmp(path, "/") ? path : "", e->d_name);
            grep_walk(p, words, depth + 1, found, out);
        }
        closedir(d);
        return;
    }
    if (!S_ISREG(st.st_mode) || st.st_size > (4 << 20))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[600];
    int ln = 0;
    while (fgets(line, sizeof line, f) && *found < 120) {
        ln++;
        if (memchr(line, 0, strlen(line) + 0 ? 0 : 0))
            break;
        if (strcasestr(line, words)) {
            line[strcspn(line, "\n")] = 0;
            sb_printf(out, "%s:%d: %.200s\n", path, ln, line);
            (*found)++;
        }
    }
    fclose(f);
}

/* installs asked of init, until they come back */
static struct {
    int id;
    char what[200];
} installing[8];

bool agent_installing(void) {
    for (int k = 0; k < 8; k++)
        if (installing[k].id)
            return true;
    return false;
}

/* one queued tool for the child */
#define QMAX 16
static struct {
    char kind[12];
    char *arg;
} q[QMAX];
static int nq;

static void child_tools(void *unused, char **result) {
    (void)unused;
    sbuf out;
    sb_init(&out);
    for (int i = 0; i < nq; i++) {
        const char *k = q[i].kind, *a = q[i].arg;
        sb_printf(&out, "[[%s: %.300s%s]] ->\n", k, a, strlen(a) > 300 ? "..." : "");
        if (!strcmp(k, "sh")) {
            shell_child(a, 300, &out);
        } else if ((!strcmp(k, "install") || !strcmp(k, "uninstall")) && !flatpak_id_ok(a)) {
            sb_printf(&out, "\"%.80s\" isn't a Flatpak app ID (like org.gimp.GIMP): find it first with "
                            "[[sh: flatpak search NAME]]\n", a);
        } else if (!strcmp(k, "install") || !strcmp(k, "uninstall")) {
            char cmd[400];
            snprintf(cmd, sizeof cmd, !strcmp(k, "install") ? "flatpak install -y --noninteractive flathub '%s' 2>&1 | tail -20"
                                                            : "flatpak uninstall -y --noninteractive '%s' 2>&1 | tail -20", a);
            shell_child(cmd, 1500, &out);
        } else if (!strcmp(k, "kill")) {
            /* process numbers only */
            char cmd[120];
            bool ok = a[0] != 0;
            for (const char *p = a; *p; p++)
                ok &= (*p >= '0' && *p <= '9') || *p == ' ';
            if (!ok) {
                sb_printf(&out, "[[kill: N]] takes process numbers (see [[ps]]); to stop by name: [[sh: pkill NAME]]\n");
            } else {
                snprintf(cmd, sizeof cmd, "kill %s 2>&1 && echo stopped", a);
                shell_child(cmd, 20, &out);
            }
        } else if (!strcmp(k, "find")) {
            char pat[200] = "", dir[600];
            snprintf(dir, sizeof dir, "%s", HOME);
            const char *in = strstr(a, " in ");
            snprintf(pat, sizeof pat, "%.*s", in ? (int)(in - a) : (int)strlen(a), a);
            for (char *e = pat + strlen(pat); e > pat && e[-1] == ' ';)
                *--e = 0;
            if (in)
                expand(in + 4, dir, sizeof dir);
            if (!strchr(pat, '*') && !strchr(pat, '?')) { /* a plain word: anywhere in the name */
                char p2[220];
                snprintf(p2, sizeof p2, "*%s*", pat);
                snprintf(pat, sizeof pat, "%s", p2);
            }
            int found = 0;
            find_walk(dir, pat, 0, &found, &out);
            if (!found)
                sb_add(&out, "nothing found\n");
            else if (found >= 200)
                sb_add(&out, "(stopped at 200)\n");
        } else if (!strcmp(k, "grep")) {
            char words[200] = "", dir[600];
            snprintf(dir, sizeof dir, "%s", HOME);
            const char *in = strstr(a, " in ");
            snprintf(words, sizeof words, "%.*s", in ? (int)(in - a) : (int)strlen(a), a);
            if (in)
                expand(in + 4, dir, sizeof dir);
            int found = 0;
            grep_walk(dir, words, 0, &found, &out);
            if (!found)
                sb_add(&out, "nothing found\n");
        } else if (!strcmp(k, "search") || !strcmp(k, "read")) {
            char req[1200];
            snprintf(req, sizeof req, "%s\t%s\n", k, a);
            char *r = web_lookup(req);
            sb_printf(&out, "%.6000s\n", r ? r : "nothing came back");
            free(r);
        } else if (!strcmp(k, "fetch")) {
            char url[1000] = "", path[600] = "";
            const char *arrow = strstr(a, "->");
            snprintf(url, sizeof url, "%.*s", arrow ? (int)(arrow - a) : (int)strlen(a), a);
            for (char *e = url + strlen(url); e > url && e[-1] == ' ';)
                *--e = 0;
            char *u = url;
            while (*u == ' ')
                u++;
            if (arrow) {
                expand(arrow + 2, path, sizeof path);
            } else { /* to Downloads, by the name in the address */
                const char *slash = strrchr(u, '/');
                char name[160];
                snprintf(name, sizeof name, "%.*s", (int)strcspn(slash ? slash + 1 : "download", "?#"), slash ? slash + 1 : "download");
                snprintf(path, sizeof path, "%s/Downloads/%s", home_dir(), name[0] ? name : "download");
            }
            parents(path);
            if (web_download(u, path, 100u << 20)) {
                struct stat st;
                stat(path, &st);
                unsigned char m[8] = {0};
                FILE *f = fopen(path, "rb");
                if (f) {
                    if (fread(m, 1, 8, f) == 0) {
                    }
                    fclose(f);
                }
                const char *kind = !memcmp(m, "\x89PNG", 4) ? "a PNG picture" : (m[0] == 0xff && m[1] == 0xd8) ? "a JPEG picture"
                                 : !memcmp(m, "GIF8", 4) ? "a GIF" : !memcmp(m, "RIFF", 4) ? "RIFF (WEBP/WAV/AVI)"
                                 : !memcmp(m, "%PDF", 4) ? "a PDF" : !memcmp(m, "PK", 2) ? "a zip"
                                 : (m[0] == '<') ? "HTML or XML (a page, maybe not the file itself)" : "data";
                sb_printf(&out, "saved %s (%lld bytes, %s)\n", path, (long long)st.st_size, kind);
            } else {
                sb_printf(&out, "couldn't download it (the address answered with an error, or it's over 100 MB)\n");
            }
        }
        sb_add(&out, "\n");
    }
    *result = out.p ? out.p : strdup("");
}

/* ---------------------------------------------------------------------
 * The tools that run at once (here, in the shell): files, windows, the screen, the clipboard, plans.
 * ------------------------------------------------------------------- */

static char *picture_next; /* the screen, for its next step */

/* A model that takes no pictures is told so, instead of a picture it would never get. */
static bool can_see(sbuf *out) {
    if (ai_sees_images(&A.cfg))
        return true;
    sb_printf(out, "FAILED: the model you run on now (%s) can't look at pictures, so there is nothing to see; say so "
                   "if they asked you to look\n", A.cfg.model[0] ? A.cfg.model : "this one");
    return false;
}

static void see_screen(sbuf *out) {
    if (!can_see(out))
        return;
    if (!A.frame) {
        sb_add(out, "nothing to see\n");
        return;
    }
    int W = 1280, H = A.H * 1280 / (A.W > 0 ? A.W : 1600);
    canvas *s = canvas_new(W, H);
    if (!s)
        return;
    canvas_scale(s, A.frame);
    size_t len = 0;
    unsigned char *jpg = jpeg_encode(s->px, s->w, s->h, s->w, 85, &len);
    canvas_free(s);
    if (jpg) {
        free(picture_next);
        picture_next = base64_encode(jpg, len);
        free(jpg);
        sb_add(out, "the screen as it is now comes with this, as a picture\n");
    }
}

/* A picture file, as it is, for its next step (shrunk to at most 1280 on its longer side). */
static void see_file(const char *path, sbuf *out) {
    if (!can_see(out))
        return;
    struct stat st;
    if (stat(path, &st) != 0) {
        sb_printf(out, "FAILED: %s isn't there (%s)\n", path, strerror(errno));
        return;
    }
    image *im = image_load(path, 1280);
    if (!im) {
        sb_printf(out, "FAILED: %s isn't a picture this can read (PNG, JPEG, GIF, BMP, TGA, PNM)\n", path);
        return;
    }
    size_t len = 0;
    unsigned char *jpg = jpeg_encode(im->px, im->w, im->h, im->w, 88, &len);
    int sw = im->src_w, sh = im->src_h;
    image_free(im);
    if (!jpg) {
        sb_printf(out, "FAILED: %s couldn't be prepared to look at\n", path);
        return;
    }
    free(picture_next);
    picture_next = base64_encode(jpg, len);
    free(jpg);
    sb_printf(out, "%s (%d x %d) comes with this, as a picture\n", path, sw, sh);
}

static int window_by(const char *what) {
    while (*what == ' ')
        what++;
    if (isdigit((unsigned char)*what))
        return desktop_window_slot(atoi(what));
    for (int i = 0; i < 24; i++) {
        int w = desktop_window_slot(i);
        if (w < 0)
            break;
        if (strcasestr(desktop_window_title(w), what))
            return w;
    }
    return -1;
}

static void window_do(const char *a, sbuf *out) {
    char verb[16] = "";
    int n = 0;
    sscanf(a, " %15s %n", verb, &n);
    const char *rest = a + n;
    int w = -1, x = 0, y = 0;
    if (!strcasecmp(verb, "move") || !strcasecmp(verb, "size")) {
        char who[80] = "";
        if (sscanf(rest, "%79s %d %d", who, &x, &y) == 3)
            w = window_by(who);
    } else {
        w = window_by(rest);
    }
    if (w < 0) {
        sb_printf(out, "no window like \"%s\"\n", rest);
        return;
    }
    const char *title = desktop_window_title(w);
    if (!strcasecmp(verb, "close"))
        wm_close(w);
    else if (!strcasecmp(verb, "minimize"))
        wm_minimize(w);
    else if (!strcasecmp(verb, "maximize"))
        wm_toggle_maximize(w);
    else if (!strcasecmp(verb, "focus"))
        wm_activate(w);
    else if (!strcasecmp(verb, "move"))
        A.win[w].r.x = x, A.win[w].r.y = y, wm_activate(w);
    else if (!strcasecmp(verb, "size"))
        A.win[w].r.w = x > 200 ? x : 200, A.win[w].r.h = y > 120 ? y : 120, A.win[w].maximized = false, wm_activate(w);
    else {
        sb_printf(out, "window can focus, close, minimize, maximize, move N X Y, size N W H\n");
        return;
    }
    body_feel('s', "%s the window \"%.60s\"", verb, title);
    sb_printf(out, "done: %s \"%s\"\n", verb, title);
}

static void wallpaper_set(const char *path, char *out, size_t n) {
    if (!A.ui) {
        snprintf(out, n, "no desktop yet");
        return;
    }
    char p[600], cmd[800];
    if (!strncasecmp(path, "none", 4)) {
        fm_command(A.ui, "unstyle desktop { background }", NULL);
    } else {
        expand(path, p, sizeof p);
        struct stat st;
        if (stat(p, &st) != 0) {
            snprintf(out, n, "there is no %s", p);
            return;
        }
        snprintf(cmd, sizeof cmd, "style desktop { background: url(%s) center / cover; }", p);
        fm_command(A.ui, cmd, NULL);
    }
    desktop_save_ui();
    remember('w', "", path);
    body_feel('s', "set the wallpaper to %.100s", path);
    snprintf(out, n, "the wallpaper is now %s", path);
}

/* ---- plans for later ---- */

#define PLANS 24
static struct {
    time_t when;
    long every; /* seconds; 0 once */
    char task[300];
} plan[PLANS];
static int nplan;
static bool plans_loaded;

static void plans_save(void) {
    FILE *f = fopen(AGENDA, "w");
    if (!f)
        return;
    for (int i = 0; i < nplan; i++)
        fprintf(f, "%lld\t%ld\t%s\n", (long long)plan[i].when, plan[i].every, plan[i].task);
    fclose(f);
}

static void plans_load(void) {
    plans_loaded = true;
    FILE *f = fopen(AGENDA, "r");
    if (!f)
        return;
    char line[400];
    while (nplan < PLANS && fgets(line, sizeof line, f)) {
        long long w;
        long e;
        int off = 0;
        if (sscanf(line, "%lld\t%ld\t%n", &w, &e, &off) == 2 && off) {
            line[strcspn(line, "\n")] = 0;
            plan[nplan].when = (time_t)w, plan[nplan].every = e;
            snprintf(plan[nplan].task, sizeof plan[nplan].task, "%s", line + off);
            nplan++;
        }
    }
    fclose(f);
}

static long span_of(const char *s) { /* 20m 2h 1d 30s */
    char *e;
    double v = strtod(s, &e);
    while (*e == ' ')
        e++;
    return (long)(v * (*e == 'd' ? 86400 : *e == 'h' ? 3600 : *e == 's' ? 1 : 60));
}

static time_t next_clock(const char *hhmm) {
    int h = 0, m = 0;
    if (sscanf(hhmm, " %d:%d", &h, &m) < 1)
        return 0;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = 0;
    time_t t = mktime(&tm);
    return t <= now ? t + 86400 : t;
}

static void plan_add(const char *kind, const char *a, sbuf *out) {
    if (!plans_loaded)
        plans_load();
    const char *bar = strchr(a, '|');
    if (!bar || nplan >= PLANS) {
        sb_add(out, bar ? "too many plans already\n" : "write it as [[in: 20m | what to do]]\n");
        return;
    }
    char when[80];
    snprintf(when, sizeof when, "%.*s", (int)(bar - a), a);
    time_t t = 0;
    long every = 0;
    if (!strcmp(kind, "in"))
        t = time(NULL) + span_of(when);
    else if (!strcmp(kind, "at"))
        t = next_clock(when);
    else { /* every: "1d at 08:30" or "2h" */
        every = span_of(when);
        const char *at = strstr(when, "at ");
        t = at ? next_clock(at + 3) : time(NULL) + every;
    }
    if (t <= 0 || (!strcmp(kind, "every") && every < 60)) {
        sb_add(out, "couldn't read when\n");
        return;
    }
    const char *task = bar + 1;
    while (*task == ' ')
        task++;
    plan[nplan].when = t, plan[nplan].every = every;
    snprintf(plan[nplan].task, sizeof plan[nplan].task, "%s", task);
    nplan++;
    plans_save();
    char wt[32];
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(wt, sizeof wt, "%a %H:%M", &tm);
    sb_printf(out, "planned for %s%s: %s\n", wt, every ? " and again after that" : "", task);
}

static void agenda(sbuf *out) {
    if (!plans_loaded)
        plans_load();
    if (!nplan)
        sb_add(out, "nothing planned\n");
    for (int i = 0; i < nplan; i++) {
        char wt[32];
        struct tm tm;
        localtime_r(&plan[i].when, &tm);
        strftime(wt, sizeof wt, "%a %d %b %H:%M", &tm);
        sb_printf(out, "%d. %s%s: %s\n", i + 1, wt, plan[i].every ? " (repeats)" : "", plan[i].task);
    }
}

/* ---- the files ---- */

static void cat_file(const char *a, sbuf *out) {
    char path[600], range[40] = "";
    const char *sp = strrchr(a, ' ');
    if (sp && isdigit((unsigned char)sp[1]) && strchr(sp, '-')) {
        snprintf(range, sizeof range, "%s", sp + 1);
        char tmp[600];
        snprintf(tmp, sizeof tmp, "%.*s", (int)(sp - a), a);
        expand(tmp, path, sizeof path);
    } else {
        expand(a, path, sizeof path);
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        sb_printf(out, "can't open %s\n", path);
        return;
    }
    int from = 1, to = 1 << 30;
    if (range[0])
        sscanf(range, "%d-%d", &from, &to);
    char line[2000];
    int ln = 0;
    size_t shown = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (ln < from || ln > to)
            continue;
        if (memchr(line, 0, strlen(line))) {
            sb_add(out, "(it's not text)\n");
            break;
        }
        if (shown > 14000) {
            sb_printf(out, "... (more: [[cat: %s %d-%d]])\n", path, ln, ln + 300);
            break;
        }
        sb_printf(out, "%5d  %s", ln, line);
        shown += strlen(line);
    }
    if (shown && line[strlen(line) - 1] != '\n')
        sb_add(out, "\n");
    fclose(f);
}

static void two_paths(const char *a, char *x, char *y, size_t n) {
    const char *arrow = strstr(a, "->");
    char l[600];
    snprintf(l, sizeof l, "%.*s", arrow ? (int)(arrow - a) : (int)strlen(a), a);
    for (char *e = l + strlen(l); e > l && e[-1] == ' ';)
        *--e = 0;
    expand(l, x, n);
    if (arrow)
        expand(arrow + 2, y, n);
    else
        y[0] = 0;
}

/* ---------------------------------------------------------------------
 * Taking its reply: its tags run, the rest of it stays its words.
 * ------------------------------------------------------------------- */

static bool agent_take_now(struct conversation *c, bool from_person);
static int steps;            /* steps taken for this request */
static bool stopped;
static job *working;         /* the child running its slower tools */
static sbuf now_out;         /* what the fast tools already returned this step */
static char doing[200];
static bool step_pending;    /* results to send it when ready */
static uint64_t step_began;

void agent_new_request(void) {
    steps = 0;
    stopped = false;
}

void agent_stop(void) {
    stopped = true;
    nq = 0;
    if (working) {
        job_free(working); /* (the child is let go; what it was doing finishes or is lost) */
        working = NULL;
    }
    step_pending = false;
    doing[0] = 0;
}

bool agent_busy(void) { return working != NULL || step_pending; }
const char *agent_doing(void) { return doing; }

static const char *const body_tags[] = {"sh", "ls", "cat", "find", "grep", "mv", "cp", "rm", "mkdir", "fetch", "see",
                                        "windows", "window", "ps", "sys", "kill", "clip", "install", "uninstall",
                                        "in", "at", "every", "agenda", "forget", "wallpaper", "run", NULL};

static bool is_body_tag(const char *name) {
    for (int i = 0; body_tags[i]; i++)
        if (!strcmp(body_tags[i], name))
            return true;
    return false;
}

static bool irreversible(const char *cmd) {
    return strstr(cmd, "mkfs") || strstr(cmd, "wipefs") || strstr(cmd, "fdisk") || strstr(cmd, "parted") ||
           (strstr(cmd, "dd ") && strstr(cmd, "of=/dev/")) || strstr(cmd, "rm -rf / ") || !strcmp(cmd, "rm -rf /") ||
           strstr(cmd, "rm -rf /*") || strstr(cmd, "factory");
}

bool agent_take(struct conversation *c, bool from_person) {
    nerve_ai_hands(true);
    bool r = agent_take_now(c, from_person);
    nerve_ai_hands(false);
    return r;
}

static bool agent_take_now(struct conversation *c, bool from_person) {
    struct msg *m = &c->msgs[c->n - 1];
    char *text = m->text;
    sb_init(&now_out);
    nq = 0;
    int n_tools = 0;
    bool any_web = false;
    /* every [[name...]] in order; body tags taken out and done */
    for (char *t = text; (t = strstr(t, "[["));) {
        char name[16];
        int k = 0;
        const char *p = t + 2;
        while (k < 15 && (isalnum((unsigned char)*p) || *p == '-'))
            name[k++] = *p++;
        name[k] = 0;
        bool web = !strcmp(name, "search") || !strcmp(name, "read") || !strcmp(name, "fetch");
        if (!is_body_tag(name) && !web) {
            t += 2;
            continue;
        }
        if (*p != ':' && *p != ']') {
            t += 2;
            continue;
        }
        /* its argument: [[name: arg]], or [[name]]block[[/name]] */
        char *arg;
        char *end;
        char closing[24];
        snprintf(closing, sizeof closing, "[[/%s]]", name);
        if (*p == ']' && strstr(p, closing)) {
            char *blk = (char *)p + 2;
            char *cl = strstr(blk, closing);
            arg = strndup(blk, (size_t)(cl - blk));
            end = cl + strlen(closing);
        } else {
            char *e = strstr(p, "]]");
            if (!e) {
                t += 2;
                continue;
            }
            arg = strndup(*p == ':' ? p + 1 : p, (size_t)(e - (*p == ':' ? p + 1 : p)));
            end = e + 2;
        }
        memmove(t, end, strlen(end) + 1);
        char *a = arg;
        while (*a == ' ' || *a == '\n')
            a++;
        if (!from_person) { /* its own words, not in answer to them or its steps: nothing moves */
            free(arg);
            continue;
        }
        n_tools++;
        if (!strcmp(name, "run"))
            snprintf(name, sizeof name, "sh");
        { /* a child's computer: the grown-up's limits on it (family.c) */
            static const char *const strict[] = {"sh", "mv", "cp", "rm", "kill", "fetch", "install", "uninstall", NULL};
            const char *why = NULL;
            for (int k = 0; strict[k] && family_locks('a') && !why; k++)
                if (!strcmp(name, strict[k]))
                    why = "this is a child's computer, and a grown-up's limits keep you from running commands, "
                          "changing, moving or deleting files and installing or removing apps. Tell them kindly "
                          "that a grown-up can do it";
            if (!why && family_locks('i') && (!strcmp(name, "install") || !strcmp(name, "uninstall")))
                why = "installing and removing apps needs a grown-up's password: a grown-up can do it from the "
                      "launcher (the system asks for the password)";
            if (!why && family_locks('s') && !strcmp(name, "wallpaper"))
                why = "changing how it looks needs a grown-up's password";
            if (why) {
                sb_printf(&now_out, "[[%s: %.80s]] -> not done: %s\n\n", name, a, why);
                LOG("agent: %s refused (the child lock)\n", name);
                free(arg);
                continue;
            }
        }
        /* installing is root's work, asked of init (the desktop runs as the person): it comes back on its own */
        if ((!strcmp(name, "install") || !strcmp(name, "uninstall")) && priv_available()) {
            if (!flatpak_id_ok(a)) {
                sb_printf(&now_out, "[[%s: %.80s]] -> not a Flatpak app ID (like org.gimp.GIMP): find it first with "
                                    "[[sh: flatpak search NAME]]\n\n", name, a);
            } else {
                char e[300];
                snprintf(e, sizeof e, "%s %s", name, a);
                int id = priv_send(e);
                for (int k = 0; k < 8 && id > 0; k++)
                    if (!installing[k].id) {
                        installing[k].id = id;
                        snprintf(installing[k].what, sizeof installing[k].what, "%s %s", name, a);
                        break;
                    }
                sb_printf(&now_out, "[[%s: %s]] -> %s; it takes a while: you'll be told when it is done\n\n", name, a,
                          id > 0 ? (!strcmp(name, "install") ? "installing from Flathub" : "uninstalling")
                                 : "couldn't be asked for");
            }
            free(arg);
            continue;
        }
        /* the slower ones, to the child, in order */
        if (!strcmp(name, "sh") || !strcmp(name, "find") || !strcmp(name, "grep") || web || !strcmp(name, "install") ||
            !strcmp(name, "uninstall") || !strcmp(name, "kill")) {
            if (!strcmp(name, "sh") && irreversible(a)) {
                actions_ask_run(a); /* this one waits for their yes */
                sb_printf(&now_out, "[[sh: %.200s]] -> can't be undone, so it waits for their yes on screen; the result "
                                    "comes to you when they answer\n\n", a);
            } else if (nq < QMAX) {
                snprintf(q[nq].kind, sizeof q[nq].kind, "%s", name);
                q[nq++].arg = strdup(a);
                any_web |= web;
                if (!strcmp(name, "sh"))
                    body_feel('s', "ran: %.120s", a);
                else if (!strcmp(name, "fetch"))
                    body_feel('s', "downloaded %.120s", a);
                else if (!strcmp(name, "install"))
                    body_feel('s', "installed %.80s", a);
            }
            free(arg);
            continue;
        }
        sb_printf(&now_out, "[[%s%s%.200s]] ->\n", name, a[0] ? ": " : "", a);
        char x[600], y[600], said[400];
        if (!strcmp(name, "ls")) {
            char look[700];
            expand(a[0] ? a : "~", x, sizeof x);
            snprintf(look, sizeof look, "files %s", x);
            static char res[12000];
            if (!senses_look(look, res, sizeof res))
                snprintf(res, sizeof res, "can't list %s", x);
            sb_add(&now_out, res);
        } else if (!strcmp(name, "cat")) {
            cat_file(a, &now_out);
        } else if (!strcmp(name, "mv") || !strcmp(name, "cp")) {
            two_paths(a, x, y, sizeof x);
            struct stat st;
            if (!y[0])
                sb_add(&now_out, "write it as [[mv: from -> to]]\n");
            else {
                if (stat(y, &st) == 0 && S_ISDIR(st.st_mode)) { /* into a folder: the same name there */
                    const char *base = strrchr(x, '/');
                    size_t l = strlen(y);
                    snprintf(y + l, sizeof y - l, "/%s", base ? base + 1 : x);
                }
                parents(y);
                bool ok = name[0] == 'm' ? move_path(x, y) : copy_path(x, y);
                if (ok) {
                    remember(name[0] == 'm' ? 'm' : 'c', x, y);
                    body_feel('s', "%s %.100s to %.100s", name[0] == 'm' ? "moved" : "copied", x, y);
                }
                sb_printf(&now_out, "%s\n", ok ? "done" : strerror(errno));
            }
        } else if (!strcmp(name, "rm")) {
            expand(a, x, sizeof x);
            struct stat st;
            if (lstat(x, &st) != 0)
                sb_printf(&now_out, "there is no %s\n", x);
            else {
                mkdir(TRASH, 0700);
                const char *base = strrchr(x, '/');
                snprintf(y, sizeof y, "%s/%ld-%s", TRASH, (long)time(NULL), base ? base + 1 : x);
                bool ok = move_path(x, y);
                if (ok) {
                    remember('m', x, y);
                    body_feel('s', "put %.120s in the trash", x);
                }
                sb_printf(&now_out, "%s\n", ok ? "in the trash (undo brings it back)" : strerror(errno));
            }
        } else if (!strcmp(name, "mkdir")) {
            expand(a, x, sizeof x);
            snprintf(y, sizeof y, "%s/", x);
            bool ok = parents(y);
            if (ok)
                remember('d', "", x), body_feel('s', "made the folder %.120s", x);
            sb_add(&now_out, ok ? "done\n" : "couldn't\n");
        } else if (!strcmp(name, "see")) {
            const char *w = a + strspn(a, " ");
            if (!*w || !strncasecmp(w, "screen", 6))
                see_screen(&now_out);
            else {
                expand(a, x, sizeof x);
                see_file(x, &now_out);
            }
        } else if (!strcmp(name, "windows")) {
            int nw = 0, foc = desktop_focused_window();
            for (int i = 0; i < 24; i++) {
                int w = desktop_window_slot(i);
                if (w < 0)
                    break;
                rect r = A.win[w].r;
                sb_printf(&now_out, "%d. \"%s\" at %d,%d %dx%d%s%s\n", i, desktop_window_title(w), r.x, r.y, r.w, r.h,
                          w == foc ? " (focused)" : "", A.win[w].minimized ? " (minimized)" : "");
                nw++;
            }
            if (!nw)
                sb_add(&now_out, "no windows open\n");
        } else if (!strcmp(name, "window")) {
            window_do(a, &now_out);
        } else if (!strcmp(name, "ps") || !strcmp(name, "sys")) {
            static char res[6000];
            if (!senses_look(!strcmp(name, "ps") ? "processes" : "hardware", res, sizeof res))
                snprintf(res, sizeof res, "nothing to tell");
            sb_add(&now_out, res);
            sb_add(&now_out, "\n");
        } else if (!strcmp(name, "clip")) {
            if (a[0]) {
                clipboard_set(a);
                sb_add(&now_out, "on the clipboard\n");
            } else {
                sb_printf(&now_out, "%.4000s\n", clipboard_text());
            }
        } else if (!strcmp(name, "in") || !strcmp(name, "at") || !strcmp(name, "every")) {
            plan_add(name, a, &now_out);
        } else if (!strcmp(name, "agenda")) {
            agenda(&now_out);
        } else if (!strcmp(name, "forget")) {
            if (!plans_loaded)
                plans_load();
            int i = atoi(a) - 1;
            if (i >= 0 && i < nplan) {
                plan[i] = plan[--nplan];
                plans_save();
                sb_add(&now_out, "forgotten\n");
            } else
                sb_add(&now_out, "no such plan\n");
        } else if (!strcmp(name, "wallpaper")) {
            wallpaper_set(a, said, sizeof said);
            sb_printf(&now_out, "%s\n", said);
        }
        sb_add(&now_out, "\n");
        free(arg);
    }
    /* [[undo]] of something it changed with its body */
    char *u = strstr(text, "[[undo]]");
    if (u && from_person && njournal) {
        char said[600];
        if (undo_last(said, sizeof said)) {
            memmove(u, u + 8, strlen(u + 8) + 1);
            sb_printf(&now_out, "[[undo]] -> %s\n\n", said);
            n_tools++;
        }
    }
    if (!n_tools) {
        sb_free(&now_out);
        return false;
    }
    steps++;
    step_pending = true;
    step_began = A.now;
    snprintf(doing, sizeof doing, "%s", nq ? (any_web ? "on the web" : !strcmp(q[0].kind, "sh") ? "working" : q[0].kind)
                                           : "working");
    if (nq)
        working = job_start(child_tools, NULL);
    LOG("agent: step %d, %d tools (%d slow)\n", steps, n_tools, nq);
    return true;
}

/* ---------------------------------------------------------------------
 * Each frame: results back to it when they're ready; plans that are due.
 * ------------------------------------------------------------------- */

void agent_tick(void) {
    for (int k = 0; k < 8; k++) { /* an install or uninstall init finished */
        bool ok;
        char *text = NULL;
        if (installing[k].id && priv_take(installing[k].id, &ok, &text)) {
            char note[1200];
            const char *tail = text ? text : "";
            size_t tl = strlen(tail);
            snprintf(note, sizeof note, "(What your body did: [[%s]] -> %s%s%.600s)", installing[k].what,
                     ok ? "done" : "failed", ok ? "" : ": ", ok ? "" : (tl > 600 ? tail + tl - 600 : tail));
            LOG("agent: %s %s\n", installing[k].what, ok ? "done" : "failed");
            installing[k].id = 0;
            free(text);
            launcher_apps_changed();
            companion_note(note, true);
        }
    }
    emblem_busy_hint = emblem_busy_hint || working != NULL;
    char *res = NULL;
    if (working && job_poll(working, &res)) {
        job_free(working);
        working = NULL;
        sb_add(&now_out, res ? res : "");
        free(res);
        for (int i = 0; i < nq; i++)
            free(q[i].arg);
        nq = 0;
    }
    if (step_pending && !working && !A.companion.pending && !stopped) {
        step_pending = false;
        doing[0] = 0;
        bool last = steps >= MAX_STEPS;
        sbuf note;
        sb_init(&note);
        sb_printf(&note, "(What your body did, step %d; only you see this:\n%.24000s\n%s)", steps,
                  now_out.p ? now_out.p : "",
                  last ? "That was the last step for this request: stop here and tell them where it stands."
                       : "Go on: the next step, with tags; or, if it is done, tell them what came of it in a sentence or "
                         "two (no tags then). Don't stop halfway to ask.");
        sb_free(&now_out);
        /* what it saw (the screen, a picture file) goes with this step's note, in the same request: it was added
         * after the note had been sent, so each picture reached it a step late */
        char *pic = picture_next;
        picture_next = NULL;
        companion_note_picture(note.p, true, pic);
        free(note.p);
    }
    /* its plans, when they're due */
    if (!plans_loaded)
        plans_load();
    time_t now = time(NULL);
    for (int i = 0; i < nplan && !A.companion.pending; i++)
        if (plan[i].when <= now) {
            char note[600];
            snprintf(note, sizeof note, "(It is time for something you planned: %s. Do it now, with your body; tell "
                                        "them only what they would want to know.)", plan[i].task);
            LOG("agent: plan due: %s\n", plan[i].task);
            if (plan[i].every > 0) {
                while (plan[i].when <= now)
                    plan[i].when += plan[i].every;
            } else {
                plan[i] = plan[--nplan];
            }
            plans_save();
            agent_new_request();
            companion_note(note, true);
            break;
        }
}
