#define _GNU_SOURCE
#include "tray.h"
#include "app.h"
#include "controls.h"
#include "settings.h"
#include "fm/fm.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The system tray. A design places <volume/>, <network/>, <notifications/>
 * and <quicksettings/> wherever it likes (a taskbar corner, a menu bar, a
 * floating pill); the engine lays them out like icons and the shell draws
 * them here, in the element's own colour. Clicking one opens a popover that
 * is drawn in the design's material, fonts and accent, so the tray belongs
 * to the design rather than sitting on top of it. Every <clock> opens a
 * calendar the same way. */

#define D (A.d)

static font *HF(float px) { return fm_host_font(D.font_heading, px); }
static font *BF(float px) { return fm_host_font(D.font_body, px); }

static int lh(font *f) { return font_line_height(f); }

/* ======================================================================
 * Sound: the master level of /dev/mixer (OSS), which scales everything
 * the sound card plays. Mute is ours: the level goes to 0 and comes back.
 * ==================================================================== */

#define MIXER_READ_VOLUME 0x80044d00u  /* _IOR('M', 0, int) */
#define MIXER_WRITE_VOLUME 0xc0044d00u /* _IOWR('M', 0, int) */

static int mixer_fd = -2;
static float vol = 0.7f;
static bool muted, vol_known;

static int mixer(void) {
    if (mixer_fd == -2)
        mixer_fd = open("/dev/mixer", O_RDWR | O_CLOEXEC);
    return mixer_fd;
}

bool sound_available(void) { return mixer() >= 0; }

static void mixer_write(void) {
    if (mixer() < 0)
        return;
    int l = muted ? 0 : (int)lroundf(vol * 100);
    int v = l | l << 8;
    ioctl(mixer(), MIXER_WRITE_VOLUME, &v);
}

static void sound_read(void) {
    if (vol_known)
        return;
    vol_known = true;
    int v = 0;
    if (mixer() >= 0 && ioctl(mixer(), MIXER_READ_VOLUME, &v) == 0 && (v & 0xff) > 0)
        vol = (v & 0xff) / 100.0f;
}

float sound_volume(void) {
    sound_read();
    return vol;
}

bool sound_muted(void) { return muted; }

void sound_set_volume(float v, bool keep) {
    sound_read();
    vol = v < 0 ? 0 : v > 1 ? 1 : v;
    if (vol > 0.005f)
        muted = false;
    mixer_write();
    if (keep) {
        settings_set_int("volume", (int)lroundf(vol * 100));
        settings_set_int("muted", muted);
    }
}

void sound_set_muted(bool m) {
    sound_read();
    muted = m;
    if (!m && vol < 0.02f)
        vol = 0.4f; /* unmuting to silence would feel broken */
    mixer_write();
    settings_set_int("muted", muted);
    settings_set_int("volume", (int)lroundf(vol * 100));
}

void sound_restore(void) {
    vol_known = true;
    vol = settings_get_int("volume", 70) / 100.0f;
    muted = settings_get_int("muted", 0) != 0;
    mixer_write();
}

#define SNDCTL_DSP_SPEED 0xC0045002u
#define SNDCTL_DSP_SETFMT 0xC0045005u
#define SNDCTL_DSP_CHANNELS 0xC0045006u
#define AFMT_S16_LE 0x10

/* Two soft notes, a fifth apart, each fading like a struck bell. */
void sound_test(void) {
    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0)
            waitpid(pid, NULL, 0); /* the grandchild plays; nothing to reap later */
        return;
    }
    if (fork() != 0)
        _exit(0);
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0)
        _exit(1);
    int fmt = AFMT_S16_LE, ch = 1, rate = 48000;
    ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(fd, SNDCTL_DSP_SPEED, &rate);
    enum { N = 48000 * 9 / 10 };
    static int16_t pcm[N];
    for (int i = 0; i < N; i++) {
        float t = i / 48000.0f, s = 0;
        s += 0.32f * sinf(2 * (float)M_PI * 659.25f * t) * expf(-t * 6);
        if (t > 0.16f)
            s += 0.3f * sinf(2 * (float)M_PI * 987.77f * (t - 0.16f)) * expf(-(t - 0.16f) * 5);
        float attack = t < 0.004f ? t / 0.004f : 1;
        pcm[i] = (int16_t)(s * attack * 32000);
    }
    for (size_t off = 0; off < sizeof pcm;) {
        ssize_t n = write(fd, (char *)pcm + off, sizeof pcm - off);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    _exit(0);
}

/* ======================================================================
 * Network: eth0 through the same ioctls ifconfig uses, the gateway from
 * /proc/net/route and the name server from /etc/resolv.conf.
 * ==================================================================== */

#define SIOCGIFFLAGS 0x8913
#define SIOCGIFADDR 0x8915
#define SIOCGIFNETMASK 0x891b
#define SIOCGIFHWADDR 0x8927
#define IFF_RUNNING 0x40

struct tray_ifreq {
    char name[16];
    union {
        struct {
            uint16_t family;
            uint8_t data[14];
        } addr;
        int16_t flags;
        uint8_t pad[24];
    };
};

static struct net_state net;
static uint64_t net_polled;

static bool if_get(int fd, unsigned long req, struct tray_ifreq *r) {
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "eth0");
    return ioctl(fd, req, r) == 0;
}

static void ip_str(const uint8_t *b, char *out, size_t n) { snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]); }

