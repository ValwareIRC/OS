#define _GNU_SOURCE
#include "lock.h"
#include "app.h"
#include "emblem.h"
#include "priv.h"
#include "settings.h"
#include "fm/fm.h"

#include "store.h"
#include "alive.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* The lock screen: see lock.h. */

#define D (A.d)

static struct {
    bool locked, has_pw, known;
    int state_req, check_req, set_req;
    int set_result;
    char set_why[48];
    struct textfield pw;
    char msg[96];
    uint64_t msg_at, shake_at, last_input, locked_at;
    char confirm; /* 'r' restart or 's' shut down, asked */
} L = {.pw = {.secret = true}};

static void hex(const char *s, char *out, size_t n) {
    size_t k = 0;
    for (; *s && k + 3 < n; s++)
        k += (size_t)snprintf(out + k, n - k, "%02x", (unsigned char)*s);
    out[k] = 0;
}

static void ask_state(void) {
    if (priv_available() && L.state_req <= 0)
        L.state_req = priv_send("password-state");
}

static void take_answers(void) {
    bool ok;
    char *text = NULL;
    if (L.state_req > 0 && priv_take(L.state_req, &ok, &text)) {
        L.state_req = 0;
        L.known = true;
        L.has_pw = ok && text && !strcmp(text, "set");
        free(text), text = NULL;
    }
    if (L.check_req > 0 && priv_take(L.check_req, &ok, &text)) {
        L.check_req = 0;
        if (ok) {
            L.locked = false;
            LOG("lock: opened\n");
        } else {
            int w = text ? atoi(strrchr(text, ' ') ? strrchr(text, ' ') + 1 : "0") : 0;
            if (text && !strncmp(text, "wait", 4))
                snprintf(L.msg, sizeof L.msg, "Too many tries. Wait %d seconds.", w);
            else if (w)
                snprintf(L.msg, sizeof L.msg, "That isn't it. Wait %d seconds before trying again.", w);
            else
                snprintf(L.msg, sizeof L.msg, "That isn't it. Try again.");
            L.msg_at = L.shake_at = A.now;
            LOG("lock: a wrong password\n");
        }
        free(text), text = NULL;
    }
    if (L.set_req > 0 && priv_take(L.set_req, &ok, &text)) {
        L.set_req = 0;
        L.set_result = ok ? 1 : -1;
        snprintf(L.set_why, sizeof L.set_why, "%s", text ? text : "");
        free(text), text = NULL;
        ask_state(); /* set, changed or taken away: what it is now */
    }
}

void lock_boot(void) {
    ask_state();
    for (int i = 0; i < 150 && !L.known && priv_available(); i++) { /* the answer is immediate; at most 1.5 s */
        take_answers();
        if (!L.known) {
            struct timespec nap = {0, 10 * 1000000L};
            nanosleep(&nap, NULL);
        }
    }
    if (L.has_pw) { /* the sign-in */
        L.locked = true;
        L.locked_at = A.now;
        LOG("lock: signing in\n");
    }
}

void lock_now(void) {
    if (L.locked)
        return;
    ask_state();
    L.locked = true;
    L.locked_at = A.now;
    L.pw.buf[0] = 0, L.pw.len = 0;
    L.msg[0] = 0;
    LOG("lock: locked\n");
}

bool lock_active(void) { return L.locked; }
bool lock_has_password(void) { return L.has_pw; }

void lock_password_change(const char *old_pw, const char *new_pw) {
    if (!priv_available()) {
        L.set_result = -1;
        snprintf(L.set_why, sizeof L.set_why, "the system can't keep a password here");
        return;
    }
    char a[200], b[200], arg[420];
    hex(old_pw, a, sizeof a), hex(new_pw, b, sizeof b);
    snprintf(arg, sizeof arg, "password-set %s:%s", a, b);
    L.set_result = 0;
    L.set_req = priv_send(arg);
    memset(a, 0, sizeof a), memset(b, 0, sizeof b), memset(arg, 0, sizeof arg);
}

int lock_password_result(char *why, int n) {
    take_answers();
    int r = L.set_result;
    if (r)
        snprintf(why, (size_t)n, "%s", L.set_why);
    L.set_result = 0;
    return r;
}

/* the time away after which it locks (Settings > Users), in minutes; 0: never */
static void idle_lock(void) {
    if (A.in.clicked || A.in.pressed || A.in.nkeys || A.in.nraw || A.in.wheel || A.in.moved || !L.last_input)
        L.last_input = A.now;
    int after = settings_get_int("lock_after", 0);
    if (after > 0 && A.screen == SCR_DESKTOP && A.now - L.last_input > (uint64_t)after * 60000) {
        LOG("lock: away for %d minutes\n", after);
        lock_now();
    }
}

