#define _GNU_SOURCE
/* What the companion can perceive.
 *
 * Three sources, each optional, each something the person can switch off:
 *   - the real PC, through the host senses service the VM reaches at
 *     http://10.0.2.2:4571 (temperatures, load, fans, RGB lights). A
 *     background thread polls it, so the screen never waits for the network;
 *     it polls less when nothing changes and backs off when nobody answers.
 *   - this computer, through /proc (load, memory, the busiest programs);
 *   - the shell itself (windows, input, the clock).
 * Nothing here is ever sent anywhere except into the companion's prompt. */
#include "alive.h"
#include "fm/fm.h"
#include "../lib/http.h"
#include "../lib/json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const struct sense_info sense_info[SENSE_COUNT] = {
    [SENSE_HARDWARE] = {"hardware", "Your PC's hardware", "temperatures, load, fans and memory of the real computer"},
    [SENSE_LIGHTS] = {"lights", "Your RGB lights", "which lit devices are on the PC and their colours"},
    [SENSE_SYSTEM] = {"system", "This system", "how busy this computer is and which programs use it most"},
    [SENSE_WINDOWS] = {"windows", "Your screen", "which windows are open, which is in front, what the desktop shows"},
    [SENSE_ACTIVITY] = {"activity", "When you're here", "whether you're typing, away, or have been at it for hours"},
    [SENSE_NETWORK] = {"network", "The network", "whether the computer is online"},
    [SENSE_TIME] = {"time", "The time", "the hour and the date"},
    [SENSE_FILES] = {"files", "Your files", "reading a file or folder, only when you ask"},
};

static bool off[SENSE_COUNT];
bool sense_on(enum sense s) { return s >= 0 && s < SENSE_COUNT && !__atomic_load_n(&off[s], __ATOMIC_RELAXED); }
void sense_set(enum sense s, bool on) {
    if (s >= 0 && s < SENSE_COUNT)
        __atomic_store_n(&off[s], !on, __ATOMIC_RELAXED);
}

static struct senses S;

const struct senses *senses_now(void) { return &S; }

/* ---------------------------------------------------------------------
 * The real PC: a background thread talks to the host senses service.
 * ------------------------------------------------------------------- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static struct host_senses shared; /* written by the thread, under mu */
static bool shared_fresh;
static bool net_up = true, net_known;
static struct {
    char path[32];
    char body[256];
} queue[8];
static int nqueue;
static char base_url[128] = "http://10.0.2.2:4571";

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void copy_str(char *dst, size_t n, const json *j) {
    const char *s = json_str(j, "");
    snprintf(dst, n, "%s", s ? s : "");
}

/* Reads the service's answer into h. Unknowns stay UNKNOWN. */
static bool parse_senses(const char *body, size_t len, struct host_senses *h) {
    json *root = json_parse(body, len);
    if (!root || root->type != JSON_OBJECT) {
        json_free(root);
        return false;
    }
    json *host = json_get(root, "host"), *cpu = json_get(host, "cpu"), *mem = json_get(host, "memory");
    copy_str(h->name, sizeof h->name, json_get(host, "name"));
    copy_str(h->cpu_model, sizeof h->cpu_model, json_get(cpu, "model"));
    h->cpu_load = json_num(json_get(cpu, "load"), UNKNOWN);
    h->cpu_temp = json_num(json_get(cpu, "temp_c"), UNKNOWN);
    h->cpu_mhz = json_num(json_get(cpu, "clock_mhz"), UNKNOWN);
    h->mem_used_mb = json_num(json_get(mem, "used_mb"), UNKNOWN);
    h->mem_total_mb = json_num(json_get(mem, "total_mb"), UNKNOWN);
    json *gpus = json_get(host, "gpu");
    h->ngpu = 0;
    for (json *g = gpus && gpus->type == JSON_ARRAY ? gpus->child : NULL; g && h->ngpu < HOST_MAX_GPUS; g = g->next) {
        typeof(h->gpu[0]) *o = &h->gpu[h->ngpu++];
        copy_str(o->name, sizeof o->name, json_get(g, "name"));
        o->load = json_num(json_get(g, "load"), UNKNOWN);
        o->temp = json_num(json_get(g, "temp_c"), UNKNOWN);
        o->fan_pct = json_num(json_get(g, "fan_pct"), UNKNOWN);
        o->power_w = json_num(json_get(g, "power_w"), UNKNOWN);
        o->vram_used = json_num(json_get(g, "vram_used_mb"), UNKNOWN);
        o->vram_total = json_num(json_get(g, "vram_total_mb"), UNKNOWN);
    }
    json *fans = json_get(host, "fans");
    h->nfan = 0;
    for (json *f = fans && fans->type == JSON_ARRAY ? fans->child : NULL; f && h->nfan < HOST_MAX_FANS; f = f->next) {
        typeof(h->fan[0]) *o = &h->fan[h->nfan++];
        copy_str(o->name, sizeof o->name, json_get(f, "name"));
        o->rpm = json_num(json_get(f, "rpm"), UNKNOWN);
        o->pct = json_num(json_get(f, "pct"), UNKNOWN);
    }
    json *lights = json_get(root, "lights");
    h->nlight = 0;
    for (json *l = lights && lights->type == JSON_ARRAY ? lights->child : NULL; l && h->nlight < HOST_MAX_LIGHTS;
         l = l->next) {
        struct host_light *o = &h->light[h->nlight++];
        json *id = json_get(l, "id");
        if (id && id->type == JSON_NUMBER)
            snprintf(o->id, sizeof o->id, "%d", (int)id->num);
        else
            copy_str(o->id, sizeof o->id, id);
        copy_str(o->name, sizeof o->name, json_get(l, "name"));
        copy_str(o->kind, sizeof o->kind, json_get(l, "kind"));
        copy_str(o->vendor, sizeof o->vendor, json_get(l, "vendor"));
        o->leds = (int)json_num(json_get(l, "leds"), 0);
        o->controllable = json_bool(json_get(l, "controllable"), false);
        const char *c = json_str(json_get(l, "color"), NULL);
        json *cs = json_get(l, "colors");
        if (!c && cs && cs->type == JSON_ARRAY && cs->child)
            c = json_str(cs->child, NULL);
        snprintf(o->color, sizeof o->color, "%s", c && c[0] == '#' && strlen(c) >= 7 ? c : "");
        if (!o->kind[0])
            snprintf(o->kind, sizeof o->kind, "other");
    }
    json *missing = json_get(root, "missing");
    h->nmissing = 0;
    for (json *m = missing && missing->type == JSON_ARRAY ? missing->child : NULL; m && h->nmissing < HOST_MAX_MISSING;
         m = m->next)
        if (json_str(m, NULL))
            copy_str(h->missing[h->nmissing++], sizeof h->missing[0], m);
    json_free(root);
    return true;
}