static void net_poll(void) {
    static int fd = -2;
    if (fd == -2)
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct net_state n;
    memset(&n, 0, sizeof n);
    struct tray_ifreq r;
    if (fd >= 0 && if_get(fd, SIOCGIFFLAGS, &r)) {
        n.present = true;
        n.link = (r.flags & IFF_RUNNING) != 0;
        if (if_get(fd, SIOCGIFADDR, &r)) {
            n.has_ip = true;
            ip_str(r.addr.data + 2, n.ip, sizeof n.ip);
        }
        if (if_get(fd, SIOCGIFNETMASK, &r))
            ip_str(r.addr.data + 2, n.mask, sizeof n.mask);
        if (if_get(fd, SIOCGIFHWADDR, &r)) {
            const uint8_t *m = r.addr.data;
            snprintf(n.mac, sizeof n.mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        }
    }
    FILE *f = fopen("/proc/net/route", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char ifn[16];
        unsigned dst, gw;
        if (sscanf(line, "%15s %x %x", ifn, &dst, &gw) == 3 && dst == 0 && gw) {
            uint8_t b[4] = {(uint8_t)gw, (uint8_t)(gw >> 8), (uint8_t)(gw >> 16), (uint8_t)(gw >> 24)};
            ip_str(b, n.gw, sizeof n.gw);
        }
    }
    if (f)
        fclose(f);
    f = fopen("/proc/net/dev", "r");
    while (f && fgets(line, sizeof line, f)) {
        char *c = strstr(line, "eth0:");
        unsigned long rb, rp, tb, tp;
        if (c && sscanf(c + 5, "%lu %lu %*u %*u %*u %*u %*u %*u %lu %lu", &rb, &rp, &tb, &tp) == 4)
            n.rx = rp, n.tx = tp;
    }
    if (f)
        fclose(f);
    f = fopen("/etc/resolv.conf", "r");
    while (f && fgets(line, sizeof line, f))
        if (!n.dns[0] && sscanf(line, "nameserver %47s", n.dns) == 1)
            break;
    if (f)
        fclose(f);

    /* A change worth telling them about: the connection dropping, or coming
     * back after it dropped. Getting the first address at boot is not news. */
    static int was = -1;
    static bool ever_dropped;
    int online = n.link && n.has_ip;
    if (was == 1 && !online) {
        ever_dropped = true;
        notify("You're offline", n.present && !n.link ? "The network cable is out." : "The network lost its address.",
               "network");
    } else if (was == 0 && online && ever_dropped) {
        char body[96];
        snprintf(body, sizeof body, "Connected again, at %s.", n.ip);
        notify("Back online", body, "network");
    }
    was = online;
    net = n;
}

const struct net_state *net_state(void) {
    if (!net_polled) {
        net_polled = A.now ? A.now : 1;
        net_poll();
    }
    return &net;
}

/* ======================================================================
 * Notifications
 * ==================================================================== */

struct note {
    int seq;
    char title[96], body[400], app[32];
    time_t when;
};

#define NHIST 40
static struct note hist[NHIST]; /* newest first */
static int nhist, next_seq = 1, unread;
static bool dnd, dnd_loaded;

struct toast {
    int seq;
    uint64_t born, life, gone_at;
    float shown; /* 0..1, eased in */
};
#define NTOAST 4
static struct toast toasts[NTOAST];
static int ntoast;

static const struct note *note_by_seq(int seq) {
    for (int i = 0; i < nhist; i++)
        if (hist[i].seq == seq)
            return &hist[i];
    return NULL;
}

bool notify_dnd(void) {
    if (!dnd_loaded) {
        dnd_loaded = true;
        dnd = settings_get_int("dnd", 0) != 0;
    }
    return dnd;
}

void notify_set_dnd(bool on) {
    dnd_loaded = true;
    dnd = on;
    settings_set_int("dnd", on);
    if (on)
        for (int i = 0; i < ntoast; i++)
            if (!toasts[i].gone_at)
                toasts[i].gone_at = A.now;
}

int notify_unread(void) { return unread; }

static uint64_t toast_life(const struct note *n) { return 5200 + 38 * (uint64_t)(strlen(n->title) + strlen(n->body)); }

void notify(const char *title, const char *body, const char *app) {
    if (!title || !*title)
        title = "Notification";
    memmove(&hist[1], &hist[0], sizeof hist[0] * (NHIST - 1));
    if (nhist < NHIST)
        nhist++;
    struct note *n = &hist[0];
    memset(n, 0, sizeof *n);
    n->seq = next_seq++;
    snprintf(n->title, sizeof n->title, "%s", title);
    snprintf(n->body, sizeof n->body, "%s", body ? body : "");
    snprintf(n->app, sizeof n->app, "%s", app ? app : "");
    n->when = time(NULL);
    unread++;
    LOG("notify \"%s\" \"%.120s\" (%s)\n", n->title, n->body, n->app);
    /* The companion's words already appear as a subtitle: a toast saying
     * the same thing would only be noise. It is kept in the history. */
    if (notify_dnd() || !strcmp(n->app, "companion"))
        return;
    if (ntoast == NTOAST) {
        memmove(&toasts[0], &toasts[1], sizeof toasts[0] * (NTOAST - 1));
        ntoast--;
    }
    toasts[ntoast++] = (struct toast){n->seq, A.now, toast_life(n), 0, 0};
}

/* Other programs drop a file in the spool: title, body and app, one a line. */
static void spool_poll(void) {
    static bool made;
    if (!made) {
        made = true;
        mkdir("/run", 0755);
        mkdir("/run/os", 0755);
        mkdir(NOTIFY_SPOOL, 0777);
    }
    DIR *d = opendir(NOTIFY_SPOOL);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[300], buf[1024] = "";
        snprintf(path, sizeof path, NOTIFY_SPOOL "/%s", e->d_name);
        FILE *f = fopen(path, "r");
        size_t len = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f)
            fclose(f);
        buf[len] = 0;
        unlink(path);
        char *title = buf, *body = strchr(title, '\n'), *app = NULL;
        if (body) {
            *body++ = 0;
            app = strchr(body, '\n');
            if (app) {
                *app++ = 0;
                app[strcspn(app, "\r\n")] = 0;
            }
        }
        notify(title, body ? body : "", app && *app ? app : NULL);
    }
    if (d)
        closedir(d);
}

static void when_text(time_t t, char *out, size_t n) {
    long ago = (long)(time(NULL) - t);
    if (ago < 60)
        snprintf(out, n, "now");
    else if (ago < 3600)
        snprintf(out, n, "%ld min ago", ago / 60);
    else if (ago < 86400)
        snprintf(out, n, "%ld h ago", ago / 3600);
    else {
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(out, n, "%d %b", &tm);
    }
}

/* The symbol for where a notification came from. */
static void note_symbol(rect box, const char *app, color col) {
    int a = fm_app_index(app);
    if (a >= 0 && a < APP_COUNT)
        icon_symbol(A.frame, box, a, box.w / 13.0f < 1.4f ? 1.4f : box.w / 13.0f, col);
    else if (!strcmp(app, "network"))
        ctl_symbol(A.frame, box, SYM_NETWORK, 1, col);
    else if (!strcmp(app, "sound"))
        ctl_symbol(A.frame, box, SYM_SPEAKER, 1, col);
    else if (!strcmp(app, "display"))
        ctl_symbol(A.frame, box, SYM_DISPLAY, 1, col);
    else if (!strcmp(app, "sense") || !strcmp(app, "senses"))
        ctl_symbol(A.frame, box, SYM_SENSE, 1, col);
    else
        ctl_symbol(A.frame, box, SYM_BELL, 1, col);
}

/* ======================================================================
 * Popovers
 * ==================================================================== */

enum { P_NONE, P_VOLUME, P_NETWORK, P_NOTES, P_CALENDAR, P_QUICK };

