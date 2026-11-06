#define _GNU_SOURCE
#include "family.h"
#include "alive.h"
#include "app.h"
#include "fm/fm.h"
#include "lock.h"
#include "priv.h"
#include "reader.h"
#include "settings.h"
#include "store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

/* The child lock: see family.h. */

#define D (A.d)
#define OPEN_MS (10 * 60 * 1000) /* a grown-up's password keeps it open this long */

static struct {
    bool known, child, pw_set;
    char name[64], pronouns[48], birth[12];
    int limit;         /* minutes a day; 0 none */
    int from, until;   /* the hours it may be used, HHMM; equal: any time */
    bool lt, lw, ls, li, la; /* locked: time, web and topics, settings and design, installs, the companion */
    int used;          /* minutes today (below 0: a grown-up added more than the limit) */
    char day[12];      /* which day `used` is */
    int state_req, used_req;
    uint64_t open_until;
    char open_pw[200];      /* while it is open: changes in Settings > Child lock need it (init checks it again) */
    time_t hours_open_until; /* outside the hours set: a grown-up opened it until then */
    uint64_t last_touch, counted_at;
    bool warned;            /* "five minutes left", said once */
    bool was_over;
} F;

static void hex(const char *s, char *out, size_t n) {
    size_t k = 0;
    for (; *s && k + 3 < n; s++)
        k += (size_t)snprintf(out + k, n - k, "%02x", (unsigned char)*s);
    out[k] = 0;
}

static void unhex(const char *h, size_t len, char *out, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i + 2 <= len && k + 1 < n; i += 2) {
        int a = isdigit((unsigned char)h[i]) ? h[i] - '0' : (h[i] | 32) - 'a' + 10;
        int b = isdigit((unsigned char)h[i + 1]) ? h[i + 1] - '0' : (h[i + 1] | 32) - 'a' + 10;
        if (a < 0 || a > 15 || b < 0 || b > 15)
            break;
        char c = (char)(a << 4 | b);
        if ((unsigned char)c >= 32) /* (nothing that isn't text) */
            out[k++] = c;
    }
    out[k] = 0;
}