bool lock_frame(void) {
    take_answers();
    if (!L.locked) {
        idle_lock();
        return false;
    }
    canvas *c = A.frame;
    /* their wallpaper, dimmed, under the clock */
    if (A.wall && A.wall->w == c->w && A.wall->h == c->h)
        memcpy(c->px, A.wall->px, (size_t)c->w * (size_t)c->h * 4);
    else
        fill_rect(c, (rect){0, 0, c->w, c->h}, D.bg | 0xff000000u);
    fill_rect(c, (rect){0, 0, c->w, c->h}, RGBA(0, 0, 0, 110));
    color ink = RGBA(246, 244, 241, 255), soft = RGBA(246, 244, 241, 180);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char hm[16], day[64];
    strftime(hm, sizeof hm, "%H:%M", &tm);
    char wd[16], mon[16];
    strftime(wd, sizeof wd, "%A", &tm);
    strftime(mon, sizeof mon, "%B", &tm);
    snprintf(day, sizeof day, "%s %d %s", wd, tm.tm_mday, mon);
    font *big = fm_host_font(D.font_heading, 104), *body = fm_host_font(D.font_body, 20), *small = fm_host_font(D.font_body, 15);
    int cy = c->h / 2 - 120;
    draw_text_centered(c, big, (rect){0, cy - 70, c->w, 130}, hm, ink);
    draw_text_centered(c, body, (rect){0, cy + 62, c->w, 30}, day, soft);
    /* who it is */
    int py = c->h / 2 + 70;
    const char *who = D.user_name[0] ? D.user_name : account_name();
    /* their picture: a ring with their initial */
    rect av = {c->w / 2 - 28, py - 28, 56, 56};
    fill_round(c, av, 28, color_alpha(D.accent | 0xff000000u, 0.85f));
    char ini[2] = {who[0] >= 'a' && who[0] <= 'z' ? (char)(who[0] - 32) : who[0], 0};
    draw_text_centered(c, fm_host_font(D.font_heading, 26), av, ini, readable_on(D.accent));
    draw_text_centered(c, body, (rect){0, py + 34, c->w, 30}, who, ink);
    int fy = py + 82;
    bool open_now = false;
    if (L.has_pw) {
        int sx = 0; /* a little shake for a wrong one */
        if (L.shake_at && A.now - L.shake_at < 400) {
            float u = (float)(A.now - L.shake_at) / 400.0f;
            sx = (int)(10 * (1 - u) * (((A.now - L.shake_at) / 50) % 2 ? 1 : -1));
        }
        rect f = {c->w / 2 - 170 + sx, fy, 340, 48};
        ui_textfield(f, &L.pw, true, "Password", body, RGBA(255, 255, 255, 34), ink, soft, 24);
        if (ui_type(&L.pw) && L.pw.buf[0] && L.check_req <= 0 && priv_available()) {
            char h[2100], m[2140];
            hex(L.pw.buf, h, sizeof h);
            snprintf(m, sizeof m, "password-check %s", h);
            L.check_req = priv_send(m);
            memset(h, 0, sizeof h), memset(m, 0, sizeof m);
            memset(L.pw.buf, 0, sizeof L.pw.buf), L.pw.len = 0;
            te_reset(&L.pw.ed, L.pw.buf);
        }
        const char *hint = L.check_req > 0 ? "Checking..." : L.msg[0] && A.now - L.msg_at < 8000 ? L.msg : "";
        draw_text_centered(c, small, (rect){0, fy + 60, c->w, 24}, hint, soft);
    } else {
        draw_text_centered(c, small, (rect){0, fy + 12, c->w, 24}, "Press Enter or click to open", soft);
        for (int i = 0; i < A.in.nkeys; i++)
            open_now |= A.in.keys[i] == K_ENTER || A.in.keys[i] == ' ';
        open_now |= A.in.clicked && A.now - L.locked_at > 400;
    }
    /* turning it off or restarting is for anyone at the machine, as on every computer: asked once */
    rect off = {c->w - 150, c->h - 64, 120, 40}, re = {c->w - 280, c->h - 64, 120, 40};
    if (L.confirm) {
        const char *q = L.confirm == 'r' ? "Restart now?" : "Shut down now?";
        draw_text(c, small, re.x - text_width(small, q) - 16, re.y + (40 - font_line_height(small)) / 2, q, ink);
        if (ui_button(re, "Not yet", small, RGBA(255, 255, 255, 30), ink, 20, true))
            L.confirm = 0;
        else if (ui_button(off, "Yes", small, D.accent | 0xff000000u, readable_on(D.accent), 20, true)) {
            alive_shutdown();
            if (priv_available()) {
                priv_call("save", 60000);
                priv_call(L.confirm == 'r' ? "restart" : "power-off", 5000);
            } else {
                store_save();
                sync();
                reboot(L.confirm == 'r' ? RB_AUTOBOOT : RB_POWER_OFF);
            }
        }
    } else {
        if (ui_button(re, "Restart", small, RGBA(255, 255, 255, 30), ink, 20, true))
            L.confirm = 'r';
        if (ui_button(off, "Shut down", small, RGBA(255, 255, 255, 30), ink, 20, true))
            L.confirm = 's';
    }
    if (open_now) {
        L.locked = false;
        LOG("lock: opened\n");
    }
    input_clear(&A.in); /* nothing under it sees a key or a click */
    return true;
}