static struct {
    int kind;        /* open, or P_NONE */
    int shown_kind;  /* still drawn while fading out */
    rect anchor;
    uint64_t at;     /* opened or closed at */
    rect r;          /* where it was drawn */
    int cal_y, cal_m; /* the month the calendar shows */
    int notes_scroll;
} pop;

static int kind_of(const char *what) {
    if (!strcasecmp(what, "volume") || !strcasecmp(what, "sound")) return P_VOLUME;
    if (!strcasecmp(what, "network")) return P_NETWORK;
    if (!strcasecmp(what, "notifications") || !strcasecmp(what, "notes")) return P_NOTES;
    if (!strcasecmp(what, "calendar") || !strcasecmp(what, "clock")) return P_CALENDAR;
    if (!strcasecmp(what, "quicksettings") || !strcasecmp(what, "quick")) return P_QUICK;
    return P_NONE;
}

void tray_close(void) {
    if (pop.kind) {
        pop.kind = P_NONE;
        pop.at = A.now;
    }
}

void tray_action(const char *what, rect anchor) {
    int k = kind_of(what);
    if (!k)
        return;
    if (pop.kind == k) {
        tray_close();
        return;
    }
    if (A.ui)
        fm_close_popovers(A.ui, NULL); /* one menu at a time */
    pop.kind = pop.shown_kind = k;
    pop.anchor = anchor;
    pop.at = A.now;
    pop.notes_scroll = 0;
    if (k == P_CALENDAR) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        pop.cal_y = tm.tm_year + 1900;
        pop.cal_m = tm.tm_mon;
    }
    if (k == P_NOTES)
        unread = 0; /* seen */
    if (k == P_NETWORK)
        net_poll();
}

static float pop_opacity(void) {
    float t = (float)(A.now - pop.at) / (A.prof.reduce_motion ? 1.0f : 150.0f);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float e = 1 - (1 - t) * (1 - t) * (1 - t);
    return pop.kind ? e : 1 - e;
}

/* Opens away from the nearest screen edge, lined up with the element. */
static rect place(rect a, int w, int h) {
    float cx = a.x + a.w / 2.0f, cy = a.y + a.h / 2.0f;
    float dt = cy, db = A.H - cy, dl = cx, dr = A.W - cx, m = fminf(fminf(dt, db), fminf(dl, dr));
    int gap = 10, x, y;
    if (m == db || m == dt) {
        y = m == db ? a.y - h - gap : a.y + a.h + gap;
        x = cx > A.W / 2.0f ? a.x + a.w - w : a.x;
    } else {
        x = m == dl ? a.x + a.w + gap : a.x - w - gap;
        y = cy > A.H / 2.0f ? a.y + a.h - h : a.y;
    }
    if (x + w > A.W - 8) x = A.W - 8 - w;
    if (y + h > A.H - 8) y = A.H - 8 - h;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    return (rect){x, y, w, h};
}

static float pop_radius(void) {
    float r = D.radius + 8;
    return r < 10 ? 10 : r > 24 ? 24 : r;
}

/* A label on the left and a value on the right, for the details lists. */
static int detail_row(int x, int y, int w, const char *label, const char *value, const palette *p) {
    font *f = BF(14);
    draw_text(A.frame, f, x, y, label, color_alpha(p->muted, p->op));
    int vw = text_width(f, value);
    int vx = x + w - vw;
    if (vx < x + 130)
        vx = x + 130;
    ctl_text_fit(A.frame, f, vx, y, x + w - vx, value, color_alpha(p->ink, p->op));
    return lh(f) + 10;
}

static void hairline(int x, int y, int w, const palette *p) { fill_rect(A.frame, (rect){x, y, w, 1}, color_alpha(p->ink, 0.1f * p->op)); }

/* ---- volume ---- */

static int volume_h(void) { return 196; }

static void volume_draw(rect r, const palette *p) {
    int pad = 20, x = r.x + pad, w = r.w - 2 * pad, y = r.y + 18;
    font *hf = HF(17), *f = BF(14);
    draw_text(A.frame, hf, x, y, "Sound", color_alpha(p->ink, p->op));
    char pct[32];
    if (!sound_available())
        snprintf(pct, sizeof pct, "Unavailable");
    else if (sound_muted())
        snprintf(pct, sizeof pct, "Muted");
    else
        snprintf(pct, sizeof pct, "%d%%", (int)lroundf(sound_volume() * 100));
    draw_text(A.frame, f, x + w - text_width(f, pct), y + 3, pct, color_alpha(p->muted, p->op));
    y += 46;
    rect mb = {x, y, 40, 40};
    bool on = sound_available();
    if (ctl_tile(mb, sound_muted(), p) && on)
        sound_set_muted(!sound_muted());
    ctl_symbol(A.frame, rect_inset(mb, 9), sound_muted() ? SYM_SPEAKER_MUTED : SYM_SPEAKER,
               sound_muted() ? 0 : sound_volume(), color_alpha(sound_muted() ? p->on_accent : p->ink, p->op));
    float v = sound_muted() ? 0 : sound_volume();
    rect sl = {x + 54, y + 9, w - 54, 22};
    int res = ctl_slider(1001, sl, &v, p);
    if (!on)
        res = CTL_IDLE;
    if (res == CTL_CHANGING)
        sound_set_volume(v, false);
    else if (res == CTL_RELEASED) {
        sound_set_volume(v, true);
        sound_test(); /* hear where it landed */
    }
    y += 56;
    hairline(x, y, w, p);
    y += 14;
    draw_text(A.frame, f, x, y, "Output", color_alpha(p->muted, p->op));
    const char *dev = on ? "Built-in audio" : "No sound card found";
    draw_text(A.frame, f, x + w - text_width(f, dev), y, dev, color_alpha(p->ink, p->op));
    y += lh(f) + 10;
    rect more = {x + w - 120, y, 120, 28};
    font *sf = BF(13);
    bool hov = ui_hover(more);
    draw_text(A.frame, sf, more.x + more.w - text_width(sf, "Sound settings"), more.y + 4, "Sound settings",
              color_alpha(hov ? p->accent : p->muted, p->op));
    if (hov && A.in.clicked) {
        tray_close();
        settings_open_page("Sound");
    }
}

/* ---- network ---- */

static int network_h(void) { return 300; }