static void today(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static int hhmm_now(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 100 + tm.tm_min;
}

/* "child=1;n=...;...;used=12;day=2026-11-06;pw=set" */
static void take_state(const char *s) {
    bool was_child = F.child;
    F.known = true;
    F.child = s && strstr(s, "child=1");
    F.name[0] = F.pronouns[0] = F.birth[0] = 0;
    F.limit = F.from = F.until = 0;
    F.lt = F.lw = F.ls = F.li = F.la = false;
    F.pw_set = false;
    if (!F.child) {
        if (was_child)
            LOG("family: the child lock is off\n");
        return;
    }
    char day[12] = "";
    int used = 0;
    for (const char *p = s; *p;) {
        size_t l = strcspn(p, ";");
        const char *eq = memchr(p, '=', l);
        if (eq) {
            size_t kl = (size_t)(eq - p), vl = l - kl - 1;
            const char *v = eq + 1;
            char val[200];
            snprintf(val, sizeof val, "%.*s", (int)(vl < 199 ? vl : 199), v);
#define K(name) (kl == strlen(name) && !strncmp(p, name, kl))
            if (K("n"))
                unhex(v, vl, F.name, sizeof F.name);
            else if (K("p"))
                unhex(v, vl, F.pronouns, sizeof F.pronouns);
            else if (K("b"))
                snprintf(F.birth, sizeof F.birth, "%.10s", val);
            else if (K("m"))
                F.limit = atoi(val);
            else if (K("f"))
                F.from = atoi(val);
            else if (K("u"))
                F.until = atoi(val);
            else if (K("lt"))
                F.lt = val[0] == '1';
            else if (K("lw"))
                F.lw = val[0] == '1';
            else if (K("ls"))
                F.ls = val[0] == '1';
            else if (K("li"))
                F.li = val[0] == '1';
            else if (K("la"))
                F.la = val[0] == '1';
            else if (K("used"))
                used = atoi(val);
            else if (K("day"))
                snprintf(day, sizeof day, "%.10s", val);
            else if (K("pw"))
                F.pw_set = !strcmp(val, "set");
#undef K
        }
        p += l;
        if (*p == ';')
            p++;
    }
    char now[12];
    today(now, sizeof now);
    if (!strcmp(day, now)) { /* (another day: nothing used yet) */
        F.used = used;
        snprintf(F.day, sizeof F.day, "%s", day);
    } else if (strcmp(F.day, now)) {
        F.used = 0;
        snprintf(F.day, sizeof F.day, "%s", now);
    }
    if (!was_child)
        LOG("family: set up for a child (%d min a day, hours %04d-%04d, locks %s%s%s%s%s)\n", F.limit, F.from,
            F.until, F.lt ? "t" : "", F.lw ? "w" : "", F.ls ? "s" : "", F.li ? "i" : "", F.la ? "a" : "");
}

static void ask_state(void) {
    if (priv_available() && F.state_req <= 0)
        F.state_req = priv_send("family-state");
}

static void take_answers(void) {
    bool ok;
    char *text = NULL;
    if (F.state_req > 0 && priv_take(F.state_req, &ok, &text)) {
        F.state_req = 0;
        take_state(ok && text ? text : "none");
        free(text), text = NULL;
    }
    if (F.used_req > 0 && priv_take(F.used_req, &ok, NULL))
        F.used_req = 0;
}

void family_boot(void) {
    ask_state();
    for (int i = 0; i < 150 && !F.known && priv_available(); i++) { /* the answer is immediate; at most 1.5 s */
        take_answers();
        if (!F.known) {
            struct timespec nap = {0, 10 * 1000000L};
            nanosleep(&nap, NULL);
        }
    }
}

bool family_child(void) { return F.child; }
bool family_open(void) { return F.child && F.open_until && A.now < F.open_until; }

void family_close(void) {
    F.open_until = 0;
    memset(F.open_pw, 0, sizeof F.open_pw);
}

bool family_locks(char what) {
    if (!F.child || family_open())
        return false;
    switch (what) {
    case 't': return F.lt;
    case 'w': return F.lw;
    case 's': return F.ls;
    case 'i': return F.li;
    case 'a': return F.la;
    }
    return false;
}

const char *family_name(void) { return F.name; }
const char *family_pronouns(void) { return F.pronouns; }
const char *family_birth(void) { return F.birth; }

int age_from_birth(const char *b) {
    int y, m, d;
    if (!b || sscanf(b, "%d-%d-%d", &y, &m, &d) != 3 || y < 1900 || m < 1 || m > 12 || d < 1 || d > 31)
        return -1;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int age = tm.tm_year + 1900 - y;
    if (tm.tm_mon + 1 < m || (tm.tm_mon + 1 == m && tm.tm_mday < d))
        age--;
    return age >= 0 && age < 130 ? age : -1;
}

int family_minutes_left(void) {
    if (!F.child || !F.lt || F.limit <= 0)
        return -1;
    int left = F.limit - F.used;
    return left > 0 ? left : 0;
}

/* ---- the grown-up's password, asked ---- */

static void opened_with(const char *pw) {
    F.open_until = A.now + OPEN_MS;
    snprintf(F.open_pw, sizeof F.open_pw, "%s", pw);
    LOG("family: a grown-up opened it\n");
}

/* One password field, its request and what came of it (the box, a window's gate and the time's-up screen each
 * have one). */
struct pwask {
    struct textfield pw;
    int req;
    char typed[200]; /* what was sent, kept until the answer: right, it is the open password */
    char msg[96];
    uint64_t msg_at, shake_at;
    bool done;       /* the answer was yes (taken by whoever asked) */
};

static void pwask_send(struct pwask *q) {
    if (!q->pw.buf[0] || q->req > 0)
        return;
    if (!priv_available()) {
        snprintf(q->msg, sizeof q->msg, "The system can't check a password here.");
        q->msg_at = A.now;
        return;
    }
    char h[420], m[440];
    hex(q->pw.buf, h, sizeof h);
    snprintf(m, sizeof m, "family-check %s", h);
    snprintf(q->typed, sizeof q->typed, "%s", q->pw.buf);
    q->req = priv_send(m);
    memset(h, 0, sizeof h), memset(m, 0, sizeof m);
    memset(q->pw.buf, 0, sizeof q->pw.buf), q->pw.len = 0;
    te_reset(&q->pw.ed, q->pw.buf);
}

static void pwask_poll(struct pwask *q) {
    bool ok;
    char *text = NULL;
    if (q->req <= 0 || !priv_take(q->req, &ok, &text))
        return;
    q->req = 0;
    if (ok) {
        opened_with(q->typed);
        q->done = true;
        q->msg[0] = 0;
    } else {
        int w = text && strrchr(text, ' ') ? atoi(strrchr(text, ' ') + 1) : 0;
        if (text && !strncmp(text, "wait", 4))
            snprintf(q->msg, sizeof q->msg, "Too many tries. Wait %d seconds.", w);
        else if (w)
            snprintf(q->msg, sizeof q->msg, "That isn't it. Wait %d seconds before trying again.", w);
        else
            snprintf(q->msg, sizeof q->msg, "That isn't it.");
        q->msg_at = q->shake_at = A.now;
        LOG("family: a wrong grown-up password\n");
    }
    memset(q->typed, 0, sizeof q->typed);
    free(text);
}

static int shake(const struct pwask *q) {
    if (!q->shake_at || A.now - q->shake_at > 400)
        return 0;
    float u = (float)(A.now - q->shake_at) / 400.0f;
    return (int)(10 * (1 - u) * (((A.now - q->shake_at) / 50) % 2 ? 1 : -1));
}

static const char *pwask_hint(const struct pwask *q) {
    return q->req > 0 ? "Checking..." : q->msg[0] && A.now - q->msg_at < 8000 ? q->msg : "";
}

/* the box (family_ask) */
static struct {
    bool up;
    char doing[120];
    void (*then)(const char *arg);
    char arg[400];
    struct pwask q;
    uint64_t since;
} Q = {.q = {.pw = {.secret = true}}};

void family_ask(const char *doing, void (*then)(const char *arg), const char *arg) {
    if (!F.child || family_open()) {
        if (then)
            then(arg ? arg : "");
        return;
    }
    Q.up = true;
    Q.since = A.now;
    snprintf(Q.doing, sizeof Q.doing, "%s", doing ? doing : "This");
    Q.then = then;
    snprintf(Q.arg, sizeof Q.arg, "%s", arg ? arg : "");
    Q.q.msg[0] = 0;
    Q.q.done = false;
    LOG("family: a grown-up's password asked for: %s\n", Q.doing);
    char say[200];
    snprintf(say, sizeof say, "%s needs a grown-up's password.", Q.doing);
    reader_announce(say);
}

bool family_asking(void) { return Q.up; }

void family_ask_draw(void) {
    if (!Q.up)
        return;
    pwask_poll(&Q.q);
    if (Q.q.done) {
        Q.up = false;
        Q.q.done = false;
        if (Q.then)
            Q.then(Q.arg);
        return;
    }
    canvas *c = A.frame;
    fill_rect(c, (rect){0, 0, c->w, c->h}, RGBA(0, 0, 0, 90));
    int w = 460 < c->w - 40 ? 460 : c->w - 40, h = 250;
    rect box = {(c->w - w) / 2 + shake(&Q.q), (c->h - h) / 2, w, h};
    ctl_panel(box, 18, 1);
    palette p = pal_on(pal_sample(box), 1);
    font *head = fm_host_font(D.font_heading, 19), *body = fm_host_font(D.font_body, 15),
         *small = fm_host_font(D.font_body, 13);
    int x = box.x + 26, y = box.y + 24;
    draw_text(c, head, x, y, "A grown-up's password", p.ink);
    y += font_line_height(head) + 8;
    char line[200];
    snprintf(line, sizeof line, "%s needs a grown-up. Ask one to type their password.", Q.doing);
    y += draw_text_wrapped(c, small, (rect){x, y, w - 52, 60}, line, p.muted, 1.3f) + 12;
    rect f = {x, y, w - 52, 44};
    ui_textfield(f, &Q.q.pw, true, "Password", body, color_alpha(p.ink, 0.06f), p.ink, p.muted, 10);
    a11y_note(f, "field", "A grown-up's password", Q.q.pw.buf[0] ? "typed" : "empty", true);
    bool enter = ui_type(&Q.q.pw);
    y += 52;
    draw_text(c, small, x, y, pwask_hint(&Q.q), p.muted);
    rect ok = {box.x + w - 26 - 120, box.y + h - 58, 120, 40}, no = {ok.x - 132, ok.y, 120, 40};
    bool esc = false;
    for (int i = 0; i < A.in.nkeys; i++)
        esc |= A.in.keys[i] == K_ESC;
    if (ctl_button(ok, "Open", body, true, Q.q.pw.buf[0] && Q.q.req <= 0, &p) || enter)
        pwask_send(&Q.q);
    if (ctl_button(no, "Cancel", body, false, true, &p) || esc) {
        Q.up = false;
        memset(Q.q.pw.buf, 0, sizeof Q.q.pw.buf), Q.q.pw.len = 0;
        te_reset(&Q.q.pw.ed, Q.q.pw.buf);
        LOG("family: the grown-up's password wasn't given\n");
    }
    input_clear(&A.in);
}

/* ---- a window's gate ---- */

static struct pwask G = {.pw = {.secret = true}};

bool family_gate(rect r, char lock, const char *what, const palette *p, font *body, font *small) {
    if (lock ? !family_locks(lock) : !F.child || family_open()) {
        G.done = false;
        return true;
    }
    pwask_poll(&G);
    if (G.done) {
        G.done = false;
        return true;
    }
    static uint64_t typed_at = (uint64_t)-1;
    canvas *c = A.frame;
    int w = r.w - 60 < 420 ? r.w - 60 : 420, x = r.x + (r.w - w) / 2, y = r.y + r.h / 2 - 110;
    if (y < r.y + 20)
        y = r.y + 20;
    font *head = fm_host_font(D.font_heading, 20);
    draw_text_centered(c, head, (rect){r.x, y, r.w, 30}, what ? what : "Locked", p->ink);
    y += 40;
    draw_text_centered(c, small, (rect){r.x, y, r.w, 22}, "Locked by a grown-up. A grown-up can open it with their password.",
                       p->muted);
    y += 34;
    rect f = {x + shake(&G), y, w, 44};
    ui_textfield(f, &G.pw, true, "A grown-up's password", body, color_alpha(p->ink, 0.06f), p->ink, p->muted, 10);
    a11y_note(f, "field", "A grown-up's password", G.pw.buf[0] ? "typed" : "empty", true);
    if (typed_at != A.now) { /* (two windows locked at once: the keys go to one) */
        typed_at = A.now;
        if (ui_type(&G.pw))
            pwask_send(&G);
    }
    y += 54;
    rect ok = {x + w / 2 - 60, y, 120, 40};
    if (ctl_button(ok, "Open", body, true, G.pw.buf[0] && G.req <= 0, p))
        pwask_send(&G);
    draw_text_centered(c, small, (rect){r.x, y + 50, r.w, 22}, pwask_hint(&G), p->muted);
    return false;
}

/* ---- the time it is used, and the hours ---- */

static bool within_hours(void) {
    if (F.from == F.until)
        return true;
    int now = hhmm_now();
    return F.from < F.until ? now >= F.from && now < F.until : now >= F.from || now < F.until;
}

static bool touched(void) {
    return A.in.clicked || A.in.pressed || A.in.nkeys || A.in.wheel || A.in.moved || A.in.down;
}

static void send_used(void) {
    if (!priv_available() || F.used_req > 0)
        return;
    char m[48];
    snprintf(m, sizeof m, "family-used %s:%d", F.day, F.used);
    F.used_req = priv_send(m);
}

static void add_time(int minutes) {
    if (F.lt && F.limit > 0 && F.used > F.limit - minutes) {
        F.used = (F.used > F.limit ? F.limit : F.used) - minutes; /* (from the limit: added time is time left) */
        if (F.used < -1440)
            F.used = -1440;
        send_used();
    }
    time_t t = time(NULL);
    if (!within_hours() || (F.hours_open_until && F.hours_open_until > t))
        F.hours_open_until = (F.hours_open_until > t ? F.hours_open_until : t) + minutes * 60;
    F.warned = false;
    LOG("family: a grown-up added %d minutes\n", minutes);
}

static void rest_of_day(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int left = (23 - tm.tm_hour) * 60 + (60 - tm.tm_min);
    add_time(left > 1 ? left : 1);
}

static struct pwask T = {.pw = {.secret = true}};
static bool t_asking, t_open; /* "a grown-up: add time", then the password right: how much */

static bool times_up(void) {
    if (!F.child || !F.lt)
        return false;
    bool over = F.limit > 0 && F.used >= F.limit;
    bool hours = !within_hours() && !(F.hours_open_until && time(NULL) < F.hours_open_until);
    return over || hours;
}

bool family_frame(void) {
    take_answers();
    static uint64_t asked_at;
    if (priv_available() && A.now - asked_at > 60000) { /* (changed in Settings, or by init: kept up to date) */
        asked_at = A.now;
        ask_state();
    }
    if (!F.child)
        return false;
    char now[12];
    today(now, sizeof now);
    if (strcmp(F.day, now)) { /* a new day: nothing used yet */
        snprintf(F.day, sizeof F.day, "%s", now);
        F.used = 0;
        F.warned = false;
        F.hours_open_until = 0;
        send_used();
    }
    if (touched())
        F.last_touch = A.now;
    if (!F.counted_at)
        F.counted_at = A.now;
    if (A.now - F.counted_at >= 60000) { /* a minute: counted when they were there (or watching something) */
        F.counted_at = A.now;
        bool there = F.last_touch && A.now - F.last_touch < 3 * 60000;
        if (!lock_active() && (there || senses_fullscreen()) && F.lt && F.limit > 0) {
            F.used++;
            send_used();
        }
    }
    int left = family_minutes_left();
    if (left > 0 && left <= 5 && !F.warned) {
        F.warned = true;
        char m[120];
        snprintf(m, sizeof m, "%d minute%s left today.", left, left == 1 ? "" : "s");
        toast("%s", m);
        reader_announce(m);
    }
    if (!times_up() || lock_active()) {
        if (F.was_over)
            LOG("family: open again\n");
        F.was_over = false;
        t_asking = t_open = false;
        return false;
    }
    if (!F.was_over) {
        F.was_over = true;
        LOG("family: %s\n", F.limit > 0 && F.used >= F.limit ? "time's up for today" : "outside the hours set");
        reader_announce(F.limit > 0 && F.used >= F.limit ? "Time's up for today." : "It's outside the hours set for this computer.");
    }
    canvas *c = A.frame;
    if (A.wall && A.wall->w == c->w && A.wall->h == c->h)
        memcpy(c->px, A.wall->px, (size_t)c->w * (size_t)c->h * 4);
    else
        fill_rect(c, (rect){0, 0, c->w, c->h}, D.bg | 0xff000000u);
    fill_rect(c, (rect){0, 0, c->w, c->h}, RGBA(0, 0, 0, 130));
    color ink = RGBA(246, 244, 241, 255), soft = RGBA(246, 244, 241, 185);
    font *big = fm_host_font(D.font_heading, 52), *body = fm_host_font(D.font_body, 20),
         *small = fm_host_font(D.font_body, 15);
    bool over = F.limit > 0 && F.used >= F.limit;
    int y = c->h / 2 - 150;
    draw_text_centered(c, big, (rect){0, y, c->w, 70}, over ? "Time's up for today" : "Not now", ink);
    y += 86;
    char line[240];
    if (over) {
        int hrs = F.limit / 60, mins = F.limit % 60;
        char amount[48];
        if (hrs && mins)
            snprintf(amount, sizeof amount, "%d h %d min", hrs, mins);
        else if (hrs)
            snprintf(amount, sizeof amount, "%d hour%s", hrs, hrs == 1 ? "" : "s");
        else
            snprintf(amount, sizeof amount, "%d minutes", mins);
        if (F.name[0])
            snprintf(line, sizeof line, "%s, that's your %s for today. See you tomorrow.", F.name, amount);
        else
            snprintf(line, sizeof line, "That's the %s for today. See you tomorrow.", amount);
    } else {
        snprintf(line, sizeof line, "This computer can be used from %02d:%02d to %02d:%02d.", F.from / 100, F.from % 100,
                 F.until / 100, F.until % 100);
    }
    draw_text_centered(c, body, (rect){0, y, c->w, 30}, line, soft);
    y += 70;
    pwask_poll(&T);
    if (T.done) {
        T.done = false;
        t_open = true;
    }
    if (t_open) { /* how much */
        draw_text_centered(c, small, (rect){0, y, c->w, 24}, "How much more?", soft);
        y += 34;
        static const char *const l[] = {"15 minutes", "30 minutes", "1 hour", "The rest of the day"};
        static const int v[] = {15, 30, 60, 0};
        int bw = 170, gap = 12, total = 4 * bw + 3 * gap, x = (c->w - total) / 2;
        for (int i = 0; i < 4; i++) {
            rect b = {x + i * (bw + gap), y, bw, 44};
            if (ui_button(b, l[i], small, RGBA(255, 255, 255, 40), ink, 22, true)) {
                if (v[i])
                    add_time(v[i]);
                else
                    rest_of_day();
                t_open = t_asking = false;
                family_close(); /* (the time, not the whole computer) */
            }
        }
    } else if (t_asking) {
        rect f = {c->w / 2 - 170 + shake(&T), y, 340, 48};
        ui_textfield(f, &T.pw, true, "A grown-up's password", body, RGBA(255, 255, 255, 34), ink, soft, 24);
        a11y_note(f, "field", "A grown-up's password", T.pw.buf[0] ? "typed" : "empty", true);
        if (ui_type(&T.pw))
            pwask_send(&T);
        draw_text_centered(c, small, (rect){0, y + 60, c->w, 24}, pwask_hint(&T), soft);
        for (int i = 0; i < A.in.nkeys; i++)
            if (A.in.keys[i] == K_ESC)
                t_asking = false;
    } else {
        rect b = {c->w / 2 - 130, y, 260, 46};
        if (ui_button(b, "A grown-up can add time", small, RGBA(255, 255, 255, 40), ink, 23, true))
            t_asking = true;
    }
    /* turning it off is for anyone, as on the lock screen */
    static bool confirm;
    rect off = {c->w - 150, c->h - 64, 120, 40};
    if (confirm) {
        rect no = {off.x - 130, off.y, 120, 40};
        if (ui_button(no, "Not yet", small, RGBA(255, 255, 255, 30), ink, 20, true))
            confirm = false;
        else if (ui_button(off, "Shut down", small, D.accent | 0xff000000u, readable_on(D.accent), 20, true)) {
            alive_shutdown();
            if (priv_available()) {
                priv_call("save", 60000);
                priv_call("power-off", 5000);
            } else {
                store_save();
                sync();
                reboot(RB_POWER_OFF);
            }
        }
    } else if (ui_button(off, "Shut down", small, RGBA(255, 255, 255, 30), ink, 20, true)) {
        confirm = true;
    }
    input_clear(&A.in);
    return true;
}

/* ---- what the companion is told ---- */

void family_prompt(char *out, size_t n) {
    out[0] = 0;
    size_t o = 0;
#define ADD(...) (o += o < n ? (size_t)snprintf(out + o, n - o, __VA_ARGS__) : 0)
    const char *pr = F.child && F.pronouns[0] ? F.pronouns : A.prof.pronouns;
    const char *who = F.child && F.name[0] ? F.name : A.prof.user_name[0] ? A.prof.user_name : "them";
    if (pr[0]) {
        if (strcasestr(pr, "name"))
            ADD("\n\nWhen you speak of them, use their name (%s), not he, she or they.", who);
        else
            ADD("\n\nTheir pronouns are %s: use them whenever you speak of them.", pr);
        ADD(" That is only how to refer to them; it changes nothing about how you talk with them.");
    }
    int age = age_from_birth(F.child ? F.birth : A.prof.birth);
    if (age >= 0 && !F.child)
        ADD("\n\nThey are %d. Let it shape only how you explain things (the words, how much you say), never what you "
            "assume about them.", age);
    if (!F.child)
        return;
    char aged[32] = "";
    if (age >= 0)
        snprintf(aged, sizeof aged, ", who is %d", age);
    ADD("\n\nThis computer is a child's: %s%s. A grown-up set it up for them, with limits. Talk in a way that suits "
        "a child of that age: words they know, short and clear explanations, patient and kind, never talking down; "
        "nothing about them assumed beyond their age.",
        who, aged);
    int left = family_minutes_left();
    if (left >= 0)
        ADD(" They have %d minute%s left on the computer today.", left, left == 1 ? "" : "s");
    if (F.lw || F.la)
        ADD("\nKeep every topic right for their age: no sex or nudity, no graphic violence or gore, no drugs, "
            "alcohol, smoking or gambling, nothing frightening or cruel, no hateful words, no ways to hurt "
            "themselves or anyone. If they bring any of that up, steer kindly to something else; if they seem "
            "upset, scared or in danger, or someone is hurting them, be gentle and tell them to talk to a grown-up "
            "they trust, now.");
    if (F.la)
        ADD("\nStrict limits (a grown-up's): never run commands or programs for them, never delete, move or "
            "overwrite files, never install or remove apps, never change Settings, and never help them get round a "
            "limit or the grown-up's password (never ask for it, guess it or tell them how). Never ask where they "
            "live, their school, their full name, or for photos; never arrange for them to meet or talk to "
            "anyone; if someone online asks them for any of that, tell them to tell a grown-up.");
    if (F.ls)
        ADD("\nChanging how the computer looks, and Settings, need a grown-up's password: when they ask, say a "
            "grown-up will have to type it, then make the change as usual (the system asks for the password). "
            "Putting a change back never needs it.");
    if (F.li)
        ADD("\nInstalling or removing apps needs a grown-up's password.");
#undef ADD
}

/* ---- the grown-up's form ---- */

static struct {
    bool loaded, editing;
    struct textfield name, day, mon, year, pr, pw1, pw2;
    int focus;
    int pron, limit, hours;
    bool lt, lw, ls, li, la;
    int req;
    char msg[140];
    bool handed, removing, remove_asked;
} FF = {.pw1 = {.secret = true}, .pw2 = {.secret = true}};

static const char *const pron_l[] = {"she/her", "he/him", "they/them", "Their name"};
static const char *const limit_l[] = {"No limit", "1 hour", "2 hours", "3 hours", "4 hours"};
static const int limit_v[] = {0, 60, 120, 180, 240};
static const char *const hours_l[] = {"Any time", "7:00 - 19:00", "7:00 - 20:00", "8:00 - 21:00"};
static const int hours_from[] = {0, 700, 700, 800}, hours_until[] = {0, 1900, 2000, 2100};

static void field_set(struct textfield *t, const char *s) {
    snprintf(t->buf, sizeof t->buf, "%s", s);
    t->len = (int)strlen(t->buf);
    te_reset(&t->ed, t->buf);
}

static void wipe(struct textfield *t) {
    memset(t->buf, 0, sizeof t->buf), t->len = 0;
    te_reset(&t->ed, t->buf);
}

void family_form_reset(void) {
    wipe(&FF.pw1), wipe(&FF.pw2);
    bool h = FF.handed;
    memset(&FF, 0, sizeof FF);
    FF.pw1.secret = FF.pw2.secret = true;
    FF.handed = h;
}

static void form_load(void) {
    FF.loaded = true;
    FF.lt = FF.lw = FF.ls = FF.li = FF.la = true;
    FF.limit = 2, FF.hours = 2, FF.pron = -1;
    if (!F.child)
        return;
    field_set(&FF.name, F.name);
    int y, m, d;
    if (sscanf(F.birth, "%d-%d-%d", &y, &m, &d) == 3) {
        char b[8];
        snprintf(b, sizeof b, "%d", d), field_set(&FF.day, b);
        snprintf(b, sizeof b, "%d", m), field_set(&FF.mon, b);
        snprintf(b, sizeof b, "%d", y), field_set(&FF.year, b);
    }
    for (int i = 0; i < 4; i++)
        if (!strcasecmp(F.pronouns, pron_l[i]) || (i == 3 && !strcasecmp(F.pronouns, "name")))
            FF.pron = i;
    if (FF.pron < 0 && F.pronouns[0])
        field_set(&FF.pr, F.pronouns);
    FF.limit = 0;
    for (int i = 0; i < 5; i++)
        if (limit_v[i] == F.limit)
            FF.limit = i;
    FF.hours = 0;
    for (int i = 0; i < 4; i++)
        if (hours_from[i] == F.from && hours_until[i] == F.until)
            FF.hours = i;
    FF.lt = F.lt, FF.lw = F.lw, FF.ls = F.ls, FF.li = F.li, FF.la = F.la;
}

static bool birth_of_form(char *out, size_t n) {
    int d = atoi(FF.day.buf), m = atoi(FF.mon.buf), y = atoi(FF.year.buf);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    static const int days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1 || d > days[m - 1] || y < 1990 || y > tm.tm_year + 1900)
        return false;
    snprintf(out, n, "%04d-%02d-%02d", y, m, d);
    return age_from_birth(out) >= 0;
}

