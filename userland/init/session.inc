/* The person's session (included by init.c): the desktop runs as them, not as root, and the few things that need
 * root are done here, at its request, through a private channel no app can reach.
 *
 * Before the desktop starts, what is theirs becomes theirs: the screen, keyboard, pointer, sound and clipboard
 * devices (the seat), their home (/root), the system state the desktop keeps for them (/etc/os, 0700: their AI
 * keys, the companion's memory, app grants), and their models and recordings on the disk. What was kept on the
 * disk is put back first, as root (the raw disk is never theirs: it would bypass every permission on it).
 *
 * The channel is one end of a socket pair, handed to the desktop as it starts (OS_PRIV_FD); the desktop marks it
 * close-on-exec, so nothing it starts (every app is its descendant, and runs as the same user) ever holds it.
 * A small root process here answers it. It does only these, each checked: save to the disk, restart, power off,
 * install or uninstall an app from Flathub (its ID checked), refresh what Flathub offers, the graphics extension
 * Flatpak apps need, erase the
 * disk store (forget me), and mark a factory reset. Messages are atomic (SOCK_SEQPACKET): "ID COMMAND ARG" in,
 * "ID ok|failed TEXT" out, so requests answered out of order are never mixed up. */

#include "../shell/store.h"

static pid_t priv_helper = -1;

/* home.c (linked in) */
const char *home_dir(void);
const char *account_name(void);
void home_forget(void);

/* "Valerie Liu" for /etc/passwd, kept beside the account's name (/etc/os/account, line 2) */
static void account_full_name(char *out, size_t n) {
    snprintf(out, n, "%s", account_name());
    FILE *f = fopen("/etc/os/account", "r");
    char l[128];
    if (f && fgets(l, sizeof l, f) && fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n:")] = 0;
        if (l[0])
            snprintf(out, n, "%s", l);
    }
    if (f)
        fclose(f);
}

/* /etc/passwd's or /etc/group's line for uid 1000 made theirs */
static void account_line(const char *file, const char *line) {
    char tmp[64], buf[4096] = "", l[512];
    snprintf(tmp, sizeof tmp, "%s.new", file);
    FILE *in = fopen(file, "r");
    size_t len = 0;
    bool put = false;
    while (in && fgets(l, sizeof l, in)) {
        char *c1 = strchr(l, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        bool ours = c2 && !strncmp(c2 + 1, "1000:", 5);
        const char *w = ours ? line : l;
        if (ours)
            put = true;
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%s%s", w, ours ? "\n" : "");
        if (len >= sizeof buf)
            break;
    }
    if (in)
        fclose(in);
    if (!put)
        snprintf(buf + len, sizeof buf - len, "%s\n", line);
    FILE *out = fopen(tmp, "w");
    if (out) {
        fputs(buf, out);
        fclose(out);
        chmod(tmp, 0644);
        rename(tmp, file);
    }
}

/* Their account: named after them (/etc/os/account, written by the desktop once it knows their name), its home
 * /home/<name> (a home under another name, from before, takes the new one), in /etc/passwd and /etc/group
 * (what a terminal and every program see), the computer called after it. */
static void account_setup(void) {
    home_forget();
    const char *name = account_name(), *home = home_dir();
    mkdir("/home", 0755);
    struct stat st;
    if (stat(home, &st) != 0) {
        DIR *d = opendir("/home");
        struct dirent *e;
        char old[300] = "";
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.' && snprintf(old, sizeof old, "/home/%s", e->d_name) > 0)
                break;
        if (d)
            closedir(d);
        if (!old[0] || rename(old, home) != 0)
            mkdir(home, 0700);
        else
            printf("init: their home is now %s (it was %s)\n", home, old);
    }
    char full[96], line[300];
    account_full_name(full, sizeof full);
    snprintf(line, sizeof line, "%s:x:1000:1000:%s:%s:/bin/bash", name, full, home);
    account_line("/etc/passwd", line);
    snprintf(line, sizeof line, "%s:x:1000:", name);
    account_line("/etc/group", line);
    char host[64];
    snprintf(host, sizeof host, "%s-pc", name);
    if (sethostname(host, strlen(host)) == 0) {
        FILE *f = fopen("/etc/hostname", "w");
        if (f) {
            fprintf(f, "%s\n", host);
            fclose(f);
        }
    }
    setenv("HOME", home, 1);
    printf("init: the account is %s (%s), at %s\n", name, full, home);
}

static bool app_id_valid(const char *id) {
    int n = 0, dots = 0;
    for (const char *p = id; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
        dots += c == '.';
    }
    return n > 2 && n < 256 && dots >= 1 && id[0] != '-' && id[0] != '.';
}

static void give(const char *p) {
    if (lchown(p, SESSION_UID, SESSION_UID) < 0) { /* not there: nothing to give */
    }
}

static void give_tree(const char *p, int depth) {
    struct stat st;
    if (depth > 64 || lstat(p, &st) < 0)
        return;
    if (st.st_uid != SESSION_UID || st.st_gid != SESSION_UID)
        give(p);
    if (!S_ISDIR(st.st_mode))
        return;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            char c[1024];
            if (snprintf(c, sizeof c, "%s/%s", p, e->d_name) < (int)sizeof c)
                give_tree(c, depth + 1);
        }
    if (d)
        closedir(d);
}

