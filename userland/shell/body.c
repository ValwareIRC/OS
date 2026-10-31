/* How the companion looks, chosen by the person: once in onboarding (while
 * the AI makes their space), any time in Settings > Mind, or by asking the
 * companion. A gallery of living previews: the built-in emblems (a gem, a
 * light, an 8-ball, the Earth, a ringed planet, a heart, the moon), the face
 * the OS makes, any VRM model of theirs on this computer, and "something
 * else" (a picture, a link, a 3D model, or a description), which the
 * companion takes from them in conversation. A model of theirs is shown as
 * it was made. */
#include "body.h"
#include "app.h"
#include "emblem.h"
#include "files.h"
#include "presence.h"
#include "settings.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define D (A.d)
#define MAX_FOUND 4

static struct {
    bool onboarding, done, took;
    input_state held;
    uint64_t shown_at;
    char found[MAX_FOUND][300];
    int nfound;
    uint64_t looked_at;
} Q;

/* .vrm files where people keep things: home, Downloads, Desktop, Documents, Pictures. */
int body_find_vrms(char out[][300], int max) {
    static const char *const dirs[] = {HOME_DIR, HOME_DIR "/Downloads", HOME_DIR "/Desktop", HOME_DIR "/Documents",
                                       HOME_DIR "/Pictures"};
    int n = 0;
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0] && n < max; d++) {
        DIR *dir = opendir(dirs[d]);
        struct dirent *e;
        while (dir && (e = readdir(dir)) && n < max) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcasecmp(e->d_name + l - 4, ".vrm") && e->d_name[0] != '.')
                snprintf(out[n++], 300, "%s/%s", dirs[d], e->d_name);
        }
        if (dir)
            closedir(dir);
    }
    return n;
}

/* ---- the gallery ---- */

enum { T_EMBLEM, T_FACE, T_VRM, T_ELSE };
struct tile {
    int kind, emblem;
    const char *vrm;
    const char *title, *line;
};

static const char *const emblem_line[EM_COUNT] = {
    "A cut gem, turning slowly in the light",  "A soft glow that breathes and listens",
    "A magic 8-ball, thinking it over",        "The Earth, clouds and all",
    "A ringed planet in the design's colours", "A heart that beats when it speaks",
    "The moon, quiet and silver"};
static const char *const emblem_title[EM_COUNT] = {"A gem", "A light", "An 8-ball", "The Earth",
                                                   "A planet", "A heart", "The moon"};

static int tiles(struct tile *t, char found[][300], int nfound) {
    int n = 0;
    for (int k = 0; k < EM_COUNT; k++)
        t[n++] = (struct tile){T_EMBLEM, k, NULL, emblem_title[k], emblem_line[k]};
    t[n++] = (struct tile){T_FACE, 0, NULL, "A face", "An anime-style figure who looks at you and talks"};
    for (int i = 0; i < nfound; i++) {
        const char *name = strrchr(found[i], '/');
        t[n++] = (struct tile){T_VRM, 0, found[i], name ? name + 1 : found[i], "Your own model, just as it was made"};
    }
    t[n++] = (struct tile){T_ELSE, 0, NULL, "Something else",
                           "A picture, a link, a 3D model, or describe it: tell me"};
    return n;
}

/* Which tile is how it looks now. */
static bool current(const struct tile *t) {
    bool face = presence_mode() != PRESENCE_LIGHT;
    bool own = access(STATE_DIR "/figure.vrm", R_OK) == 0;
    if (t->kind == T_EMBLEM)
        return !face && emblem_kind() == t->emblem;
    if (t->kind == T_FACE)
        return face && !own;
    return false;
}

static void choose(const struct tile *t) {
    if (t->kind == T_EMBLEM) {
        presence_set_mode(PRESENCE_LIGHT);
        emblem_set(t->emblem);
        LOG("body: %s\n", emblem_name(t->emblem));
    } else if (t->kind == T_FACE) {
        presence_set_body(NULL);
        LOG("body: the face the OS makes\n");
    } else if (t->kind == T_VRM) {
        if (!presence_set_body(t->vrm))
            toast("That model couldn't be read.");
    } else {
        LOG("body: something else, in conversation\n");
        toast("Tell me what you'd like me to be: a picture, a link, a 3D model, or just describe it.");
    }
}