static void form_pronouns(char *out, size_t n) {
    if (FF.pr.buf[0])
        snprintf(out, n, "%.40s", FF.pr.buf);
    else if (FF.pron == 3)
        snprintf(out, n, "name");
    else if (FF.pron >= 0)
        snprintf(out, n, "%s", pron_l[FF.pron]);
    else
        out[0] = 0;
}

static void form_send(bool remove) {
    if (!priv_available()) {
        snprintf(FF.msg, sizeof FF.msg, "The system can't keep a child lock here.");
        return;
    }
    char cfg[420] = "", birth[12], pr[48], nh[100], ph[100];
    if (!remove) {
        birth_of_form(birth, sizeof birth);
        form_pronouns(pr, sizeof pr);
        hex(FF.name.buf, nh, sizeof nh);
        hex(pr, ph, sizeof ph);
        snprintf(cfg, sizeof cfg, "child=1;n=%s;b=%s;p=%s;m=%d;f=%d;u=%d;lt=%d;lw=%d;ls=%d;li=%d;la=%d", nh, birth, ph,
                 limit_v[FF.limit], hours_from[FF.hours], hours_until[FF.hours], FF.lt, FF.lw, FF.ls, FF.li, FF.la);
    }
    static char a[220], b[220], ch[860], m[1200];
    hex(F.pw_set ? F.open_pw : "", a, sizeof a);
    hex(remove ? "" : FF.pw1.buf, b, sizeof b);
    hex(cfg, ch, sizeof ch);
    snprintf(m, sizeof m, "family-set %s:%s:%s", a, b, ch);
    FF.req = priv_send(m);
    FF.removing = remove;
    memset(a, 0, sizeof a), memset(b, 0, sizeof b), memset(m, 0, sizeof m);
    FF.msg[0] = 0;
    LOG("family: the child lock %s\n", remove ? "taken away" : F.child ? "changed" : "set up");
}