static void prepare_session(void) {
    /* the seat */
    give("/dev/fb0");
    for (int i = 0; i < 16; i++) {
        char d[32];
        snprintf(d, sizeof d, "/dev/input/event%d", i);
        give(d);
    }
    give("/dev/dsp");
    give("/dev/mixer");
    give("/dev/hostclip");
    /* their state and their home */
    mkdir("/etc/os", 0700);
    give_tree("/etc/os", 0);
    chmod("/etc/os", 0700);
    give_tree(home_dir(), 0);
    chmod(home_dir(), 0700);
    /* their models and recordings (on the disk when there is one): the folders always theirs, what was in them
     * given once (then everything new in them is made by them) */
    mkdir("/var/lib/os-data", 0755);
    mkdir("/var/lib/os-data/models", 0755);
    mkdir("/var/lib/os-data/Videos", 0755);
    give("/var/lib/os-data/models");
    give("/var/lib/os-data/Videos");
    if (access("/var/lib/os-data/.theirs", F_OK) != 0) {
        give_tree("/var/lib/os-data/models", 0);
        give_tree("/var/lib/os-data/Videos", 0);
        int fd = open("/var/lib/os-data/.theirs", O_WRONLY | O_CREAT, 0600);
        if (fd >= 0)
            close(fd);
    }
}

/* ---- the helper ---- */

static void priv_reply(int fd, const char *id, const char *status, const char *text) {
    char m[3600];
    int n = snprintf(m, sizeof m, "%s %s %s", id, status, text ? text : "");
    if (n >= (int)sizeof m)
        n = (int)sizeof m - 1;
    if (send(fd, m, (size_t)n, MSG_NOSIGNAL) < 0) { /* the desktop is gone */
    }
}

