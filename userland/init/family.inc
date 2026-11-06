/* ---- the child lock (included by session.inc) ----
 *
 * A computer set up for a child by a grown-up: what is locked, and the grown-up's password, are the system's, not
 * the desktop's (which runs as the person: the child). The password is kept as the account password is (SHA-512
 * crypt, a random salt), wrong guesses waiting longer after the third. Both are kept where the person can neither
 * read nor change nor move them: on the disk (/var/lib/os-data/family, root's, in a folder of root's) when there
 * is one, else beside their state (/etc/os/.family, taken back from them at every start).
 *   family-state                        "none", or the lock: child=1;n=NAMEHEX;b=BIRTH;p=PRONOUNSHEX;m=MIN;f=HHMM;
 *                                       u=HHMM;lt=1;lw=1;ls=1;li=1;la=1;used=MIN;day=YYYY-MM-DD;pw=set
 *   family-check PWHEX                  the grown-up's password: ok or failed (with how long to wait)
 *   family-set OLDHEX:NEWHEX:CFGHEX     the lock (and a new password, if NEWHEX): the old one first, once set;
 *                                       CFGHEX "" with NEWHEX "" takes the lock away
 *   family-used YYYY-MM-DD:MINUTES      how long today the computer has been used (the time limit; below 0 when
 *                                       a grown-up added more time than the limit) */

static int family_fails;
static time_t family_wait_until;

static const char *family_dir(void) {
    static char d[64];
    struct stat st;
    if (stat("/var/lib/os-data", &st) == 0 && st.st_uid == 0 && access("/var/lib/os-data", W_OK) == 0)
        snprintf(d, sizeof d, "/var/lib/os-data/family");
    else
        snprintf(d, sizeof d, "/etc/os/.family");
    mkdir(d, 0700);
    if (lchown(d, 0, 0) == 0)
        chmod(d, 0700);
    return d;
}

static void family_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", family_dir(), name); }

static bool family_read(const char *name, char *out, size_t n) {
    char p[128];
    family_path(p, sizeof p, name);
    FILE *f = fopen(p, "r");
    bool ok = f && fgets(out, (int)n, f);
    if (f)
        fclose(f);
    if (ok)
        out[strcspn(out, "\r\n")] = 0;
    else if (n)
        out[0] = 0;
    return ok;
}

static bool family_write(const char *name, const char *text) {
    char p[128], tmp[140];
    family_path(p, sizeof p, name);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    if (!text) /* none: it goes */
        return unlink(p) == 0 || errno == ENOENT;
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = out >= 0 && write(out, text, strlen(text)) == (ssize_t)strlen(text) && write(out, "\n", 1) == 1;
    if (out >= 0)
        close(out);
    return ok && chown(tmp, 0, 0) == 0 && rename(tmp, p) == 0;
}

static bool family_pw_matches(const char *pw) {
    char h[256];
    if (!family_read("guardian", h, sizeof h) || strncmp(h, "$6$", 3))
        return false; /* (no grown-up password: nothing opens the lock but setting one) */
    const char *c = crypt(pw, h);
    return c && !strcmp(c, h);
}

static bool family_pw_try(const char *pw, char *why, size_t n) {
    time_t now = time(NULL);
    if (now < family_wait_until) {
        snprintf(why, n, "wait %ld", (long)(family_wait_until - now));
        return false;
    }
    if (family_pw_matches(pw)) {
        family_fails = 0;
        return true;
    }
    family_fails++;
    static const int waits[] = {0, 0, 0, 5, 15, 30, 60, 120};
    int w = waits[family_fails < 8 ? family_fails : 7];
    family_wait_until = now + w;
    snprintf(why, n, w ? "wrong, wait %d" : "wrong", w);
    return false;
}