/* Can this computer reach the internet? A plain TCP handshake, given up
 * after a few seconds; nothing is sent. */
static bool tcp_probe(const char *ip, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    bool ok = false;
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (r == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (poll(&p, 1, timeout_ms) > 0) {
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            ok = err == 0;
        }
    }
    close(fd);
    return ok;
}

static void read_senses_url(void) {
    /* Tests (and unusual setups) can point it elsewhere: "senses_url" in
     * /etc/os/mind.json. */
    FILE *f = fopen(STATE_DIR "/mind.json", "r");
    if (!f)
        return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    json *j = json_parse(buf, n);
    const char *u = json_str(json_get(j, "senses_url"), NULL);
    if (u && !strncmp(u, "http://", 7))
        snprintf(base_url, sizeof base_url, "%s", u);
    json_free(j);
}

static void *poller(void *arg) {
    (void)arg;
    read_senses_url();
    uint64_t next_poll = 0, next_net = mono_ms() + 4000, interval = 3000, backoff = 5000;
    uint32_t last_hash = 0;
    int same = 0, net_fail = 0;
    for (;;) {
        /* Wait for something to send or the next poll, whichever is first. */
        char path[32] = "", body[256] = "";
        pthread_mutex_lock(&mu);
        uint64_t now = mono_ms();
        uint64_t until = next_poll < next_net ? next_poll : next_net;
        if (!nqueue && until > now) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t wait = until - now;
            ts.tv_sec += (time_t)(wait / 1000);
            ts.tv_nsec += (long)(wait % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&cv, &mu, &ts);
        }
        if (nqueue) {
            snprintf(path, sizeof path, "%s", queue[0].path);
            snprintf(body, sizeof body, "%s", queue[0].body);
            memmove(&queue[0], &queue[1], sizeof queue[0] * (size_t)--nqueue);
        }
        pthread_mutex_unlock(&mu);
        now = mono_ms();

        if (path[0]) {
            char url[200];
            snprintf(url, sizeof url, "%s%s", base_url, path);
            const char *hdr[] = {"Content-Type: application/json", NULL};
            struct http_response r;
            int rc = http_request("POST", url, hdr, body, strlen(body), 4, &r);
            char res[160];
            if (rc == 0 && r.status >= 200 && r.status < 300)
                snprintf(res, sizeof res, "done");
            else if (rc == 0)
                snprintf(res, sizeof res, "the PC refused (HTTP %d%s%.80s)", r.status, r.body ? ": " : "",
                         r.body ? r.body : "");
            else
                snprintf(res, sizeof res, "the PC's senses service didn't answer");
            LOG("senses: POST %s %s: %s\n", path, body, res);
            http_response_free(&r);
            pthread_mutex_lock(&mu);
            snprintf(shared.light_result, sizeof shared.light_result, "%s", res);
            shared_fresh = true;
            pthread_mutex_unlock(&mu);
            next_poll = now + 400; /* show the new colours soon */
            continue;
        }

        if (now >= next_net) {
            bool up = tcp_probe("1.1.1.1", 443, 3000) || tcp_probe("8.8.8.8", 443, 3000);
            net_fail = up ? 0 : net_fail + 1;
            /* One lost handshake isn't an outage: two in a row is. */
            bool state = up || net_fail < 2;
            __atomic_store_n(&net_up, state, __ATOMIC_RELAXED);
            __atomic_store_n(&net_known, true, __ATOMIC_RELAXED);
            next_net = mono_ms() + (up ? 30000 : 10000);
            continue;
        }

        if (now >= next_poll) {
            if (!sense_on(SENSE_HARDWARE) && !sense_on(SENSE_LIGHTS)) {
                /* Both switched off: don't even ask. */
                pthread_mutex_lock(&mu);
                shared.up = false;
                snprintf(shared.why_down, sizeof shared.why_down, "they switched these senses off");
                shared_fresh = true;
                pthread_mutex_unlock(&mu);
                next_poll = now + 5000;
                continue;
            }
            char url[200];
            snprintf(url, sizeof url, "%s/senses", base_url);
            struct http_response r;
            int rc = http_request("GET", url, NULL, NULL, 0, 3, &r);
            struct host_senses h;
            memset(&h, 0, sizeof h);
            bool ok = rc == 0 && r.status == 200 && r.body && parse_senses(r.body, r.body_len, &h);
            if (ok) {
                /* A reading that doesn't change needn't be read so often. */
                uint32_t hash = 2166136261u; /* of the readings, not the timestamp */
                for (size_t i = 0; i < sizeof h; i++)
                    hash = (hash ^ ((const uint8_t *)&h)[i]) * 16777619u;
                same = hash == last_hash ? same + 1 : 0;
                last_hash = hash;
                interval = same > 3 ? 8000 : 3000;
                backoff = 5000;
                h.up = true;
                h.seen = now;
            } else {
                h.up = false;
                if (rc != 0)
                    snprintf(h.why_down, sizeof h.why_down, "the senses service on the PC isn't running");
                else
                    snprintf(h.why_down, sizeof h.why_down, "the PC's senses service answered strangely (HTTP %d)",
                             r.status);
                interval = backoff;
                backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
            }
            http_response_free(&r);
            pthread_mutex_lock(&mu);
            char keep[160];
            snprintf(keep, sizeof keep, "%s", shared.light_result);
            if (ok || !shared.seen || now - shared.seen > 15000) {
                /* A single missed poll doesn't make the PC vanish. */
                uint64_t seen = shared.seen;
                shared = h;
                if (!ok)
                    shared.seen = seen;
            }
            snprintf(shared.light_result, sizeof shared.light_result, "%s", keep);
            shared_fresh = true;
            pthread_mutex_unlock(&mu);
            next_poll = mono_ms() + interval;
        }
    }
    return NULL;
}