/* runs a command as root, its output's last part as the answer */
static void priv_run(int fd, const char *id, char *const argv[]) {
    int p[2];
    if (pipe(p) < 0) {
        priv_reply(fd, id, "failed", "no pipe");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1), dup2(p[1], 2);
        close(p[0]), close(p[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    static char out[64 * 1024];
    size_t len = 0;
    ssize_t r;
    while ((r = read(p[0], out + len % (sizeof out - 1), sizeof out - 1 - len % (sizeof out - 1))) > 0)
        len += (size_t)r;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    size_t keep = len < sizeof out - 1 ? len : sizeof out - 1;
    out[keep] = 0;
    const char *tail = keep > 3000 ? out + keep - 3000 : out;
    priv_reply(fd, id, WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "ok" : "failed", tail);
}

static void priv_serve(int fd) {
    signal(SIGCHLD, SIG_DFL);
    for (;;) {
        while (waitpid(-1, NULL, WNOHANG) > 0) /* finished errands */
            ;
        char buf[512];
        ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0)
            _exit(0); /* the desktop ended: a new one gets a new helper */
        buf[n] = 0;
        char id[16] = "", cmd[24] = "", arg[300] = "";
        if (sscanf(buf, "%15s %23s %299s", id, cmd, arg) < 2) {
            priv_reply(fd, id[0] ? id : "0", "failed", "not understood");
            continue;
        }
        printf("init: the desktop asks: %s %s\n", cmd, arg);
        if (!strcmp(cmd, "restart") || !strcmp(cmd, "power-off")) {
            priv_reply(fd, id, "ok", "");
            sync();
            reboot(!strcmp(cmd, "restart") ? RB_AUTOBOOT : RB_POWER_OFF);
            continue;
        }
        /* everything else may take a while: a process of its own, answering when done */
        if (fork() != 0)
            continue;
        if (!strcmp(cmd, "save")) {
            priv_reply(fd, id, store_save() ? "ok" : "failed", "");
        } else if (!strcmp(cmd, "wipe")) {
            priv_reply(fd, id, store_wipe() ? "ok" : "failed", "");
        } else if (!strcmp(cmd, "factory-reset")) {
            priv_reply(fd, id, store_mark_factory_reset() ? "ok" : "failed", "");
        } else if (!strcmp(cmd, "install") || !strcmp(cmd, "uninstall")) {
            if (!app_id_valid(arg)) {
                priv_reply(fd, id, "failed", "that isn't an app ID");
                _exit(0);
            }
            bool disk = access("/var/lib/os-data/flatpak/repo/config", F_OK) == 0;
            const char *where = disk ? "--installation=os-data" : "--system";
            if (!strcmp(cmd, "install")) {
                /* apps need the graphics extension to open a window: first, if it isn't there yet */
                if (access("/var/lib/os-data/flatpak/runtime/org.freedesktop.Platform.GL.default/x86_64/26.08", F_OK) != 0 &&
                    access("/var/lib/flatpak/runtime/org.freedesktop.Platform.GL.default/x86_64/26.08", F_OK) != 0) {
                    pid_t g = fork();
                    if (g == 0) {
                        int nul = open("/dev/null", O_RDWR);
                        if (nul >= 0)
                            dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
                        execl("/usr/bin/flatpak", "flatpak", "install", where, "-y", "--noninteractive", "flathub",
                              "org.freedesktop.Platform.GL.default//26.08", (char *)NULL);
                        _exit(127);
                    }
                    if (g > 0)
                        waitpid(g, NULL, 0);
                }
                char *argv[] = {"/usr/bin/flatpak", "install", (char *)where, "-y", "--noninteractive", "flathub", arg, NULL};
                priv_run(fd, id, argv);
            } else {
                char *argv[] = {"/usr/bin/flatpak", "uninstall", (char *)where, "-y", "--noninteractive", arg, NULL};
                priv_run(fd, id, argv);
            }
        } else if (!strcmp(cmd, "catalog")) { /* what Flathub offers: its index updated (root's), the list theirs to read */
            mkdir("/var/lib/os", 0755);
            char *argv[] = {"/usr/libexec/os/flathub-catalog", "/var/lib/os/flathub.tsv", NULL};
            priv_run(fd, id, argv);
            chmod("/var/lib/os/flathub.tsv", 0644);
        } else if (!strcmp(cmd, "graphics")) { /* the GL extension apps need to open a window */
            bool disk = access("/var/lib/os-data/flatpak/repo/config", F_OK) == 0;
            char *argv[] = {"/usr/bin/flatpak", "install", disk ? "--installation=os-data" : "--system", "-y",
                            "--noninteractive", "flathub", "org.freedesktop.Platform.GL.default//26.08", NULL};
            priv_run(fd, id, argv);
        } else {
            priv_reply(fd, id, "failed", "the system doesn't do that for the desktop");
        }
        _exit(0);
    }
}

/* What the disk kept comes back once a boot, before anything is shown. */
static void restore_once(void) {
    static bool restored;
    if (restored)
        return;
    restored = true;
    int kept = store_restore();
    char k[16];
    snprintf(k, sizeof k, "%d", kept);
    setenv("OS_STORE_KEPT", k, 1);
    printf("init: store: %s\n", kept > 0 ? "restored from the disk" : kept == 0 ? "a new disk" : "no disk");
}

/* The graphical experience, as the person; its helper, as root. */
static pid_t spawn_gui(void) {
    if (access("/bin/os-shell", X_OK) != 0)
        return -1;
    restore_once();
    account_setup();
    prepare_session();
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0)
        sv[0] = sv[1] = -1;
    if (priv_helper > 0)
        kill(priv_helper, SIGTERM);
    priv_helper = -1;
    if (sv[0] >= 0) {
        priv_helper = fork();
        if (priv_helper == 0) {
            close(sv[1]);
            priv_serve(sv[0]);
            _exit(0);
        }
        close(sv[0]);
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (sv[1] >= 0) {
            char f[16];
            snprintf(f, sizeof f, "%d", sv[1]);
            setenv("OS_PRIV_FD", f, 1);
        }
        setenv("OS_STORE", store_available() ? "1" : "0", 1);
        setenv("HOME", home_dir(), 1);
        setenv("USER", account_name(), 1);
        setenv("LOGNAME", account_name(), 1);
        if (setgroups(0, NULL) < 0 || setgid(SESSION_UID) < 0 || setuid(SESSION_UID) < 0)
            _exit(126); /* never as root */
        char *argv[] = {"/bin/os-shell", NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    if (sv[1] >= 0)
        close(sv[1]);
    return pid;
}
