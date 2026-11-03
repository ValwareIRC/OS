#define _GNU_SOURCE
#include "clock.h"
#include "app.h"
#include "priv.h"
#include "fm/fm.h"
#include "../lib/http.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Date and time: see clock.h. */

#define CONF "/etc/os/time.conf"
#define ZONEINFO "/usr/share/zoneinfo"

static struct {
    bool loaded, auto_zone, auto_time, h12;
    char zone[64];
    /* the automatic time zone, looked up on a thread of its own */
    pthread_t th;
    volatile int looking; /* 1 asking, 2 answered */
    char found[64], status[96];
    uint64_t asked_at;
    int sync_req, sync_result, zone_req;
    char sync_msg[160];
} C;

static bool zone_ok(const char *z) {
    if (!z[0] || strlen(z) >= sizeof C.zone || z[0] == '/' || strstr(z, ".."))
        return false;
    for (const char *p = z; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("/_+-", *p)))
            return false;
    char path[128];
    snprintf(path, sizeof path, ZONEINFO "/%s", z);
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void save_conf(void) {
    FILE *f = fopen(CONF ".new", "w");
    if (!f)
        return;
    fprintf(f, "zone %s\nauto-zone %d\nauto-time %d\nclock %d\n", C.zone, C.auto_zone, C.auto_time, C.h12 ? 12 : 24);
    if (fclose(f) == 0)
        rename(CONF ".new", CONF);
}

static void use_zone(const char *z) {
    if (z != C.zone)
        snprintf(C.zone, sizeof C.zone, "%s", z);
    setenv("TZ", C.zone, 1); /* this desktop and all it starts from now */
    tzset();
}

void clock_init(void) {
    if (C.loaded)
        return;
    C.loaded = true;
    C.auto_zone = C.auto_time = true;
    snprintf(C.zone, sizeof C.zone, "UTC");
    FILE *f = fopen(CONF, "r");
    char line[160];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, ' ');
        if (!v)
            continue;
        *v++ = 0;
        if (!strcmp(line, "zone") && zone_ok(v))
            snprintf(C.zone, sizeof C.zone, "%s", v);
        else if (!strcmp(line, "auto-zone"))
            C.auto_zone = atoi(v) != 0;
        else if (!strcmp(line, "auto-time"))
            C.auto_time = atoi(v) != 0;
        else if (!strcmp(line, "clock"))
            C.h12 = atoi(v) == 12;
    }
    if (f)
        fclose(f);
    use_zone(C.zone);
    fm_clock_12h = C.h12;
    LOG("clock: time zone %s (%s), %s clock, network time %s\n", C.zone, C.auto_zone ? "automatic" : "chosen",
        C.h12 ? "12-hour" : "24-hour", C.auto_time ? "on" : "off");
}

/* /etc/localtime, for everything not started by the desktop (init's) */
static void tell_init(const char *zone) {
    if (!priv_available())
        return;
    char m[96];
    snprintf(m, sizeof m, "time-zone %s", zone);
    C.zone_req = priv_send(m);
}

/* ---- the automatic time zone ---- */

/* Where this computer is, by its internet address: the zone's name from the first service that answers. */
static void *look_up(void *arg) {
    (void)arg;
    static const char *const urls[] = {"https://ipapi.co/timezone/", "http://ip-api.com/line/?fields=timezone",
                                       "https://worldtimeapi.org/api/ip.txt"};
    const char *const headers[] = {"User-Agent: OperatingSystem/" OS_VERSION, NULL};
    C.found[0] = 0;
    for (size_t i = 0; i < sizeof urls / sizeof *urls && !C.found[0]; i++) {
        struct http_response r;
        if (http_request("GET", urls[i], headers, NULL, 0, 8, &r) == 0 && r.status == 200 && r.body) {
            char *b = r.body, *t = strstr(b, "timezone: ");
            if (t)
                b = t + 10;
            b[strcspn(b, "\r\n \t")] = 0;
            if (zone_ok(b))
                snprintf(C.found, sizeof C.found, "%s", b);
        }
        free(r.body);
    }
    __atomic_store_n(&C.looking, 2, __ATOMIC_RELEASE);
    return NULL;
}

static void ask_where(void) {
    if (C.looking)
        return;
    C.looking = 1;
    C.asked_at = A.now;
    if (pthread_create(&C.th, NULL, look_up, NULL) != 0)
        C.looking = 0;
}