static void network_draw(rect r, const palette *p) {
    const struct net_state *n = net_state();
    int pad = 20, x = r.x + pad, w = r.w - 2 * pad, y = r.y + 18;
    font *hf = HF(17), *f = BF(14), *big = HF(22);
    draw_text(A.frame, hf, x, y, "Network", color_alpha(p->ink, p->op));
    bool online = n->link && n->has_ip;
    const char *status = !n->present ? "No network card" : !n->link ? "No cable" : !n->has_ip ? "Connecting" : "Connected";
    font *sf = BF(12);
    int sw = text_width(sf, status) + 20;
    rect pill = {x + w - sw, y, sw, 24};
    fill_round(A.frame, pill, 12, color_alpha(online ? p->accent : color_alpha(p->ink, 0.12f), p->op));
    draw_text_centered(A.frame, sf, pill, status, color_alpha(online ? p->on_accent : p->ink, p->op));
    y += 44;
    rect ic = {x, y, 44, 44};
    fill_round(A.frame, ic, 14, color_alpha(p->ink, 0.08f * p->op));
    ctl_symbol(A.frame, rect_inset(ic, 10), online ? SYM_NETWORK : SYM_NETWORK_OFF, 1, color_alpha(p->ink, p->op));
    draw_text(A.frame, f, x + 58, y + 1, "Wired  ·  eth0", color_alpha(p->muted, p->op));
    draw_text(A.frame, big, x + 58, y + 18, n->has_ip ? n->ip : online ? "" : "Offline", color_alpha(p->ink, p->op));
    y += 62;
    hairline(x, y, w, p);
    y += 14;
    char traffic[64];
    snprintf(traffic, sizeof traffic, "%lu in  ·  %lu out", n->rx, n->tx);
    y += detail_row(x, y, w, "Gateway", n->gw[0] ? n->gw : "None", p);
    y += detail_row(x, y, w, "Subnet mask", n->mask[0] ? n->mask : "None", p);
    y += detail_row(x, y, w, "Name server", n->dns[0] ? n->dns : n->gw[0] ? n->gw : "None", p);
    y += detail_row(x, y, w, "Hardware", n->mac[0] ? n->mac : "None", p);
    detail_row(x, y, w, "Packets", traffic, p);
}

/* ---- calendar ---- */

static int calendar_h(void) { return 56 + 28 + 6 * 38 + 70; }

static int days_in(int y, int m) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : d[m];
}