static void start_poller(void) {
    static bool started;
    if (started)
        return;
    started = true;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&t, &at, poller, NULL) != 0)
        LOG("senses: could not start the poller\n");
    pthread_attr_destroy(&at);
}

void senses_lights_post(const char *path, const char *json_body) {
    start_poller();
    pthread_mutex_lock(&mu);
    if (nqueue == (int)(sizeof queue / sizeof queue[0])) /* the newest wish wins */
        memmove(&queue[0], &queue[1], sizeof queue[0] * (size_t)--nqueue);
    snprintf(queue[nqueue].path, sizeof queue[0].path, "%s", path);
    snprintf(queue[nqueue].body, sizeof queue[0].body, "%s", json_body);
    nqueue++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

void senses_lights_restore_now(void) {
    char url[200];
    snprintf(url, sizeof url, "%s/lights/restore", base_url);
    const char *hdr[] = {"Content-Type: application/json", NULL};
    struct http_response r;
    if (http_request("POST", url, hdr, "{}", 2, 2, &r) == 0)
        LOG("senses: lights handed back to their apps (HTTP %d)\n", r.status);
    http_response_free(&r);
}

/* ---------------------------------------------------------------------
 * This computer, from /proc.
 * ------------------------------------------------------------------- */

static void read_vm_load(void) {
    static unsigned long long prev_busy, prev_total;
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long u = 0, n = 0, s = 0, idle = 0;
        if (fscanf(f, "cpu %llu %llu %llu %llu", &u, &n, &s, &idle) == 4) {
            unsigned long long busy = u + n + s, total = busy + idle;
            if (prev_total && total > prev_total)
                S.vm_cpu = 100.0 * (double)(busy - prev_busy) / (double)(total - prev_total);
            prev_busy = busy, prev_total = total;
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        double total = 0, avail = -1;
        while (fgets(line, sizeof line, f)) {
            unsigned long kb;
            if (sscanf(line, "MemTotal: %lu", &kb) == 1)
                total = kb / 1024.0;
            else if (sscanf(line, "MemAvailable: %lu", &kb) == 1)
                avail = kb / 1024.0;
        }
        fclose(f);
        if (total > 0 && avail >= 0) {
            S.vm_mem_total_mb = total;
            S.vm_mem_used_mb = total - avail;
        }
    }
}

/* Programs and how much they've used, so the busiest can be named and a
 * long job finishing can be noticed. */
#define MAX_TRACK 256
static struct track {
    int pid;
    char name[32];
    unsigned long long ticks, ticks_prev;
    uint64_t first_seen;
    bool seen;
} track[MAX_TRACK];
static int ntrack;

static void read_processes(uint64_t now, double dt_s) {
    for (int i = 0; i < ntrack; i++)
        track[i].seen = false;
    DIR *d = opendir("/proc");
    if (!d)
        return;
    struct dirent *e;
    int count = 0, self = getpid();
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char p[64], buf[512];
        snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = 0;
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (!lp || !rp)
            continue;
        count++;
        int pid = atoi(buf);
        char name[32];
        snprintf(name, sizeof name, "%.*s", (int)(rp - lp - 1) < 31 ? (int)(rp - lp - 1) : 31, lp + 1);
        /* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime ... rss(22) */
        unsigned long long ut = 0, st = 0;
        long rss = 0;
        sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
               &ut, &st, &rss);
        struct track *t = NULL;
        for (int i = 0; i < ntrack; i++)
            if (track[i].pid == pid && !strcmp(track[i].name, name)) {
                t = &track[i];
                break;
            }
        if (!t && ntrack < MAX_TRACK) {
            t = &track[ntrack++];
            memset(t, 0, sizeof *t);
            t->pid = pid;
            snprintf(t->name, sizeof t->name, "%s", name);
            t->first_seen = now;
            t->ticks_prev = ut + st;
        }
        if (!t)
            continue;
        t->ticks_prev = t->ticks ? t->ticks : t->ticks_prev;
        t->ticks = ut + st;
        t->seen = true;
        /* the busiest, by what they used since the last look */
        double cpu = dt_s > 0 && t->ticks >= t->ticks_prev ? (double)(t->ticks - t->ticks_prev) / (dt_s * 100.0) * 100.0 : 0;
        if (pid == self && cpu < 0.5)
            continue;
        int at = S.ntop;
        while (at > 0 && S.top[at - 1].cpu < cpu)
            at--;
        if (at < TOP_PROCS) {
            int last = S.ntop < TOP_PROCS ? S.ntop : TOP_PROCS - 1;
            memmove(&S.top[at + 1], &S.top[at], sizeof S.top[0] * (size_t)(last - at));
            S.top[at].pid = pid;
            snprintf(S.top[at].name, sizeof S.top[at].name, "%s", name);
            S.top[at].cpu = cpu;
            S.top[at].rss_mb = rss * 4096.0 / 1048576.0;
            if (S.ntop < TOP_PROCS)
                S.ntop++;
        }
    }
    closedir(d);
    S.nprocs = count;
    /* Anything gone that had worked hard for a while: a long task finished. */
    for (int i = 0; i < ntrack;) {
        struct track *t = &track[i];
        if (t->seen) {
            i++;
            continue;
        }
        double secs = (double)(now - t->first_seen) / 1000.0, cpu_s = (double)t->ticks / 100.0;
        if (secs >= 60 && cpu_s >= 20 && t->pid != self && sense_on(SENSE_SYSTEM))
            alive_event(2, "task", "a long task finished: %s, after %.0f minutes (it used %.0f s of CPU)", t->name,
                        secs / 60.0 < 1 ? 1 : secs / 60.0, cpu_s);
        *t = track[--ntrack];
    }
}

