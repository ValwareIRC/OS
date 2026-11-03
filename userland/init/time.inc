/* The clock and the time zone (included by session.inc).
 *
 * The time: set from the network (SNTP, RFC 4330) a moment after it comes up, then every six hours, unless they
 * turned it off (Settings > Date & time; /etc/os/time.conf "auto-time 0"). Only root may set the clock, so it is
 * done here; the desktop can ask for it at once ("time-sync"). The real-time clock is left as it is: a computer
 * shared with Windows keeps it in local time, and the network's time is set at every start anyway.
 *
 * The time zone: /etc/os/time.conf "zone Europe/London" (the desktop writes it, theirs), made /etc/localtime here
 * at the start and on the desktop's word ("time-zone NAME"), and handed to the desktop as TZ. */

#define TIME_CONF "/etc/os/time.conf"
#define ZONEINFO "/usr/share/zoneinfo"

/* a time zone's name, as the tz database spells them: Europe/London, America/Argentina/Buenos_Aires, UTC */
static bool zone_name_ok(const char *z) {
    if (!z[0] || strlen(z) > 64 || z[0] == '/' || strstr(z, ".."))
        return false;
    for (const char *p = z; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("/_+-", *p)))
            return false;
    char path[128];
    snprintf(path, sizeof path, ZONEINFO "/%s", z);
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* one setting from time.conf ("key value" lines), or def */
static void time_conf(const char *key, char *out, size_t n, const char *def) {
    snprintf(out, n, "%s", def);
    FILE *f = fopen(TIME_CONF, "r");
    if (!f)
        return;
    char line[160];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ' ') {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(out, n, "%s", line + kl + 1);
            break;
        }
    fclose(f);
}

static void zone_apply(const char *z) {
    char target[128];
    snprintf(target, sizeof target, ZONEINFO "/%s", z);
    unlink("/etc/localtime.new");
    if (symlink(target, "/etc/localtime.new") == 0 && rename("/etc/localtime.new", "/etc/localtime") == 0)
        setenv("TZ", z, 1);
}

/* at the start: their zone (UTC until there is one) */
static void zone_setup(void) {
    char z[80];
    time_conf("zone", z, sizeof z, "UTC");
    if (!zone_name_ok(z))
        snprintf(z, sizeof z, "UTC");
    zone_apply(z);
    printf("init: time zone %s\n", z);
}

/* ---- SNTP ---- */

#define NTP_UNIX 2208988800ULL /* 1900 to 1970, in seconds */

static uint64_t ntp_to_ns(const uint8_t *b) {
    uint64_t s = ((uint64_t)b[0] << 24) | ((uint64_t)b[1] << 16) | ((uint64_t)b[2] << 8) | b[3];
    uint64_t f = ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) | ((uint64_t)b[6] << 8) | b[7];
    return (s - NTP_UNIX) * 1000000000ULL + ((f * 1000000000ULL) >> 32);
}

static uint64_t real_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* Asks one server; true with the clock's error (positive: the clock is behind) in *off_ms. */
static bool sntp_ask(const char *host, int64_t *off_ns, char *why, size_t wn) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *ai = NULL;
    if (getaddrinfo(host, "123", &hints, &ai) != 0 || !ai) {
        snprintf(why, wn, "%s: not found", host);
        return false;
    }
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    bool ok = false;
    if (s >= 0) {
        uint8_t q[48] = {0x23}; /* no warning, version 4, client */
        uint64_t t1 = real_ns();
        /* our send time in the transmit field: the answer must carry it back (origin), so a stray reply is
         * never taken for it */
        uint64_t s1 = t1 / 1000000000ULL + NTP_UNIX, f1 = ((t1 % 1000000000ULL) << 32) / 1000000000ULL;
        for (int i = 0; i < 4; i++)
            q[40 + i] = (uint8_t)(s1 >> (24 - 8 * i)), q[44 + i] = (uint8_t)(f1 >> (24 - 8 * i));
        struct pollfd pf = {s, POLLIN, 0};
        if (sendto(s, q, sizeof q, 0, ai->ai_addr, ai->ai_addrlen) == (ssize_t)sizeof q && poll(&pf, 1, 3000) == 1) {
            uint8_t a[48];
            ssize_t n = recv(s, a, sizeof a, 0);
            uint64_t t4 = real_ns();
            int mode = n >= 48 ? a[0] & 7 : 0, stratum = n >= 48 ? a[1] : 0, li = n >= 48 ? a[0] >> 6 : 3;
            if (n < 48 || mode != 4 || stratum < 1 || stratum > 15 || li == 3 || memcmp(a + 24, q + 40, 8))
                snprintf(why, wn, "%s: not a good answer", host);
            else {
                uint64_t t2 = ntp_to_ns(a + 32), t3 = ntp_to_ns(a + 40);
                *off_ns = (((int64_t)t2 - (int64_t)t1) + ((int64_t)t3 - (int64_t)t4)) / 2;
                ok = true;
            }
        } else
            snprintf(why, wn, "%s: no answer", host);
        close(s);
    }
    freeaddrinfo(ai);
    return ok;
}