static void calendar_draw(rect r, const palette *p, bool interactive) {
    int pad = 18, x = r.x + pad, w = r.w - 2 * pad, y = r.y + 16;
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    struct tm first = {0};
    first.tm_year = pop.cal_y - 1900;
    first.tm_mon = pop.cal_m;
    first.tm_mday = 1;
    first.tm_hour = 12;
    mktime(&first);
    char title[64];
    strftime(title, sizeof title, "%B %Y", &first);
    font *hf = HF(19), *f = BF(14), *sf = BF(12);
    draw_text(A.frame, hf, x + 4, y + 2, title, color_alpha(p->ink, p->op));
    bool this_month = pop.cal_y == today.tm_year + 1900 && pop.cal_m == today.tm_mon;
    rect prev = {x + w - 68, y, 32, 32}, next = {x + w - 32, y, 32, 32};
    int step = 0;
    for (int i = 0; i < 2; i++) {
        rect b = i ? next : prev;
        bool h = interactive && ui_hover(b);
        if (h)
            fill_round(A.frame, b, 16, color_alpha(p->ink, 0.1f * p->op));
        ctl_symbol(A.frame, rect_inset(b, 8), i ? SYM_CHEVRON_RIGHT : SYM_CHEVRON_LEFT, 1, color_alpha(p->ink, p->op));
        if (h && A.in.clicked)
            step = i ? 1 : -1;
    }
    if (!this_month) {
        int tw = text_width(sf, "Today") + 20;
        rect tb = {prev.x - tw - 8, y + 4, tw, 24};
        bool h = interactive && ui_hover(tb);
        fill_round(A.frame, tb, 12, color_alpha(h ? p->accent : color_alpha(p->ink, 0.1f), p->op));
        draw_text_centered(A.frame, sf, tb, "Today", color_alpha(h ? p->on_accent : p->ink, p->op));
        if (h && A.in.clicked) {
            pop.cal_y = today.tm_year + 1900;
            pop.cal_m = today.tm_mon;
        }
    }
    if (interactive && ui_hover(r) && A.in.wheel)
        step = A.in.wheel > 0 ? -1 : 1;
    if (step) {
        pop.cal_m += step;
        if (pop.cal_m < 0) pop.cal_m = 11, pop.cal_y--;
        if (pop.cal_m > 11) pop.cal_m = 0, pop.cal_y++;
    }
    y += 50;
    /* Weeks start on Monday (ISO 8601). */
    static const char *const wd[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    int cw = w / 7;
    for (int i = 0; i < 7; i++)
        draw_text_centered(A.frame, sf, (rect){x + i * cw, y, cw, 20}, wd[i], color_alpha(p->muted, p->op));
    y += 28;
    int lead = (first.tm_wday + 6) % 7;
    int dim = days_in(pop.cal_y, pop.cal_m), pdim = days_in(pop.cal_m ? pop.cal_y : pop.cal_y - 1, (pop.cal_m + 11) % 12);
    for (int cell = 0; cell < 42; cell++) {
        int row = cell / 7, col = cell % 7, day = cell - lead + 1;
        bool inside = day >= 1 && day <= dim;
        int shown = inside ? day : day < 1 ? pdim + day : day - dim;
        rect c = {x + col * cw, y + row * 38, cw, 36};
        char num[4];
        snprintf(num, sizeof num, "%d", shown);
        bool is_today = inside && this_month && day == today.tm_mday;
        float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f;
        if (is_today) {
            glow_circle(A.frame, cx, cy, 26, color_alpha(p->accent, 0.25f * p->op));
            fill_circle(A.frame, cx, cy, 16, color_alpha(p->accent, p->op));
        } else if (interactive && inside && ui_hover(c)) {
            fill_circle(A.frame, cx, cy, 16, color_alpha(p->ink, 0.08f * p->op));
        }
        color ink = is_today ? p->on_accent : inside ? (col >= 5 ? color_mix(p->ink, p->muted, 0.45f) : p->ink) : p->muted;
        draw_text_centered(A.frame, f, c, num, color_alpha(ink, (inside ? 1 : 0.45f) * p->op));
    }
    y += 6 * 38 + 10;
    hairline(x, y, w, p);
    y += 14;
    char date[64], clock[16];
    strftime(date, sizeof date, "%A, %d %B", &today);
    strftime(clock, sizeof clock, "%H:%M:%S", &today);
    draw_text(A.frame, BF(15), x + 4, y, date, color_alpha(p->ink, p->op));
    font *tf = BF(15);
    draw_text(A.frame, tf, x + w - 4 - text_width(tf, clock), y, clock, color_alpha(p->muted, p->op));
}

/* ---- notification centre ---- */

static int card_h(const struct note *n, int w) {
    int th = lh(HF(15));
    int bh = n->body[0] ? draw_text_wrapped(NULL, BF(14), (rect){0, 0, w - 66, 3 * lh(BF(14)) * 2}, n->body, 0, 1.25f) : 0;
    int maxb = (int)(3 * lh(BF(14)) * 1.25f) + 2;
    return 24 + th + (bh > maxb ? maxb : bh) + (bh ? 4 : 0);
}

static int notes_h(int w) {
    int h = 18 + 34 + 46 + 10;
    if (!nhist)
        return h + 130;
    int list = 0;
    for (int i = 0; i < nhist; i++)
        list += card_h(&hist[i], w - 24) + 8;
    int maxl = A.H * 6 / 10;
    return h + (list > maxl ? maxl : list) + 12;
}

static void notes_draw(rect r, const palette *p) {
    int pad = 20, x = r.x + pad, w = r.w - 2 * pad, y = r.y + 18;
    font *hf = HF(17), *f = BF(14), *sf = BF(12);
    draw_text(A.frame, hf, x, y, "Notifications", color_alpha(p->ink, p->op));
    if (nhist) {
        int cw = text_width(sf, "Clear all") + 20;
        rect cb = {x + w - cw, y - 1, cw, 26};
        if (ctl_button(cb, "Clear all", sf, false, true, p)) {
            nhist = 0;
            ntoast = 0;
            unread = 0;
        }
    }
    y += 40;
    rect row = {x, y, w, 36};
    fill_round(A.frame, row, 12, color_alpha(p->ink, 0.06f * p->op));
    ctl_symbol(A.frame, (rect){x + 10, y + 8, 20, 20}, notify_dnd() ? SYM_BELL_OFF : SYM_BELL, 1, color_alpha(p->ink, p->op));
    draw_text(A.frame, f, x + 40, y + (36 - lh(f)) / 2, "Do not disturb", color_alpha(p->ink, p->op));
    if (ctl_switch((rect){x + w - 54, y + 6, 44, 24}, notify_dnd(), p) || (ui_hover(row) && A.in.clicked && !ui_hover((rect){x + w - 54, y + 6, 44, 24})))
        notify_set_dnd(!notify_dnd());
    y += 50;
    if (!nhist) {
        rect em = {x, y + 6, w, 110};
        ctl_symbol(A.frame, (rect){em.x + w / 2 - 18, em.y + 10, 36, 36}, SYM_BELL, 1, color_alpha(p->muted, 0.8f * p->op));
        draw_text_centered(A.frame, f, (rect){em.x, em.y + 56, w, 24}, "You're all caught up.", color_alpha(p->muted, p->op));
        return;
    }
    rect list = {r.x + 12, y, r.w - 24, r.y + r.h - 12 - y};
    int total = 0;
    for (int i = 0; i < nhist; i++)
        total += card_h(&hist[i], list.w) + 8;
    if (ui_hover(list) && A.in.wheel)
        pop.notes_scroll -= A.in.wheel * 60;
    if (pop.notes_scroll > total - list.h)
        pop.notes_scroll = total - list.h;
    if (pop.notes_scroll < 0)
        pop.notes_scroll = 0;
    int ox0 = A.frame->cx0, oy0 = A.frame->cy0, ox1 = A.frame->cx1, oy1 = A.frame->cy1;
    canvas_clip(A.frame, list);
    int cy = list.y - pop.notes_scroll, remove = -1;
    for (int i = 0; i < nhist; i++) {
        const struct note *n = &hist[i];
        int h = card_h(n, list.w);
        rect c = {list.x, cy, list.w, h};
        cy += h + 8;
        if (c.y > list.y + list.h || c.y + c.h < list.y)
            continue;
        bool hov = ui_hover(c) && ui_hover(list);
        fill_round(A.frame, c, 14, color_alpha(p->ink, (hov ? 0.1f : 0.06f) * p->op));
        rect ic = {c.x + 12, c.y + 12, 32, 32};
        fill_round(A.frame, ic, 10, color_alpha(p->accent, 0.18f * p->op));
        note_symbol(rect_inset(ic, 7), n->app, color_alpha(p->ink, p->op));
        char when[32];
        when_text(n->when, when, sizeof when);
        int ww = text_width(sf, when);
        font *tf = HF(15);
        ctl_text_fit(A.frame, tf, c.x + 56, c.y + 11, c.w - 56 - ww - 22, n->title, color_alpha(p->ink, p->op));
        if (hov) {
            rect x_r = {c.x + c.w - 30, c.y + 8, 22, 22};
            bool xh = ui_hover(x_r);
            fill_circle(A.frame, x_r.x + 11.0f, x_r.y + 11.0f, 11, color_alpha(p->ink, (xh ? 0.18f : 0.08f) * p->op));
            ctl_symbol(A.frame, rect_inset(x_r, 5), SYM_CLOSE, 1, color_alpha(p->ink, p->op));
            if (xh && A.in.clicked)
                remove = i;
        } else {
            draw_text(A.frame, sf, c.x + c.w - 12 - ww, c.y + 13, when, color_alpha(p->muted, p->op));
        }
        if (n->body[0]) {
            int maxb = (int)(3 * lh(BF(14)) * 1.25f) + 2;
            int ox = A.frame->cy1;
            if (A.frame->cy1 > c.y + 14 + lh(tf) + maxb)
                A.frame->cy1 = c.y + 14 + lh(tf) + maxb;
            draw_text_wrapped(A.frame, f, (rect){c.x + 56, c.y + 14 + lh(tf), c.w - 66, maxb}, n->body,
                              color_alpha(color_mix(p->ink, p->muted, 0.3f), p->op), 1.25f);
            A.frame->cy1 = ox;
        }
        if (hov && A.in.clicked && remove < 0) {
            int a = fm_app_index(n->app);
            if (a >= 0 && A.ui) {
                char act[64];
                snprintf(act, sizeof act, "open:%s", n->app);
                tray_close();
                fm_host_action(A.ui, NULL, act);
            }
        }
    }
    A.frame->cx0 = ox0, A.frame->cy0 = oy0, A.frame->cx1 = ox1, A.frame->cy1 = oy1;
    if (remove >= 0) {
        memmove(&hist[remove], &hist[remove + 1], sizeof hist[0] * (size_t)(nhist - remove - 1));
        nhist--;
    }
}

/* ---- quick settings ---- */

static int quick_h(void) { return 18 + 40 + 2 * 76 + 12 + 60 + 54; }

static void quick_tile(rect t, int sym, float level, const char *label, const char *state, bool on, const palette *p,
                       bool *clicked) {
    *clicked = ctl_tile(t, on, p);
    color ink = on ? p->on_accent : p->ink;
    ctl_symbol(A.frame, (rect){t.x + 14, t.y + 14, 22, 22}, sym, level, color_alpha(ink, p->op));
    font *f = BF(14), *sf = BF(12);
    ctl_text_fit(A.frame, f, t.x + 14, t.y + t.h - 12 - lh(f) - lh(sf), t.w - 24, label, color_alpha(ink, p->op));
    ctl_text_fit(A.frame, sf, t.x + 14, t.y + t.h - 10 - lh(sf), t.w - 24, state,
                 color_alpha(on ? color_alpha(p->on_accent, 0.8f) : p->muted, p->op));
}

static void quick_draw(rect r, const palette *p) {
    int pad = 20, x = r.x + pad, w = r.w - 2 * pad, y = r.y + 18;
    draw_text(A.frame, HF(17), x, y, "Quick settings", color_alpha(p->ink, p->op));
    y += 40;
    int tw = (w - 12) / 2, th = 68;
    bool c;
    bool dark = appearance_dark();
    quick_tile((rect){x, y, tw, th}, dark ? SYM_MOON : SYM_SUN, 1, "Dark mode", appearance_can_switch() ? (dark ? "On" : "Off") : "Set by the design",
               dark, p, &c);
    if (c && appearance_can_switch())
        appearance_set_dark(!dark);
    quick_tile((rect){x + tw + 12, y, tw, th}, notify_dnd() ? SYM_BELL_OFF : SYM_BELL, 1, "Do not disturb",
               notify_dnd() ? "On" : "Off", notify_dnd(), p, &c);
    if (c)
        notify_set_dnd(!notify_dnd());
    y += th + 8;
    const struct net_state *n = net_state();
    bool online = n->link && n->has_ip;
    quick_tile((rect){x, y, tw, th}, online ? SYM_NETWORK : SYM_NETWORK_OFF, 1, "Network",
               online ? n->ip : n->link ? "Connecting" : "Offline", online, p, &c);
    if (c) {
        rect a = pop.anchor;
        tray_action("network", a);
    }
    char nn[32];
    snprintf(nn, sizeof nn, unread ? "%d new" : "None new", unread);
    quick_tile((rect){x + tw + 12, y, tw, th}, SYM_BELL, 1, "Notifications", nn, false, p, &c);
    if (c) {
        rect a = pop.anchor;
        tray_action("notifications", a);
        return;
    }
    y += th + 20;
    rect mb = {x, y, 36, 36};
    if (ctl_tile(mb, sound_muted(), p) && sound_available())
        sound_set_muted(!sound_muted());
    ctl_symbol(A.frame, rect_inset(mb, 8), sound_muted() ? SYM_SPEAKER_MUTED : SYM_SPEAKER, sound_volume(),
               color_alpha(sound_muted() ? p->on_accent : p->ink, p->op));
    float v = sound_muted() ? 0 : sound_volume();
    int res = ctl_slider(1002, (rect){x + 50, y + 7, w - 50, 22}, &v, p);
    if (res == CTL_CHANGING)
        sound_set_volume(v, false);
    else if (res == CTL_RELEASED)
        sound_set_volume(v, true);
    y += 54;
    font *bf = BF(14);
    if (ctl_button((rect){x, y, w, 36}, "All settings", bf, false, true, p)) {
        tray_close();
        settings_open_page(NULL);
    }
}

/* ---- drawing and input ---- */

static int pop_width(int k) {
    switch (k) {
    case P_VOLUME: return 320;
    case P_NETWORK: return 340;
    case P_NOTES: return 380;
    case P_CALENDAR: return 324;
    default: return 344;
    }
}

static int pop_height(int k, int w) {
    switch (k) {
    case P_VOLUME: return volume_h();
    case P_NETWORK: return network_h();
    case P_NOTES: return notes_h(w);
    case P_CALENDAR: return calendar_h();
    default: return quick_h();
    }
}

static rect pop_rect(void) {
    int k = pop.kind ? pop.kind : pop.shown_kind;
    if (!k)
        return (rect){0, 0, 0, 0};
    int w = pop_width(k);
    return place(pop.anchor, w, pop_height(k, w));
}

/* ---- toasts ---- */

static const char *toast_corner(void) {
    static char corner[24];
    snprintf(corner, sizeof corner, "top-right");
    if (!A.ui || !A.ui->desktop)
        return corner;
    fm_node *stack[256];
    int sp = 0;
    stack[sp++] = A.ui->desktop;
    while (sp) {
        fm_node *n = stack[--sp];
        for (; n; n = n->next) {
            if (n->kind != FM_ELEM)
                continue;
            if (!strcasecmp(n->tag, "notifications")) {
                const char *t = fm_attr_get(n, "toasts");
                if (t && *t) {
                    snprintf(corner, sizeof corner, "%s", t);
                } else if (n->laid_out) { /* near the bell */
                    bool top = n->vis.y + n->vis.h / 2 < A.H / 2, left = n->vis.x + n->vis.w / 2 < A.W / 2;
                    snprintf(corner, sizeof corner, "%s-%s", top ? "top" : "bottom", left ? "left" : "right");
                }
                return corner;
            }
            if (n->first && sp < 256)
                stack[sp++] = n->first;
        }
    }
    return corner;
}

static int toast_h(const struct note *n, int w) {
    int bh = n->body[0] ? draw_text_wrapped(NULL, BF(14), (rect){0, 0, w - 74, 400}, n->body, 0, 1.25f) : 0;
    int maxb = (int)(3 * lh(BF(14)) * 1.25f) + 2;
    return 30 + lh(HF(15)) + (bh > maxb ? maxb : bh) + (bh ? 4 : 0);
}

static int toast_layout(rect *out) {
    const char *c = toast_corner();
    bool bottom = strstr(c, "bottom") != NULL, left = strstr(c, "left") != NULL;
    bool centred = !strstr(c, "left") && !strstr(c, "right");
    rect wk = A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H};
    int w = 372 < A.W - 32 ? 372 : A.W - 32, y = bottom ? wk.y + wk.h - 16 : wk.y + 16;
    int n = 0;
    for (int i = ntoast - 1; i >= 0; i--) { /* newest nearest the corner */
        const struct note *nt = note_by_seq(toasts[i].seq);
        if (!nt)
            continue;
        int h = toast_h(nt, w);
        int x = centred ? wk.x + (wk.w - w) / 2 : left ? wk.x + 16 : wk.x + wk.w - 16 - w;
        rect r = {x, bottom ? y - h : y, w, h};
        y = bottom ? y - h - 10 : y + h + 10;
        out[i] = r;
        n++;
    }
    return n;
}