/* ---------------------------------------------------------------------
 * The shell: windows, input, notifications.
 * ------------------------------------------------------------------- */

static char notes[6][200];
static uint64_t note_at[6];
static int nnotes;

void senses_note_notification(const char *title, const char *body, const char *app) {
    if (nnotes == 6) {
        memmove(&notes[0], &notes[1], sizeof notes[0] * 5);
        memmove(&note_at[0], &note_at[1], sizeof note_at[0] * 5);
        nnotes--;
    }
    snprintf(notes[nnotes], sizeof notes[0], "%s%s%s%s%s%s", app && *app ? app : "", app && *app ? ": " : "",
             title ? title : "", body && *body ? " (" : "", body ? body : "", body && *body ? ")" : "");
    note_at[nnotes++] = A.now;
}

int senses_notifications(char out[][200], int max) {
    int k = 0;
    for (int i = nnotes - 1; i >= 0 && k < max; i--) {
        uint64_t mins = (A.now - note_at[i]) / 60000;
        snprintf(out[k++], 200, "%s (%s)", notes[i], mins < 1 ? "just now" : mins < 60 ? "minutes ago" : "earlier");
    }
    return k;
}

bool senses_typing(void) { return S.last_key && A.now - S.last_key < 6000; }

bool senses_fullscreen(void) {
    int best = -1;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized && (best < 0 || A.win[w].z > A.win[best].z))
            best = w;
    return best >= 0 && A.win[best].maximized;
}