static void form_poll(bool onboarding) {
    bool ok;
    char *text = NULL;
    if (FF.req <= 0 || !priv_take(FF.req, &ok, &text))
        return;
    FF.req = 0;
    if (ok) {
        if (FF.pw1.buf[0])
            opened_with(FF.pw1.buf); /* (the new one is the open one now) */
        wipe(&FF.pw1), wipe(&FF.pw2);
        snprintf(FF.msg, sizeof FF.msg, "%s", FF.removing ? "The child lock is off." : "Kept.");
        if (FF.removing)
            family_close();
        F.known = false;
        ask_state();
        family_boot(); /* (what it is now, at once) */
        if (onboarding && !FF.removing)
            FF.handed = true;
        FF.editing = false;
        FF.remove_asked = false;
    } else {
        snprintf(FF.msg, sizeof FF.msg, "It couldn't be kept%s%s.", text && text[0] ? ": " : "", text ? text : "");
    }
    free(text);
}

bool family_handed_over(void) {
    if (!FF.handed)
        return false;
    FF.handed = false;
    return true;
}

/* a labelled field; returns its bottom */
static int form_field(int x, int y, int w, const char *label, struct textfield *t, int idx, const char *hint,
                      const palette *p, font *body, font *small) {
    draw_text(A.frame, small, x, y, label, p->muted);
    y += font_line_height(small) + 4;
    rect r = {x, y, w, 42};
    if (A.in.clicked && pt_in(r, A.in.mx, A.in.my))
        FF.focus = idx;
    ui_textfield(r, t, FF.focus == idx, hint, body, color_alpha(p->ink, 0.06f), p->ink, p->muted, 10);
    a11y_note(r, "field", label, t->buf[0] ? (t->secret ? "typed" : t->buf) : "empty", FF.focus == idx);
    if (FF.focus == idx)
        ui_type(t);
    return y + 42 + 14;
}