static void toast_draw(int i, rect r, input_state *in) {
    struct toast *t = &toasts[i];
    const struct note *n = note_by_seq(t->seq);
    if (!n)
        return;
    float in_t = (float)(A.now - t->born) / (A.prof.reduce_motion ? 1.0f : 260.0f);
    float e = in_t > 1 ? 1 : in_t < 0 ? 0 : 1 - (1 - in_t) * (1 - in_t) * (1 - in_t);
    float out = t->gone_at ? 1 - (float)(A.now - t->gone_at) / 220.0f : 1;
    if (out < 0)
        out = 0;
    float op = e * out;
    bool left = strstr(toast_corner(), "left") != NULL;
    r.x += (int)((1 - e) * (left ? -28 : 28));
    float rad = pop_radius();
    ctl_panel(r, rad, op);
    palette p = pal_on(pal_sample(rect_inset(r, 8)), op);
    bool hov = pt_in(r, in->mx, in->my) && !t->gone_at;
    rect ic = {r.x + 14, r.y + 14, 36, 36};
    fill_round(A.frame, ic, 11, color_alpha(p.accent, 0.2f * op));
    note_symbol(rect_inset(ic, 8), n->app, color_alpha(p.ink, op));
    font *tf = HF(15), *f = BF(14), *sf = BF(12);
    ctl_text_fit(A.frame, tf, r.x + 64, r.y + 14, r.w - 64 - 44, n->title, color_alpha(p.ink, op));
    if (n->body[0]) {
        int maxb = (int)(3 * lh(f) * 1.25f) + 2;
        int ocy = A.frame->cy1;
        if (A.frame->cy1 > r.y + 16 + lh(tf) + maxb)
            A.frame->cy1 = r.y + 16 + lh(tf) + maxb;
        draw_text_wrapped(A.frame, f, (rect){r.x + 64, r.y + 16 + lh(tf), r.w - 74, maxb}, n->body,
                          color_alpha(color_mix(p.ink, p.muted, 0.3f), op), 1.25f);
        A.frame->cy1 = ocy;
    }
    rect x_r = {r.x + r.w - 32, r.y + 12, 22, 22};
    if (hov) {
        bool xh = pt_in(x_r, in->mx, in->my);
        fill_circle(A.frame, x_r.x + 11.0f, x_r.y + 11.0f, 11, color_alpha(p.ink, (xh ? 0.18f : 0.08f) * op));
        ctl_symbol(A.frame, rect_inset(x_r, 5), SYM_CLOSE, 1, color_alpha(p.ink, op));
        if (in->clicked) {
            t->gone_at = A.now;
            if (!xh && fm_app_index(n->app) >= 0 && A.ui) {
                char act[64];
                snprintf(act, sizeof act, "open:%s", n->app);
                fm_host_action(A.ui, NULL, act);
            }
        }
    } else {
        char when[32];
        when_text(n->when, when, sizeof when);
        draw_text(A.frame, sf, r.x + r.w - 14 - text_width(sf, when), r.y + 16, when, color_alpha(p.muted, op));
    }
    static uint64_t logged;
    if (A.now - logged > 1000) {
        logged = A.now;
        LOGRECT("toast", r);
    }
}