void senses_tick(void) {
    start_poller();
    /* Them: every frame, since input is per frame. */
    bool input = A.in.moved || A.in.clicked || A.in.pressed || A.in.wheel || A.in.nkeys || A.in.nraw;
    if (!S.session_start)
        S.session_start = S.last_input = A.now;
    if (input) {
        if (A.now - S.last_input > 10 * 60000) /* a real break: a new stretch begins */
            S.session_start = A.now;
        S.last_input = A.now;
        if (A.in.nkeys || A.in.nraw)
            S.last_key = A.now;
    }
    static char last_toast[240];
    if (A.toast[0] && A.now < A.toast_until && strcmp(last_toast, A.toast)) {
        snprintf(last_toast, sizeof last_toast, "%s", A.toast);
        senses_note_notification(A.toast, NULL, "system");
    }

    /* The real PC: whatever the thread last heard. */
    pthread_mutex_lock(&mu);
    if (shared_fresh) {
        S.host = shared;
        shared_fresh = false;
    }
    S.net_up = __atomic_load_n(&net_up, __ATOMIC_RELAXED);
    S.net_known = __atomic_load_n(&net_known, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&mu);
    if (S.host.up && A.now - S.host.seen > 20000) {
        S.host.up = false;
        snprintf(S.host.why_down, sizeof S.host.why_down, "the PC's senses service stopped answering");
    }

    /* This computer: every few seconds is plenty. */
    static uint64_t last;
    if (last && A.now - last < 3000)
        return;
    double dt = last ? (double)(A.now - last) / 1000.0 : 0;
    last = A.now;
    read_vm_load();
    S.ntop = 0;
    read_processes(A.now, dt);
}

/* ---------------------------------------------------------------------
 * Putting it into words.
 * ------------------------------------------------------------------- */

static const char *part_of_day(int h) {
    return h < 5 ? "the middle of the night" : h < 12 ? "morning" : h < 17 ? "afternoon" : h < 21 ? "evening" : "late evening";
}

static void dur(char *out, size_t n, uint64_t ms) {
    uint64_t m = ms / 60000;
    if (m < 1)
        snprintf(out, n, "a moment");
    else if (m < 60)
        snprintf(out, n, "%llu min", (unsigned long long)m);
    else
        snprintf(out, n, "%llu h %llu min", (unsigned long long)(m / 60), (unsigned long long)(m % 60));
}

static void gb(char *out, size_t n, double mb) {
    if (mb >= 1024)
        snprintf(out, n, "%.1f GB", mb / 1024.0);
    else
        snprintf(out, n, "%.0f MB", mb);
}

static bool known(double v) { return v > -900; }

/* How the lights look, grouped: "keyboard K70 #ff8800; 3 fans #00aaff". */
static void lights_line(char *out, size_t n, bool detail) {
    size_t o = 0;
    out[0] = 0;
    const struct host_senses *h = &S.host;
    bool done[HOST_MAX_LIGHTS] = {0};
    int ctl = 0;
    for (int i = 0; i < h->nlight; i++) {
        ctl += h->light[i].controllable;
        if (done[i])
            continue;
        if (detail) {
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s (%s%s%s, id %s)%s", o ? "; " : "",
                                  h->light[i].name[0] ? h->light[i].name : h->light[i].kind, h->light[i].kind,
                                  h->light[i].color[0] ? " " : "", h->light[i].color, h->light[i].id,
                                  h->light[i].controllable ? "" : " can't be changed");
            continue;
        }
        /* same kind and colour together */
        int same = 1;
        for (int j = i + 1; j < h->nlight; j++)
            if (!done[j] && !strcmp(h->light[j].kind, h->light[i].kind) && !strcmp(h->light[j].color, h->light[i].color))
                done[j] = true, same++;
        if (same > 1)
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%d %ss %s", o ? "; " : "", same, h->light[i].kind,
                                  h->light[i].color[0] ? h->light[i].color : "(colour not reported)");
        else
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s %s %s", o ? "; " : "", h->light[i].kind,
                                  h->light[i].name, h->light[i].color[0] ? h->light[i].color : "(colour not reported)");
    }
    if (!detail && h->nlight && o < n)
        snprintf(out + o, n - o, ". %s", ctl == h->nlight ? "You can change all of them" : ctl ? "You can change some" : "None can be changed from here");
}

