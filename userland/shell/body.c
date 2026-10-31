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
#include "image.h"
#include "web.h"
#include "../lib/json.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- pictures and models from the web, chosen for its look ---- */

#define FIND_MAX 8
static struct {
    job *fetch, *find;          /* a picture or model being fetched; a search for pictures */
    char fetch_kind;            /* 'p' picture, 'm' model */
    bool open;                  /* the thumbnails are up to choose from */
    char query[160];
    int n;
    char urls[FIND_MAX][512], titles[FIND_MAX][160];
    image *thumbs[FIND_MAX];
    uint64_t opened;
    bool took;
    input_state held;
} F;

static void fetch_child(void *arg, char **result) {
    const char *url = arg;
    const char *path = F.fetch_kind == 'm' ? "/tmp/emblem-fetch.glb" : "/tmp/emblem-fetch.img";
    *result = strdup(web_download(url, path, F.fetch_kind == 'm' ? (200u << 20) : (25u << 20)) ? path : "");
}

static char fetch_url[1024];

static void fetch(const char *url, char kind) {
    if (F.fetch)
        return;
    snprintf(fetch_url, sizeof fetch_url, "%s", url);
    F.fetch_kind = kind;
    F.fetch = job_start(fetch_child, fetch_url);
    emblem_busy_hint = true;
    LOG("body: fetching %s\n", url);
}

static void find_child(void *arg, char **result) {
    char urls[FIND_MAX][512], thumbs[FIND_MAX][512], titles[FIND_MAX][160];
    int n = web_images((const char *)arg, urls, thumbs, titles, FIND_MAX);
    sbuf out;
    sb_init(&out);
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof path, "/tmp/emblem-find-%d", i);
        if (!web_download(thumbs[i], path, 4u << 20))
            continue;
        for (char *p = titles[i]; *p; p++)
            if (*p == '\t' || *p == '\n')
                *p = ' ';
        sb_printf(&out, "%s\t%s\t%s\n", path, urls[i], titles[i]);
    }
    *result = out.p ? out.p : strdup("");
}

void body_use(const char *how, const char *what) {
    while (*what == ' ')
        what++;
    if (!strcmp(how, "picture")) {
        if (!strncmp(what, "http://", 7) || !strncmp(what, "https://", 8))
            fetch(what, 'p');
        else if (!emblem_set_picture(what))
            toast("That picture couldn't be read.");
        else
            presence_set_mode(PRESENCE_LIGHT);
    } else if (!strcmp(how, "model")) {
        if (!strncmp(what, "http://", 7) || !strncmp(what, "https://", 8))
            fetch(what, 'm');
        else if (!emblem_set_model(what))
            toast("That model couldn't be read.");
        else
            presence_set_mode(PRESENCE_LIGHT);
    } else if (!strcmp(how, "find") && !F.find) {
        snprintf(F.query, sizeof F.query, "%s", what);
        F.find = job_start(find_child, F.query);
        emblem_busy_hint = true;
        LOG("body: looking for pictures of %s\n", what);
    }
}

static void body_poll(void) {
    char *res = NULL;
    if (F.fetch && job_poll(F.fetch, &res)) {
        job_free(F.fetch);
        F.fetch = NULL;
        bool ok = res && res[0] &&
                  (F.fetch_kind == 'm' ? emblem_set_model(res) : emblem_set_picture(res));
        if (ok)
            presence_set_mode(PRESENCE_LIGHT);
        else
            toast(F.fetch_kind == 'm' ? "That model couldn't be fetched or read." : "That picture couldn't be fetched.");
        LOG("body: fetched %s: %s\n", fetch_url, ok ? "in use" : "failed");
        free(res);
    }
    if (F.find && job_poll(F.find, &res)) {
        job_free(F.find);
        F.find = NULL;
        for (int i = 0; i < F.n; i++)
            image_free(F.thumbs[i]);
        F.n = 0;
        for (char *line = res ? strtok(res, "\n") : NULL; line && F.n < FIND_MAX; line = strtok(NULL, "\n")) {
            char *u = strchr(line, '\t'), *t = u ? strchr(u + 1, '\t') : NULL;
            if (!u || !t)
                continue;
            *u++ = 0, *t++ = 0;
            image *im = image_load(line, 220);
            if (!im)
                continue;
            F.thumbs[F.n] = im;
            snprintf(F.urls[F.n], sizeof F.urls[0], "%s", u);
            snprintf(F.titles[F.n], sizeof F.titles[0], "%s", t);
            F.n++;
        }
        free(res);
        LOG("body: %d pictures of %s to choose from\n", F.n, F.query);
        if (F.n) {
            F.open = true;
            F.opened = A.now;
        } else {
            toast("I couldn't find pictures of that. Try other words, or paste a link.");
        }
    }
}

/* The pictures it found: click one to become it. */
static void finder_draw(void) {
    bool desk = A.screen == SCR_DESKTOP;
    color panel = desk ? (D.surface | 0xff000000u) : RGBA(247, 246, 255, 255);
    color ink = desk ? D.surface_text : RGBA(31, 34, 64, 255), muted = desk ? D.muted : RGBA(107, 111, 147, 255);
    color accent = desk ? D.accent : RGBA(124, 108, 255, 255);
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, RGBA(40, 34, 90, 70));
    int cols = 4, tw = 200, gap = 14, rows = (F.n + cols - 1) / cols;
    int w = cols * tw + (cols - 1) * gap + 64, h = 120 + rows * (tw + gap) + 50;
    rect r = {(A.W - w) / 2, (A.H - h) / 2, w, h};
    shadow_round(A.frame, (rect){r.x, r.y + 18, r.w, r.h}, 28, 56, RGBA(40, 30, 110, 90));
    fill_round(A.frame, r, 28, panel);
    char title[220];
    snprintf(title, sizeof title, "Pictures of %s", F.query);
    draw_text(A.frame, font_get_styled("Outfit", 26, 600, false, 0), r.x + 32, r.y + 26, title, ink);
    draw_text(A.frame, font_get("Manrope", 14), r.x + 32, r.y + 66, "Click one and I'll take it on. Esc for none.",
              muted);
    for (int i = 0; i < F.n; i++) {
        rect c = {r.x + 32 + (i % cols) * (tw + gap), r.y + 104 + (i / cols) * (tw + gap), tw, tw};
        bool over = pt_in(c, A.in.mx, A.in.my);
        fill_round(A.frame, c, 18, color_alpha(ink, 0.06f));
        image *im = F.thumbs[i];
        canvas_clip(A.frame, rect_inset(c, 6));
        image_draw(A.frame, im, c.x + (c.w - im->w) / 2, c.y + (c.h - im->h) / 2, 1);
        canvas_unclip(A.frame);
        if (over)
            stroke_round(A.frame, c, 18, 3, accent);
        if (over && A.in.clicked) {
            fetch(F.urls[i], 'p');
            F.open = false;
            toast("Taking it on...");
            return;
        }
    }
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            F.open = false;
}

void body_take_input(void) {
    body_poll();
    F.took = F.open;
    if (F.took) {
        F.held = A.in;
        A.in.clicked = A.in.pressed = A.in.right_clicked = false;
        A.in.wheel = 0;
        A.in.nkeys = 0;
        A.in.nraw = 0;
        A.in.mx = A.in.my = -10000;
    }
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

static void question_draw(void);

void body_draw(void) {
    if (Q.took)
        question_draw();
    if (F.took) { /* the pictures it found, over everything */
        A.in = F.held;
        F.took = false;
        finder_draw();
    }
}

static void question_draw(void) {
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