/* ---- the frame ---- */

static input_state tin;
static bool captured;
static rect toast_r[NTOAST];
static int ntoast_r;

void tray_frame_begin(void) {
    static uint64_t net_at, spool_at;
    settings_tick();
    if (A.now - spool_at > 1000) {
        spool_at = A.now;
        spool_poll();
    }
    if (A.now - net_at > 2000) {
        net_at = A.now;
        net_polled = A.now;
        net_poll();
    }
    /* toasts age (not while the pointer rests on them) */
    for (int i = 0; i < ntoast; i++) {
        struct toast *t = &toasts[i];
        bool hov = i < ntoast_r && pt_in(toast_r[i], A.in.mx, A.in.my);
        if (hov && !t->gone_at)
            t->born = A.now - (t->born + 300 < A.now ? 300 : A.now - t->born); /* hold, keep it shown */
        if (!t->gone_at && A.now - t->born > t->life)
            t->gone_at = A.now;
    }
    for (int i = 0; i < ntoast;) {
        if (toasts[i].gone_at && A.now - toasts[i].gone_at > 240) {
            memmove(&toasts[i], &toasts[i + 1], sizeof toasts[0] * (size_t)(ntoast - i - 1));
            ntoast--;
        } else {
            i++;
        }
    }
    for (int i = 0; i < NTOAST; i++)
        toast_r[i] = (rect){-1000, -1000, 0, 0};
    /* with the notification centre open, they are all in it already */
    ntoast_r = pop.kind != P_NOTES && toast_layout(toast_r) ? ntoast : 0;

    tin = A.in;
    captured = false;
    if (pop.kind || (pop.shown_kind && pop_opacity() > 0.01f))
        pop.r = pop_rect();
    bool over = pop.kind && pt_in(pop.r, A.in.mx, A.in.my);
    for (int i = 0; i < ntoast_r; i++)
        over |= pt_in(toast_r[i], A.in.mx, A.in.my);
    /* A press belongs to where it started: a window dragged across a toast
     * stays with the window, a slider in a popover keeps the pointer. */
    static bool press_ours;
    if (A.in.pressed)
        press_ours = over;
    if (A.in.down && !A.in.pressed)
        over = press_ours;
    if (ctl_drag_id() >= 1000 && ctl_drag_id() < 2000 && A.in.down)
        over = true;
    if (over) {
        captured = true;
        A.in.mx = A.in.my = -10000;
        A.in.pressed = A.in.clicked = A.in.right_clicked = false;
        A.in.wheel = 0;
    } else if (pop.kind && A.in.pressed && !pt_in(pop.anchor, A.in.mx, A.in.my)) {
        tray_close(); /* a click anywhere else closes it */
    }
    if (pop.kind)
        for (int i = 0; i < A.in.nkeys; i++)
            if (A.in.keys[i] == K_ESC)
                tray_close();
}