static void hardware_line(char *out, size_t n) {
    const struct host_senses *h = &S.host;
    size_t o = 0;
    char a[32], b[32];
    o += (size_t)snprintf(out + o, n - o, "%s", h->cpu_model[0] ? h->cpu_model : "CPU");
    if (known(h->cpu_load))
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, " at %.0f%% load", h->cpu_load);
    if (known(h->cpu_temp))
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, ", %.0f°C", h->cpu_temp);
    else
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, ", temperature unknown");
    if (known(h->cpu_mhz))
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, ", %.1f GHz", h->cpu_mhz / 1000.0);
    for (int i = 0; i < h->ngpu; i++) {
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "; GPU %s", h->gpu[i].name);
        if (known(h->gpu[i].temp))
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, " %.0f°C", h->gpu[i].temp);
        if (known(h->gpu[i].load))
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, " %.0f%% busy", h->gpu[i].load);
        if (known(h->gpu[i].fan_pct))
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, ", fan %.0f%%", h->gpu[i].fan_pct);
        if (known(h->gpu[i].power_w))
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, ", %.0f W", h->gpu[i].power_w);
    }
    if (known(h->mem_used_mb) && known(h->mem_total_mb)) {
        gb(a, sizeof a, h->mem_used_mb), gb(b, sizeof b, h->mem_total_mb);
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "; memory %s of %s", a, b);
    }
    double lo = 1e9, hi = 0;
    int spinning = 0;
    for (int i = 0; i < h->nfan; i++)
        if (known(h->fan[i].rpm)) {
            lo = h->fan[i].rpm < lo ? h->fan[i].rpm : lo;
            hi = h->fan[i].rpm > hi ? h->fan[i].rpm : hi;
            spinning++;
        }
    if (spinning && hi > lo)
        snprintf(out + o, o < n ? n - o : 0, "; %d fans at %.0f-%.0f rpm", spinning, lo, hi);
    else if (spinning)
        snprintf(out + o, o < n ? n - o : 0, "; %d fan%s at %.0f rpm", spinning, spinning > 1 ? "s" : "", lo);
}

void senses_write(char *out, size_t n) {
    size_t o = 0;
    out[0] = 0;
#define ADD(...) (o += (size_t)snprintf(out + o, o < n ? n - o : 0, __VA_ARGS__))
    char a[64], b[64];
    if (sense_on(SENSE_TIME)) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char when[64];
        strftime(when, sizeof when, "%A %e %B %Y, %H:%M", &tm);
        ADD("- Now: %s (%s).\n", when, part_of_day(tm.tm_hour));
    }
    if (sense_on(SENSE_ACTIVITY)) {
        dur(a, sizeof a, A.now - S.session_start);
        if (A.now - S.last_input > 5 * 60000) {
            dur(b, sizeof b, A.now - S.last_input);
            ADD("- Them: away from the keyboard for %s.\n", b);
        } else {
            ADD("- Them: here%s; at it for %s without a long break.\n", senses_typing() ? " and typing" : "", a);
        }
    }
    if (sense_on(SENSE_WINDOWS)) {
        int nw = fm_host_window_count();
        if (!nw) {
            ADD("- Windows: none open.\n");
        } else {
            ADD("- Windows:");
            for (int i = 0; i < nw && i < 6; i++)
                ADD("%s %s%s", i ? "," : "", fm_host_window_title(i), fm_host_window_focused(i) ? " (in front)" : "");
            ADD("%s.\n", senses_fullscreen() ? "; the front one fills the screen" : "");
        }
    }
    if (sense_on(SENSE_SYSTEM)) {
        gb(a, sizeof a, S.vm_mem_used_mb), gb(b, sizeof b, S.vm_mem_total_mb);
        ADD("- This computer (the one you live in): CPU %.0f%%, memory %s of %s", S.vm_cpu, a, b);
        for (int i = 0; i < S.ntop && i < 3; i++)
            if (S.top[i].cpu >= 1)
                ADD("%s %s %.0f%%", i ? "," : "; busiest:", S.top[i].name, S.top[i].cpu);
        ADD(".\n");
    }
    if (sense_on(SENSE_NETWORK) && S.net_known)
        ADD("- Network: %s.\n", S.net_up ? "online" : "offline (the internet can't be reached)");
    bool hw = sense_on(SENSE_HARDWARE), li = sense_on(SENSE_LIGHTS);
    if ((hw || li) && !S.host.up) {
        ADD("- Their real PC: you can't feel it right now (%s), so you know nothing of its temperatures, fans or "
            "lights.\n",
            S.host.why_down[0] ? S.host.why_down : "not connected yet");
    } else if (hw || li) {
        if (hw) {
            char line[700];
            hardware_line(line, sizeof line);
            ADD("- Their real PC%s%s%s: %s.\n", S.host.name[0] ? " (" : "", S.host.name, S.host.name[0] ? ")" : "", line);
        }
        if (li) {
            char line[900];
            lights_line(line, sizeof line, false);
            ADD("- RGB lights: %s.\n", S.host.nlight ? line : "none found");
            if (S.host.light_result[0])
                ADD("- Your last change to the lights: %s.\n", S.host.light_result);
        }
        for (int i = 0; i < S.host.nmissing; i++)
            ADD("%s%s", i ? "; " : "- Can't sense on the PC: ", S.host.missing[i]);
        if (S.host.nmissing)
            ADD(".\n");
    }
    char offs[200] = "";
    size_t k = 0;
    for (int s = 0; s < SENSE_COUNT; s++)
        if (!sense_on(s))
            k += (size_t)snprintf(offs + k, k < sizeof offs ? sizeof offs - k : 0, "%s%s", k ? ", " : "", sense_info[s].name);
    if (k)
        ADD("- Switched off by them (don't guess these): %s.\n", offs);