static int form_switch(int x, int y, int w, const char *label, const char *sub, bool *v, const palette *p, font *body,
                       font *small) {
    rect s = {x + w - 52, y + 4, 48, 28};
    draw_text(A.frame, body, x, y, label, p->ink);
    int h = font_line_height(body);
    if (sub)
        h += draw_text_wrapped(A.frame, small, (rect){x, y + font_line_height(body) + 2, w - 70, 60}, sub, p->muted, 1.25f) + 2;
    if (ctl_switch(s, *v, p))
        *v = !*v;
    a11y_note(s, "switch", label, *v ? "on" : "off", false);
    return y + (h > 36 ? h : 36) + 12;
}

int family_form(rect c, int y, const palette *p, font *body, font *small, font *head, bool onboarding) {
    int y0 = y;
    if (!FF.loaded)
        form_load();
    form_poll(onboarding);
    /* Tab: the next field */
    int k = 0;
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_TAB)
            FF.focus = (FF.focus + 1) % 7;
        else
            A.in.keys[k++] = A.in.keys[i];
    A.in.nkeys = k;
    bool two = c.w >= 860;
    int colw = two ? (c.w - 40) / 2 : c.w, x1 = c.x, x2 = two ? c.x + colw + 40 : c.x;
    if (colw > 520)
        colw = 520;
    int ya = y;
    /* who */
    draw_text(A.frame, head, x1, ya, "About them", p->ink);
    ya += font_line_height(head) + 10;
    ya = form_field(x1, ya, colw, "Their name", &FF.name, 0, "Their first name", p, body, small);
    draw_text(A.frame, small, x1, ya, "Their date of birth (only their age is told to the AI)", p->muted);
    ya += font_line_height(small) + 4;
    {
        struct textfield *ds[3] = {&FF.day, &FF.mon, &FF.year};
        const char *hs[3] = {"Day", "Month", "Year"};
        int ws[3] = {70, 70, 100}, x = x1;
        for (int i = 0; i < 3; i++) {
            rect r = {x, ya, ws[i], 42};
            if (A.in.clicked && pt_in(r, A.in.mx, A.in.my))
                FF.focus = 1 + i;
            /* digits only */
            if (FF.focus == 1 + i) {
                int kk = 0;
                for (int q = 0; q < A.in.nkeys; q++)
                    if (A.in.keys[q] >= 0x100 || isdigit(A.in.keys[q]))
                        A.in.keys[kk++] = A.in.keys[q];
                A.in.nkeys = kk;
                if (ds[i]->len >= (i == 2 ? 4 : 2)) { /* full: on to the next */
                    for (int q = 0; q < A.in.nkeys; q++)
                        if (A.in.keys[q] < 0x100 && i < 2) {
                            FF.focus = 2 + i;
                            break;
                        }
                }
            }
            ui_textfield(r, ds[i], FF.focus == 1 + i, hs[i], body, color_alpha(p->ink, 0.06f), p->ink, p->muted, 10);
            a11y_note(r, "field", hs[i], ds[i]->buf[0] ? ds[i]->buf : "empty", FF.focus == 1 + i);
            if (FF.focus == 1 + i && ds[i]->len < (i == 2 ? 4 : 2))
                ui_type(ds[i]);
            else if (FF.focus == 1 + i) { /* full: only taking away */
                bool back = false;
                for (int q = 0; q < A.in.nkeys; q++)
                    back |= A.in.keys[q] == K_BACKSPACE;
                if (back)
                    ui_type(ds[i]);
            }
            x += ws[i] + 10;
        }
        char b[12];
        int age = birth_of_form(b, sizeof b) ? age_from_birth(b) : -1;
        if (age >= 0) {
            char s[32];
            snprintf(s, sizeof s, "%d year%s old", age, age == 1 ? "" : "s");
            draw_text(A.frame, small, x + 6, ya + 12, s, p->muted);
        }
        ya += 42 + 14;
    }
    draw_text(A.frame, small, x1, ya, "How the AI should refer to them", p->muted);
    ya += font_line_height(small) + 4;
    int ps = ctl_segmented((rect){x1, ya, colw, 38}, pron_l, 4, FF.pron, small, p);
    if (ps != FF.pron) {
        FF.pron = ps;
        wipe(&FF.pr);
    }
    ya += 38 + 10;
    ya = form_field(x1, ya, colw, "Or in their own words", &FF.pr, 4, "e.g. she/they", p, body, small);
    if (FF.pr.buf[0])
        FF.pron = -1;
    /* the password */
    ya += 6;
    draw_text(A.frame, head, x1, ya, F.child ? "A new grown-up password" : "A grown-up password", p->ink);
    ya += font_line_height(head) + 4;
    ya += draw_text_wrapped(A.frame, small, (rect){x1, ya, colw, 60},
                            F.child ? "Leave these empty to keep the one you have."
                                    : "Only for grown-ups: it opens what is locked. Never shown to the AI.",
                            p->muted, 1.3f) + 8;
    ya = form_field(x1, ya, colw, "Password", &FF.pw1, 5, "At least 4 characters", p, body, small);
    ya = form_field(x1, ya, colw, "The same again", &FF.pw2, 6, "Password", p, body, small);

    /* what is locked */
    int yb = two ? y : ya + 10;
    draw_text(A.frame, head, x2, yb, "What is locked", p->ink);
    yb += font_line_height(head) + 10;
    yb = form_switch(x2, yb, colw, "Time", "How long a day, and the hours it can be used", &FF.lt, p, body, small);
    if (FF.lt) {
        draw_text(A.frame, small, x2, yb, "A day", p->muted);
        yb += font_line_height(small) + 4;
        FF.limit = ctl_segmented((rect){x2, yb, colw, 38}, limit_l, 5, FF.limit, small, p);
        yb += 38 + 10;
        draw_text(A.frame, small, x2, yb, "Hours", p->muted);
        yb += font_line_height(small) + 4;
        FF.hours = ctl_segmented((rect){x2, yb, colw, 38}, hours_l, 4, FF.hours, small, p);
        yb += 38 + 16;
    }
    yb = form_switch(x2, yb, colw, "The web and topics", "Safe search, and nothing unsuitable for their age",
                     &FF.lw, p, body, small);
    yb = form_switch(x2, yb, colw, "Settings, the design and the Terminal",
                     "Changes need the password (putting a change back never does)", &FF.ls, p, body, small);
    yb = form_switch(x2, yb, colw, "Installing and removing apps", NULL, &FF.li, p, body, small);
    yb = form_switch(x2, yb, colw, "The AI's limits",
                     "No running commands, no deleting files, no risky topics, never around a limit", &FF.la, p,
                     body, small);
    y = (ya > yb ? ya : yb) + 8;

    /* done */
    char birth[12];
    bool born = birth_of_form(birth, sizeof birth);
    bool pw_same = !strcmp(FF.pw1.buf, FF.pw2.buf);
    bool pw_ok = pw_same && (FF.pw1.len >= 4 || (F.child && !FF.pw1.buf[0]));
    bool ready = FF.name.buf[0] && born && pw_ok && FF.req <= 0;
    const char *why = !FF.name.buf[0] ? "Their name, please."
                      : !born ? "Their date of birth, as day, month and year."
                      : !pw_same ? "The two passwords differ."
                      : !pw_ok ? "A password of at least 4 characters."
                               : "";
    char label[96];
    if (onboarding)
        snprintf(label, sizeof label, "Hand over to %.40s", FF.name.buf[0] ? FF.name.buf : "them");
    else
        snprintf(label, sizeof label, "%s", F.child ? "Keep the changes" : "Set up the child lock");
    rect go = {c.x, y, text_width(body, label) + 48, 44};
    if (ctl_button(go, label, body, true, ready, p))
        form_send(false);
    int mx = go.x + go.w + 20;
    const char *shown = FF.req > 0 ? "Keeping..." : FF.msg[0] ? FF.msg : why;
    draw_text(A.frame, small, mx, y + 13, shown, p->muted);
    y += 44 + 18;
    if (!onboarding && F.child) {
        rect again = {c.x, y, 180, 40};
        if (ctl_button(again, "Lock it again now", small, false, true, p)) {
            family_close();
            toast("Locked again.");
        }
        rect rm = {again.x + again.w + 12, y, 220, 40};
        if (!FF.remove_asked) {
            if (ctl_button(rm, "Take the child lock away", small, false, FF.req <= 0, p))
                FF.remove_asked = true;
        } else {
            rect sure = {rm.x, y, 200, 40}, no = {rm.x + 212, y, 110, 40};
            if (ctl_button(sure, "Yes, take it away", small, true, FF.req <= 0, p))
                form_send(true);
            if (ctl_button(no, "Keep it", small, false, true, p))
                FF.remove_asked = false;
        }
        y += 40 + 12;
        int left = (int)((F.open_until - A.now) / 60000) + 1;
        char s[120];
        snprintf(s, sizeof s, "Open to a grown-up for %d more minute%s.", left, left == 1 ? "" : "s");
        draw_text(A.frame, small, c.x, y, s, p->muted);
        y += font_line_height(small) + 8;
    }
    return y - y0;
}