static bool family_pw_write(const char *pw) {
    static const char abc[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char r[16];
    int fd = open("/dev/urandom", O_RDONLY);
    bool got = fd >= 0 && read(fd, r, sizeof r) == (ssize_t)sizeof r;
    if (fd >= 0)
        close(fd);
    if (!got || !pw[0])
        return false;
    char salt[48] = "$6$rounds=200000$";
    size_t l = strlen(salt);
    for (int i = 0; i < 16; i++)
        salt[l++] = abc[r[i] % 64];
    salt[l++] = '$', salt[l] = 0;
    const char *h = crypt(pw, salt);
    return h && !strncmp(h, "$6$", 3) && family_write("guardian", h);
}

/* the lock's words, checked: only the keys it knows, values of the kind each takes (nothing to run, nothing long) */
static bool family_cfg_ok(const char *c) {
    if (strlen(c) > 400)
        return false;
    for (const char *p = c; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr("=;-", *p)))
            return false;
    return strstr(c, "child=1") != NULL;
}

static bool family_errand(int fd, const char *id, const char *cmd, const char *arg) {
    char why[48] = "", a[100] = "", b[100] = "";
    if (!strcmp(cmd, "family-state")) {
        char cfg[420], used[64], h[256], out[600];
        if (!family_read("lock", cfg, sizeof cfg) || !cfg[0]) {
            priv_reply(fd, id, "ok", "none");
            return true;
        }
        family_read("used", used, sizeof used);
        char *colon = strchr(used, ':');
        if (colon)
            *colon = 0;
        snprintf(out, sizeof out, "%s;used=%s;day=%s;pw=%s", cfg, colon ? colon + 1 : "0", colon ? used : "",
                 family_read("guardian", h, sizeof h) ? "set" : "none");
        priv_reply(fd, id, "ok", out);
    } else if (!strcmp(cmd, "family-check")) {
        bool ok = unhex(arg, a, sizeof a) && family_pw_try(a, why, sizeof why);
        priv_reply(fd, id, ok ? "ok" : "failed", ok ? "" : why[0] ? why : "not understood");
    } else if (!strcmp(cmd, "family-set")) {
        const char *c1 = strchr(arg, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        char oldhex[220] = "", newhex[220] = "", cfg[420] = "";
        if (!c1 || !c2) {
            priv_reply(fd, id, "failed", "not understood");
            return true;
        }
        snprintf(oldhex, sizeof oldhex, "%.*s", (int)(c1 - arg), arg);
        snprintf(newhex, sizeof newhex, "%.*s", (int)(c2 - c1 - 1), c1 + 1);
        bool parsed = unhex(oldhex, a, sizeof a) && unhex(newhex, b, sizeof b) && unhex(c2 + 1, cfg, sizeof cfg);
        char h[256];
        bool has_pw = family_read("guardian", h, sizeof h);
        bool allowed = parsed && (!has_pw || family_pw_try(a, why, sizeof why)); /* (the first time: no old one) */
        bool ok = false;
        if (allowed && !cfg[0] && !b[0]) { /* taken away, the password with it */
            ok = family_write("lock", NULL) && family_write("guardian", NULL) && family_write("used", NULL);
            printf("init: the child lock was taken away\n");
        } else if (allowed && family_cfg_ok(cfg) && (has_pw || b[0])) {
            ok = (!b[0] || family_pw_write(b)) && family_write("lock", cfg);
            printf("init: the child lock %s\n", has_pw ? "was changed" : "was set");
        }
        priv_reply(fd, id, ok ? "ok" : "failed", ok ? "" : why[0] ? why : !parsed ? "not understood" : "couldn't be kept");
    } else if (!strcmp(cmd, "family-used")) {
        int mins = 0;
        char day[16] = "";
        if (sscanf(arg, "%10[0-9-]:%d", day, &mins) == 2 && mins >= -1440 && mins < 2000) {
            char t[40];
            snprintf(t, sizeof t, "%s:%d", day, mins);
            family_write("used", t);
            priv_reply(fd, id, "ok", "");
        } else {
            priv_reply(fd, id, "failed", "not understood");
        }
    } else {
        return false;
    }
    memset(a, 0, sizeof a), memset(b, 0, sizeof b);
    return true;
}