#undef ADD
}

static void list_dir(const char *path, char *out, size_t n) {
    DIR *d = opendir(path);
    if (!d) {
        snprintf(out, n, "%s can't be opened.", path);
        return;
    }
    size_t o = (size_t)snprintf(out, n, "In %s:", path);
    struct dirent *e;
    int k = 0;
    while ((e = readdir(d)) && o < n) {
        if (e->d_name[0] == '.')
            continue;
        char full[600];
        snprintf(full, sizeof full, "%s/%s", strcmp(path, "/") ? path : "", e->d_name);
        struct stat st;
        bool dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (k++ < 40)
            o += (size_t)snprintf(out + o, n - o, " %s%s%s", k > 1 ? "," : "", e->d_name, dir ? "/" : "");
    }
    closedir(d);
    if (k > 40 && o < n)
        snprintf(out + o, n - o, " and %d more", k - 40);
    if (!k && o < n)
        snprintf(out + o, n - o, " nothing");
}

static void read_file(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(out, n, "%s can't be opened.", path);
        return;
    }
    char buf[1600];
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    buf[got] = 0;
    if (memchr(buf, 0, got)) {
        snprintf(out, n, "%s isn't text (it's a binary file).", path);
        return;
    }
    snprintf(out, n, "%s%s:\n%s%s", path, more ? " (the beginning)" : "", buf, more ? "\n..." : "");
}

