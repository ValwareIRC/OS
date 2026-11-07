/* How the companion looks: nothing is offered from a list. Once their space
 * is made, it brings it up itself in conversation, suggesting looks made
 * up to belong in that design, and makes the one they want (a drawing of
 * itself, a picture, a 3D model, a face, a VRM of theirs). Settings opens
 * the same conversation. Pictures and models come from the web here too. */
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
    bool onboarding, done;
    uint64_t shown_at; /* the desktop first came up after onboarding */
} Q;

/* .vrm files where people keep things: home, Downloads, Desktop, Documents, Pictures. */
int body_find_vrms(char out[][300], int max) {
    const char *const dirs[] = {HOME_DIR, home_path("/Downloads"), home_path("/Desktop"), home_path("/Documents"),
                                       home_path("/Pictures")};
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

/* ---- talking about it ---- */

void body_talk_about_it(bool first) {
    char vrms[MAX_FOUND][300];
    int nv = body_find_vrms(vrms, MAX_FOUND);
    sbuf b;
    sb_init(&b);
    if (first)
        sb_add(&b, "(Their space is ready and they're looking at it now. You have no look of your own yet: nothing of "
                   "you shows on their desktop. ");
    else
        sb_add(&b, "(They opened Settings to talk about how you look. ");
    sb_add(&b, "Bring it up the way a friend would, in a line or two: suggest one or two looks you could take that "
               "would belong in this design, made up for it (not a list of options), and ask what they'd like to see. "
               "They might describe something, give you a picture, a link or a 3D model, or ask for a face. Make "
               "it once they've answered, or straight away if they tell you to choose. ");
    if (nv) {
        sb_add(&b, "They have VRM avatars on this computer you could mention:");
        for (int i = 0; i < nv; i++)
            sb_printf(&b, " %s", vrms[i]);
        sb_add(&b, ". ");
    }
    sb_add(&b, "Don't mention this note.)");
    companion_note(b.p, true);
    free(b.p);
}

/* ---- after onboarding: it brings it up itself, once their space is there ---- */

static void bring_it_up(void) {
    if (Q.done)
        return;
    if (A.screen == SCR_DESIGNING)
        Q.onboarding = true;
    if (!Q.onboarding || A.screen != SCR_DESKTOP)
        return;
    if (settings_get_int("body_asked", 0)) {
        Q.done = true;
        return;
    }
    if (!Q.shown_at)
        Q.shown_at = A.now;
    if (A.now - Q.shown_at < 4000 || A.companion.pending) /* a moment to take their space in first */
        return;
    if (desktop_window_in_use()) /* (not over something they are using: when they are back) */
        return;
    Q.done = true;
    settings_set_int("body_asked", 1);
    LOG("body: asking how it should look\n");
    desktop_open_window(WIN_COMPANION);
    body_talk_about_it(true);
}

bool body_asking(void) { return false; }

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

/* starts like the file it should be (a glTF model's magic, or not a web page) */
static bool looks_right(const char *path, char kind) {
    char head[16] = {0};
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    if (kind == 'm')
        return n >= 4 && !memcmp(head, "glTF", 4);
    return n > 0 && head[0] != '<';
}

static void fetch_child(void *arg, char **result) {
    const char *url = arg;
    const char *path = F.fetch_kind == 'm' ? "/tmp/emblem-fetch.glb" : "/tmp/emblem-fetch.img";
    size_t cap = F.fetch_kind == 'm' ? (200u << 20) : (25u << 20);
    bool ok = web_download(url, path, cap) && looks_right(path, F.fetch_kind);
    /* a sharing site's page instead of the file: the usual ways such sites hand the file itself over */
    static const char *const ask[] = {"download=true", "download=1", "dl=1", "raw=1", NULL};
    for (int i = 0; !ok && ask[i]; i++) {
        char again[1200];
        snprintf(again, sizeof again, "%s%c%s", url, strchr(url, '?') ? '&' : '?', ask[i]);
        ok = web_download(again, path, cap) && looks_right(path, F.fetch_kind);
    }
    *result = strdup(ok ? path : "");
}

/* a VRM is a person's own avatar: it becomes their companion's face, not an object on a turntable */
static bool is_vrm(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    static char json[1 << 16];
    size_t n = fread(json, 1, sizeof json - 1, f);
    fclose(f);
    json[n] = 0;
    for (size_t i = 20; i + 8 < n; i++) /* the JSON chunk, after the 20-byte headers */
        if (!memcmp(json + i, "\"VRM\"", 5) || !memcmp(json + i, "\"VRMC_vrm\"", 10))
            return true;
    return false;
}

static bool take_model(const char *path) {
    if (is_vrm(path))
        return presence_set_body(path);
    if (!emblem_set_model(path))
        return false;
    presence_set_mode(PRESENCE_LIGHT);
    return true;
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
        else if (!take_model(what))
            toast("That model couldn't be read.");
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
                  (F.fetch_kind == 'm' ? take_model(res) : emblem_set_picture(res));
        if (ok && F.fetch_kind != 'm')
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
    draw_text(A.frame, font_get("Manrope", 14), r.x + 32, r.y + 66, "Choose one to use it, or press Esc to cancel.",
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
            toast("Applying...");
            return;
        }
    }
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            F.open = false;
}

void body_take_input(void) {
    body_poll();
    bring_it_up();
    F.took = F.open;
    if (F.took) {
        F.held = A.in;
        A.in.clicked = A.in.pressed = A.in.right_clicked = false;
        A.in.wheel = 0;
        A.in.nkeys = 0;
        A.in.nraw = 0;
        A.in.mx = A.in.my = -10000;
    }
}

void body_draw(void) {
    if (F.took) { /* the pictures it found, over everything */
        A.in = F.held;
        F.took = false;
        finder_draw();
    }
}