void tray_frame_end(void) {
    input_state saved = A.in;
    A.in = tin;
    if (!captured) {
        A.in.mx = A.in.my = -10000;
        A.in.pressed = A.in.clicked = A.in.right_clicked = false;
        A.in.wheel = 0;
    }
    /* toasts first: an open popover sits above them and takes the pointer */
    input_state ti = tin;
    if (!captured || (pop.kind && pt_in(pop.r, tin.mx, tin.my)))
        ti.mx = ti.my = -10000, ti.clicked = false;
    for (int i = 0; i < ntoast_r; i++)
        toast_draw(i, toast_r[i], &ti);
    int k = pop.kind ? pop.kind : pop.shown_kind;
    float op = pop_opacity();
    if (k && op > 0.01f) {
        rect r = pop.r;
        r.y += (int)((1 - op) * (pop.anchor.y > A.H / 2 ? 8 : -8));
        ctl_panel(r, pop_radius(), op);
        palette p = pal_on(pal_sample(rect_inset(r, 10)), op);
        if (!pop.kind) /* fading out: look, don't touch */
            A.in.mx = A.in.my = -10000, A.in.clicked = A.in.pressed = false;
        switch (k) {
        case P_VOLUME: volume_draw(r, &p); break;
        case P_NETWORK: network_draw(r, &p); break;
        case P_NOTES: notes_draw(r, &p); break;
        case P_CALENDAR: calendar_draw(r, &p, pop.kind != 0); break;
        case P_QUICK: quick_draw(r, &p); break;
        }
        static uint64_t logged;
        static int logged_kind;
        if (pop.kind && (A.now - logged > 1000 || logged_kind != k)) {
            logged = A.now;
            logged_kind = k;
            static const char *const names[] = {"", "volume", "network", "notifications", "calendar", "quicksettings"};
            char nm[48];
            snprintf(nm, sizeof nm, "tray-%s", names[k]);
            LOGRECT(nm, pop.r);
        }
    } else if (!pop.kind) {
        pop.shown_kind = P_NONE;
    }
    A.in = saved;
    /* the pointer itself stays where it is (the cursor is drawn there, and
     * the next frame's movement starts from it); only its clicks were ours */
    A.in.mx = tin.mx;
    A.in.my = tin.my;
}

/* ======================================================================
 * The tray elements, as the Facet engine sees them
 * ==================================================================== */

static int elem_kind(const fm_node *n) {
    if (!strcasecmp(n->tag, "volume")) return P_VOLUME;
    if (!strcasecmp(n->tag, "network")) return P_NETWORK;
    if (!strcasecmp(n->tag, "notifications")) return P_NOTES;
    if (!strcasecmp(n->tag, "quicksettings")) return P_QUICK;
    return P_NONE;
}

/* show="label" (or "both") puts a few words beside the icon. */
static const char *elem_label(const fm_node *n, char *buf, size_t len) {
    const char *show = fm_attr_get(n, "show");
    if (!show || (strcasecmp(show, "label") && strcasecmp(show, "both") && strcasecmp(show, "text")))
        return NULL;
    switch (elem_kind(n)) {
    case P_VOLUME:
        if (sound_muted() || !sound_available())
            snprintf(buf, len, "Muted");
        else
            snprintf(buf, len, "%d%%", (int)lroundf(sound_volume() * 100));
        return buf;
    case P_NETWORK: {
        const struct net_state *s = net_state();
        snprintf(buf, len, "%s", s->link && s->has_ip ? s->ip : s->link ? "Connecting" : "Offline");
        return buf;
    }
    case P_NOTES:
        if (!unread)
            return NULL;
        snprintf(buf, len, "%d new", unread);
        return buf;
    }
    return NULL;
}

float fm_host_tray_extra(const fm_node *n) {
    char buf[48];
    const char *l = elem_label(n, buf, sizeof buf);
    if (!l)
        return 0;
    font *f = fm_host_font(n->st.font, n->st.font_size);
    return (float)text_width(f, l) + 8;
}

void fm_host_draw_tray(fm_node *n, rect r, color col, float op) {
    int k = elem_kind(n);
    n->open = pop.kind && pop.kind == k; /* so :open styles the element whose popover is up */
    if (!k || r.w <= 0 || r.h <= 0)
        return;
    static uint64_t logged[6];
    if (A.now - logged[k] > 1500) { /* for tests: where each kind is */
        logged[k] = A.now;
        char nm[32];
        snprintf(nm, sizeof nm, "tray-el-%s", n->tag);
        LOGRECT(nm, r);
    }
    char buf[48];
    const char *label = elem_label(n, buf, sizeof buf);
    float s = n->vscale > 0 ? n->vscale : 1;
    font *f = label ? fm_host_font(n->st.font, n->st.font_size * s) : NULL;
    int lw = label ? text_width(f, label) + (int)(8 * s) : 0;
    /* the icon is the size the engine measured; padding stays outside it */
    rect inner = {r.x + (int)(n->st.pad[3] * s), r.y + (int)(n->st.pad[0] * s),
                  r.w - (int)((n->st.pad[1] + n->st.pad[3]) * s), r.h - (int)((n->st.pad[0] + n->st.pad[2]) * s)};
    int sz = inner.h < inner.w - lw ? inner.h : inner.w - lw;
    if (sz < 8)
        sz = inner.h;
    int x0 = inner.x + (inner.w - sz - lw) / 2;
    rect box = {x0, inner.y + (inner.h - sz) / 2, sz, sz};
    color c = color_alpha(col, op);
    switch (k) {
    case P_VOLUME:
        ctl_symbol(A.frame, box, sound_muted() || !sound_available() ? SYM_SPEAKER_MUTED : SYM_SPEAKER,
                   sound_volume(), c);
        break;
    case P_NETWORK: {
        const struct net_state *st = net_state();
        ctl_symbol(A.frame, box, st->link && st->has_ip ? SYM_NETWORK : SYM_NETWORK_OFF, 1, c);
        break;
    }
    case P_NOTES:
        ctl_symbol(A.frame, box, notify_dnd() ? SYM_BELL_OFF : SYM_BELL, 1, c);
        if (unread && !label) {
            /* the unread count, in the accent, on the bell's shoulder */
            char num[8];
            snprintf(num, sizeof num, unread > 9 ? "9+" : "%d", unread);
            font *bf = font_get(D.font_body, sz * 0.5f < 9 ? 9 : sz * 0.5f);
            int bw = text_width(bf, num) + (int)(sz * 0.3f), bh = (int)(sz * 0.62f);
            if (bw < bh)
                bw = bh;
            rect b = {box.x + box.w - bw / 2 - 1, box.y - bh / 4, bw, bh};
            fill_round(A.frame, rect_inset(b, -2), (bh + 4) / 2.0f, color_alpha(pal_sample(b), op));
            fill_round(A.frame, b, bh / 2.0f, color_alpha(D.accent, op));
            draw_text_centered(A.frame, bf, b, num, color_alpha(readable_on(D.accent), op));
        }
        break;
    case P_QUICK:
        ctl_symbol(A.frame, box, SYM_SLIDERS, 1, c);
        break;
    }
    if (label)
        draw_text(A.frame, f, box.x + box.w + (int)(8 * s), inner.y + (inner.h - font_line_height(f)) / 2, label, c);
}