bool senses_look(const char *what, char *out, size_t n) {
    while (*what == ' ')
        what++;
    char word[32];
    size_t wl = strcspn(what, " ");
    snprintf(word, sizeof word, "%.*s", (int)(wl < 31 ? wl : 31), what);
    const char *arg = what + wl;
    while (*arg == ' ')
        arg++;
    char a[64], b[64];
#define NEED(s)                                                                                              \
    if (!sense_on(s)) {                                                                                      \
        snprintf(out, n, "They have switched off \"%s\", so you can't look at that.", sense_info[s].name); \
        return true;                                                                                         \
    }
    if (!strcasecmp(word, "processes") || !strcasecmp(word, "programs") || !strcasecmp(word, "system") ||
        !strcasecmp(word, "computer") || !strcasecmp(word, "memory")) {
        NEED(SENSE_SYSTEM);
        gb(a, sizeof a, S.vm_mem_used_mb), gb(b, sizeof b, S.vm_mem_total_mb);
        size_t o = (size_t)snprintf(out, n, "This computer: CPU %.0f%%, memory %s of %s, %d programs running. Busiest:",
                                    S.vm_cpu, a, b, S.nprocs);
        for (int i = 0; i < S.ntop && o < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s %s (pid %d) %.0f%% CPU, %.0f MB", i ? "," : "", S.top[i].name,
                                  S.top[i].pid, S.top[i].cpu, S.top[i].rss_mb);
        FILE *f = fopen("/proc/uptime", "r");
        double up = 0;
        if (f) {
            if (fscanf(f, "%lf", &up) != 1)
                up = 0;
            fclose(f);
        }
        if (o < n && up > 0) {
            dur(a, sizeof a, (uint64_t)(up * 1000));
            snprintf(out + o, n - o, ". Running for %s.", a);
        }
        return true;
    }
    if (!strcasecmp(word, "hardware") || !strcasecmp(word, "pc") || !strcasecmp(word, "temperature") ||
        !strcasecmp(word, "temperatures") || !strcasecmp(word, "fans") || !strcasecmp(word, "gpu") || !strcasecmp(word, "cpu")) {
        NEED(SENSE_HARDWARE);
        if (!S.host.up) {
            snprintf(out, n, "You can't feel the real PC right now: %s.", S.host.why_down[0] ? S.host.why_down : "not connected");
            return true;
        }
        char line[700];
        hardware_line(line, sizeof line);
        size_t o = (size_t)snprintf(out, n, "%s: %s.", S.host.name[0] ? S.host.name : "Their PC", line);
        for (int i = 0; i < S.host.nfan && o < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s %s %.0f rpm", i ? "," : " Fans:", S.host.fan[i].name,
                                  known(S.host.fan[i].rpm) ? S.host.fan[i].rpm : 0);
        for (int i = 0; i < S.host.nmissing && o < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s%s", i ? "; " : ". Can't sense: ", S.host.missing[i]);
        return true;
    }
    if (!strcasecmp(word, "lights") || !strcasecmp(word, "rgb") || !strcasecmp(word, "devices")) {
        NEED(SENSE_LIGHTS);
        if (!S.host.up) {
            snprintf(out, n, "You can't see the PC's lights right now: %s.", S.host.why_down[0] ? S.host.why_down : "not connected");
            return true;
        }
        char line[1800];
        lights_line(line, sizeof line, true);
        snprintf(out, n, "RGB devices: %s.%s%s", S.host.nlight ? line : "none found",
                 S.host.light_result[0] ? " Last change: " : "", S.host.light_result);
        return true;
    }
    if (!strcasecmp(word, "windows") || !strcasecmp(word, "apps")) {
        NEED(SENSE_WINDOWS);
        int nw = fm_host_window_count();
        size_t o = (size_t)snprintf(out, n, "%d window%s open", nw, nw == 1 ? "" : "s");
        for (int i = 0; i < nw && o < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s %s%s", i ? "," : ":", fm_host_window_title(i),
                                  fm_host_window_focused(i) ? " (in front)" : "");
        return true;
    }
    if (!strcasecmp(word, "screen") || !strcasecmp(word, "desktop")) {
        NEED(SENSE_WINDOWS);
        char *t = A.ui ? fm_describe(A.ui) : NULL;
        snprintf(out, n, "The desktop:\n%.2400s", t ? t : "(not shown)");
        free(t);
        return true;
    }
    if (!strcasecmp(word, "network") || !strcasecmp(word, "internet")) {
        NEED(SENSE_NETWORK);
        snprintf(out, n, "%s", !S.net_known ? "Not checked yet." : S.net_up ? "Online." : "Offline: the internet can't be reached.");
        return true;
    }
    if (!strcasecmp(word, "time") || !strcasecmp(word, "date")) {
        NEED(SENSE_TIME);
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(out, n, "%A %e %B %Y, %H:%M:%S", &tm);
        return true;
    }
    if (!strcasecmp(word, "activity") || !strcasecmp(word, "them")) {
        NEED(SENSE_ACTIVITY);
        dur(a, sizeof a, A.now - S.session_start), dur(b, sizeof b, A.now - S.last_input);
        snprintf(out, n, "At it for %s; last touched the keyboard or mouse %s ago%s.", a, b,
                 senses_typing() ? " (typing now)" : "");
        return true;
    }
    if (!strcasecmp(word, "notifications")) {
        char list[6][200];
        int k = senses_notifications(list, 6);
        size_t o = (size_t)snprintf(out, n, "%s", k ? "Recent notifications:" : "No notifications lately.");
        for (int i = 0; i < k && o < n; i++)
            o += (size_t)snprintf(out + o, n - o, "\n- %s", list[i]);
        return true;
    }
    if (!strcasecmp(word, "files") || !strcasecmp(word, "folder") || !strcasecmp(word, "file")) {
        NEED(SENSE_FILES);
        char path[400];
        snprintf(path, sizeof path, "%s", *arg ? arg : "/root");
        if (path[0] == '~')
            snprintf(path, sizeof path, "/root%s", arg + 1);
        struct stat st;
        if (stat(path, &st) != 0)
            snprintf(out, n, "There's nothing at %s.", path);
        else if (S_ISDIR(st.st_mode))
            list_dir(path, out, n);
        else
            read_file(path, out, n);
        return true;
    }
#undef NEED
    return false;
}

bool senses_bind(const char *key, char *out, size_t n) {
    char a[32], b[32];
    if (!strcmp(key, "cpu_temp")) {
        if (sense_on(SENSE_HARDWARE) && S.host.up && known(S.host.cpu_temp))
            snprintf(out, n, "%.0f°C", S.host.cpu_temp);
        else
            snprintf(out, n, "--");
    } else if (!strcmp(key, "gpu_temp")) {
        if (sense_on(SENSE_HARDWARE) && S.host.up && S.host.ngpu && known(S.host.gpu[0].temp))
            snprintf(out, n, "%.0f°C", S.host.gpu[0].temp);
        else
            snprintf(out, n, "--");
    } else if (!strcmp(key, "cpu_load")) {
        if (sense_on(SENSE_HARDWARE) && S.host.up && known(S.host.cpu_load))
            snprintf(out, n, "%.0f%%", S.host.cpu_load);
        else
            snprintf(out, n, "%.0f%%", S.vm_cpu);
    } else if (!strcmp(key, "memory")) {
        gb(a, sizeof a, S.vm_mem_used_mb), gb(b, sizeof b, S.vm_mem_total_mb);
        snprintf(out, n, "%s of %s", a, b);
    } else if (!strcmp(key, "host")) {
        snprintf(out, n, "%s", S.host.up ? (S.host.name[0] ? S.host.name : "your PC") : "not connected");
    } else if (!strcmp(key, "lights")) {
        snprintf(out, n, "%d", S.host.up ? S.host.nlight : 0);
    } else if (!strcmp(key, "network")) {
        snprintf(out, n, "%s", !S.net_known ? "" : S.net_up ? "online" : "offline");
    } else {
        return false;
    }
    return true;
}