/* The gallery in r (cols across): each tile a living preview. Returns its height. */
int body_gallery(rect r, int cols, bool desk, bool *picked) {
    char found[MAX_FOUND][300];
    static uint64_t looked;
    static char cache[MAX_FOUND][300];
    static int ncache;
    if (!looked || A.now - looked > 3000) {
        looked = A.now;
        ncache = body_find_vrms(cache, MAX_FOUND);
    }
    memcpy(found, cache, sizeof found);
    struct tile t[EM_COUNT + 2 + MAX_FOUND];
    int n = tiles(t, found, ncache);
    color ink = desk ? D.surface_text : RGBA(31, 34, 64, 255);
    color muted = desk ? D.muted : RGBA(107, 111, 147, 255);
    color accent = desk ? D.accent : RGBA(124, 108, 255, 255);
    color accent2 = desk ? D.accent2 : RGBA(255, 143, 184, 255);
    color card = desk ? color_alpha(D.surface_text, 0.05f) : RGBA(255, 255, 255, 255);
    font *fb = font_get_styled("Outfit", 15, 600, false, 0), *fs = font_get("Manrope", 12);
    int gap = 12, tw = (r.w - (cols - 1) * gap) / cols, th = 150;
    for (int i = 0; i < n; i++) {
        rect c = {r.x + (i % cols) * (tw + gap), r.y + (i / cols) * (th + gap), tw, th};
        bool over = pt_in(c, A.in.mx, A.in.my), now = current(&t[i]);
        if (!desk)
            shadow_round(A.frame, (rect){c.x, c.y + 8, c.w, c.h}, 16, 20, RGBA(60, 50, 140, over ? 50 : 24));
        fill_round(A.frame, c, 16, card);
        if (over || now)
            stroke_round(A.frame, c, 16, 2, now ? accent : color_alpha(accent, 0.6f));
        float cx = c.x + c.w / 2.0f, cy = c.y + 52;
        if (t[i].kind == T_EMBLEM) {
            emblem_draw_kind(A.frame, t[i].emblem, cx, cy, 30, accent, accent2, over ? ES_THINKING : ES_IDLE, 0);
        } else if (t[i].kind == T_FACE || t[i].kind == T_VRM) { /* a little bust: head and shoulders */
            fill_round(A.frame, (rect){(int)cx - 26, (int)cy + 10, 52, 30}, 14, color_alpha(accent, 0.35f));
            fill_circle(A.frame, cx, cy - 6, 18, color_alpha(accent2, 0.55f));
            fill_circle(A.frame, cx - 6, cy - 8, 2.5f, ink);
            fill_circle(A.frame, cx + 6, cy - 8, 2.5f, ink);
        } else { /* something else: a sparkle */
            for (int k = 0; k < 4; k++) {
                float a = k * 3.14159f / 2 + (over ? (float)A.now / 900.0f : 0);
                draw_line(A.frame, cx, cy, cx + cosf(a) * 22, cy + sinf(a) * 22, 3, color_alpha(accent, 0.8f));
            }
            fill_circle(A.frame, cx, cy, 7, accent2);
        }
        draw_text_centered(A.frame, fb, (rect){c.x + 6, c.y + 92, c.w - 12, 22}, t[i].title, ink);
        draw_text_wrapped(A.frame, fs, (rect){c.x + 12, c.y + 114, c.w - 24, 34}, t[i].line, muted, 1.15f);
        if (now)
            draw_text(A.frame, fs, c.x + 10, c.y + 8, "now", accent);
        if (over && A.in.clicked) {
            choose(&t[i]);
            if (picked)
                *picked = true;
        }
    }
    return ((n + cols - 1) / cols) * (th + gap) - gap;
}

/* ---- the onboarding question ---- */

static bool showing(void) {
    if (Q.done)
        return false;
    if (A.screen == SCR_DESIGNING)
        Q.onboarding = true;
    if (!Q.onboarding || (A.screen != SCR_DESIGNING && A.screen != SCR_DESKTOP))
        return false;
    if (settings_get_int("body_asked", 0)) {
        Q.done = true;
        return false;
    }
    if (!Q.shown_at) {
        Q.shown_at = A.now;
        LOG("body: asking how she should look\n");
    }
    return true;
}

bool body_asking(void) { return !Q.done && Q.shown_at; }

void body_take_input(void) {
    Q.took = showing();
    if (!Q.took)
        return;
    Q.held = A.in;
    A.in.clicked = A.in.pressed = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.nkeys = 0;
    A.in.nraw = 0;
    A.in.mx = A.in.my = -10000;
}

void body_draw(void) {
    if (!Q.took)
        return;
    A.in = Q.held;
    Q.took = false;
    bool desk = A.screen == SCR_DESKTOP;
    color panel = desk ? (D.surface | 0xff000000u) : RGBA(247, 246, 255, 255);
    color ink = desk ? D.surface_text : RGBA(31, 34, 64, 255);
    color muted = desk ? D.muted : RGBA(107, 111, 147, 255);
    float e = (float)(A.now - Q.shown_at) / 220.0f;
    e = e > 1 ? 1 : e;
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(40, 34, 90, 255), 0.30f * e));
    int cols = 5, w = cols * 168 + (cols - 1) * 12 + 64;
    if (w > A.W - 60)
        w = A.W - 60;
    int rows = (EM_COUNT + 2 + Q.nfound + cols - 1) / cols;
    int h = 118 + rows * 162 + 30;
    rect r = {(A.W - w) / 2, (A.H - h) / 2 + (int)((1 - e) * 16), w, h};
    shadow_round(A.frame, (rect){r.x, r.y + 18, r.w, r.h}, 28, 56, RGBA(40, 30, 110, 90));
    fill_round(A.frame, r, 28, panel);
    char title[96];
    snprintf(title, sizeof title, "How should %s look?", A.prof.ai_name[0] ? A.prof.ai_name : "your companion");
    font *ft = font_get_styled("Outfit", 28, 600, false, 0), *fs = font_get("Manrope", 14);
    draw_text(A.frame, ft, r.x + 32, r.y + 26, title, ink);
    draw_text(A.frame, fs, r.x + 32, r.y + 34 + font_line_height(ft),
              "Pick one to begin. You can change it any time, in Settings or just by asking.", muted);
    bool picked = false;
    body_gallery((rect){r.x + 32, r.y + 104, r.w - 64, r.h - 130}, cols, desk, &picked);
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            picked = true;
    if (picked) {
        Q.done = true;
        settings_set_int("body_asked", 1);
    }
}