void clock_frame(void) {
    clock_init();
    /* the automatic zone: a while after the start (the network first), then every three hours (a laptop travels) */
    if (C.auto_zone && !C.looking && (C.asked_at ? A.now - C.asked_at > 3 * 3600 * 1000ULL : A.now > 15000))
        ask_where();
    if (__atomic_load_n(&C.looking, __ATOMIC_ACQUIRE) == 2) {
        pthread_join(C.th, NULL);
        C.looking = 0;
        if (C.found[0]) {
            snprintf(C.status, sizeof C.status, "Found from your internet address: %s", C.found);
            if (C.auto_zone && strcmp(C.found, C.zone)) {
                LOG("clock: the time zone here is %s (was %s)\n", C.found, C.zone);
                use_zone(C.found);
                save_conf();
                tell_init(C.zone);
            }
        } else {
            snprintf(C.status, sizeof C.status, "Couldn't find it from the internet just now");
            LOG("clock: the automatic time zone found nothing\n");
            C.asked_at = A.now - 3 * 3600 * 1000ULL + 10 * 60 * 1000ULL; /* again in ten minutes */
        }
    }
    if (C.zone_req > 0) { /* its answer taken (an answer left unread would hold a place for good) */
        bool ok;
        char *text = NULL;
        if (priv_take(C.zone_req, &ok, &text)) {
            C.zone_req = 0;
            if (!ok)
                LOG("clock: init couldn't use the time zone: %s\n", text ? text : "");
            free(text);
        }
    }
    if (C.sync_req > 0) {
        bool ok;
        char *text = NULL;
        if (priv_take(C.sync_req, &ok, &text)) {
            C.sync_req = 0;
            C.sync_result = ok ? 1 : -1;
            snprintf(C.sync_msg, sizeof C.sync_msg, "%s", text ? text : "");
            free(text);
        }
    }
}

const char *clock_zone(void) {
    clock_init();
    return C.zone;
}

bool clock_set_zone(const char *zone) {
    clock_init();
    if (!zone_ok(zone))
        return false;
    C.auto_zone = false;
    use_zone(zone);
    save_conf();
    tell_init(zone);
    LOG("clock: time zone chosen: %s\n", zone);
    return true;
}

bool clock_auto_zone(void) { return clock_init(), C.auto_zone; }
void clock_set_auto_zone(bool on) {
    clock_init();
    C.auto_zone = on;
    save_conf();
    if (on)
        ask_where();
}
bool clock_auto_time(void) { return clock_init(), C.auto_time; }
void clock_set_auto_time(bool on) {
    clock_init();
    C.auto_time = on;
    save_conf();
    if (on)
        clock_sync_now();
}
bool clock_12h(void) { return clock_init(), C.h12; }
void clock_set_12h(bool on) {
    clock_init();
    C.h12 = on;
    fm_clock_12h = on;
    save_conf();
}

void clock_sync_now(void) {
    if (!priv_available() || C.sync_req > 0)
        return;
    C.sync_result = 0;
    C.sync_req = priv_send("time-sync");
}

int clock_sync_result(char *msg, size_t n) {
    snprintf(msg, n, "%s", C.sync_msg);
    return C.sync_req > 0 ? 0 : C.sync_result;
}

const char *clock_auto_status(void) { return C.looking ? "Looking it up..." : C.status; }

/* ---- the zones of the world ---- */

static int by_name(const void *a, const void *b) {
    return strcmp(((const struct clock_zone_entry *)a)->name, ((const struct clock_zone_entry *)b)->name);
}

int clock_zones(const struct clock_zone_entry **out) {
    static struct clock_zone_entry *z;
    static int n;
    if (!z) {
        /* country codes to names */
        static char codes[400][3], names[400][48];
        int nc = 0;
        FILE *f = fopen(ZONEINFO "/iso3166.tab", "r");
        char line[512];
        while (f && fgets(line, sizeof line, f) && nc < 400) {
            if (line[0] == '#' || strlen(line) < 4 || line[2] != '\t')
                continue;
            line[strcspn(line, "\r\n")] = 0;
            memcpy(codes[nc], line, 2), codes[nc][2] = 0;
            snprintf(names[nc], sizeof names[nc], "%s", line + 3);
            nc++;
        }
        if (f)
            fclose(f);
        z = calloc(600, sizeof *z);
        f = fopen(ZONEINFO "/zone1970.tab", "r");
        while (z && f && fgets(line, sizeof line, f) && n < 599) {
            if (line[0] == '#')
                continue;
            line[strcspn(line, "\r\n")] = 0;
            char *cc = strtok(line, "\t"), *coord = strtok(NULL, "\t"), *name = strtok(NULL, "\t");
            (void)coord;
            if (!cc || !name)
                continue;
            snprintf(z[n].name, sizeof z[n].name, "%s", name);
            const char *where = cc;
            char first[3] = {cc[0], cc[1], 0};
            for (int i = 0; i < nc; i++)
                if (!strcmp(codes[i], first))
                    where = names[i];
            snprintf(z[n].where, sizeof z[n].where, "%s", where);
            n++;
        }
        if (f)
            fclose(f);
        if (z) {
            snprintf(z[n].name, sizeof z[n].name, "UTC");
            snprintf(z[n].where, sizeof z[n].where, "Coordinated Universal Time");
            n++;
            qsort(z, (size_t)n, sizeof *z, by_name);
        }
    }
    *out = z;
    return n;
}