/* ---- Settings > Child lock ---- */

int family_settings_page(rect c, int y) {
    const palette *p = settings_palette();
    font *body = settings_font(SF_BODY), *small = settings_font(SF_SMALL), *head = settings_font(SF_SECTION);
    int y0 = y;
    take_answers();
    if (!F.child && !FF.editing) {
        y = settings_section(c, y, "Child lock");
        y += draw_text_wrapped(A.frame, small, (rect){c.x, y, c.w < 620 ? c.w : 620, 200},
                               "For a computer that is a child's: a grown-up's password, how long a day and at what "
                               "hours, the web and topics suitable for their age, Settings and the design, "
                               "installing apps, and limits on the AI. Something locked asks for the password; the AI "
                               "can always put a change back.",
                               p->muted, 1.35f) + 16;
        if (ctl_button((rect){c.x, y, 220, 42}, "Set up a child lock", body, true, priv_available(), p)) {
            FF.loaded = false;
            FF.editing = true;
        }
        y += 42 + 10;
        if (!priv_available())
            y += draw_text_wrapped(A.frame, small, (rect){c.x, y, c.w, 60}, "The system can't keep a child lock here.",
                                   p->muted, 1.3f);
        return y - y0 + 10;
    }
    if (F.child && !family_open()) { /* (Settings itself may be open to them: this page never is) */
        rect r = {c.x, y, c.w, 360};
        family_gate(r, 0, "The child lock", p, body, small);
        return 380;
    }
    if (!F.child && FF.editing) {
        rect cancel = {c.x + c.w - 110, y, 110, 36};
        if (ctl_button(cancel, "Cancel", small, false, true, p))
            family_form_reset();
    }
    y += family_form(c, y, p, body, small, head, false);
    return y - y0 + 10;
}