/* The network's time, from the first server that answers; the clock set when it is off by more than a little. */
static bool time_sync(char *msg, size_t mn) {
    static const char *const servers[] = {"time.cloudflare.com", "pool.ntp.org", "time.google.com"};
    char why[96] = "";
    for (size_t i = 0; i < sizeof servers / sizeof *servers; i++) {
        int64_t off;
        if (!sntp_ask(servers[i], &off, why, sizeof why))
            continue;
        long ms = (long)(off / 1000000);
        if (off > 20000000LL || off < -20000000LL) {
            struct timespec t;
            uint64_t now = real_ns() + (uint64_t)off;
            t.tv_sec = (time_t)(now / 1000000000ULL), t.tv_nsec = (long)(now % 1000000000ULL);
            if (clock_settime(CLOCK_REALTIME, &t) != 0) {
                snprintf(msg, mn, "couldn't set the clock: %s", strerror(errno));
                return false;
            }
        }
        snprintf(msg, mn, "from %s, the clock was %ld ms %s", servers[i], ms < 0 ? -ms : ms, ms < 0 ? "ahead" : "behind");
        return true;
    }
    snprintf(msg, mn, "no time server answered (%s)", why);
    return false;
}

/* A process of its own: after the network, then every six hours (every few minutes until one answers). */
static pid_t timesync_pid = -1;
static void timesync_start(void) {
    if (timesync_pid > 0 && kill(timesync_pid, 0) == 0)
        return;
    timesync_pid = fork();
    if (timesync_pid != 0)
        return;
    signal(SIGCHLD, SIG_DFL);
    for (int wait_s = 0;;) {
        sleep((unsigned)wait_s);
        char on[8];
        time_conf("auto-time", on, sizeof on, "1");
        if (strcmp(on, "0") && access("/etc/resolv.conf", F_OK) == 0) {
            char msg[160];
            bool ok = time_sync(msg, sizeof msg);
            printf("init: time: %s\n", msg);
            wait_s = ok ? 6 * 3600 : wait_s < 30 ? 30 : wait_s < 600 ? wait_s * 2 : 600;
        } else
            wait_s = 60;
    }
}

/* the desktop's word: a new zone, or the network's time now */
static bool time_errand(int fd, const char *id, const char *cmd, const char *arg) {
    if (!strcmp(cmd, "time-zone")) {
        if (!zone_name_ok(arg)) {
            priv_reply(fd, id, "failed", "no such time zone");
        } else {
            zone_apply(arg);
            printf("init: time zone now %s\n", arg);
            priv_reply(fd, id, "ok", arg);
        }
        return true;
    }
    if (!strcmp(cmd, "time-sync")) {
        if (fork() == 0) { /* the network may be slow: not in the way of the other errands */
            char msg[160];
            bool ok = time_sync(msg, sizeof msg);
            printf("init: time: %s\n", msg);
            priv_reply(fd, id, ok ? "ok" : "failed", msg);
            _exit(0);
        }
        return true;
    }
    return false;
}
