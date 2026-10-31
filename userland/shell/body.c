/* Onboarding's question about the companion's body: a light (the orb), the
 * face the OS makes for her, or a VRM model of their own that is already on
 * this computer (any .vrm in their home folder or its usual folders). Asked
 * once, while the AI makes their space; Settings > Mind can change it later.
 * A model of theirs is shown as it is, never altered. */
#include "body.h"
#include "app.h"
#include "presence.h"
#include "files.h"
#include "settings.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define D (A.d)
#define MAX_FOUND 6

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

static void answered(int choice, const char *vrm) {
    Q.done = true;
    settings_set_int("body_asked", 1);
    if (choice == 0) {
        presence_set_mode(PRESENCE_LIGHT);
        LOG("body: a light\n");
    } else if (choice == 1) {
        presence_set_body(NULL);
        LOG("body: the face the OS makes\n");
    } else if (!presence_set_body(vrm)) {
        toast("That model couldn't be read. Try another in Settings.");
    }
}

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
    if (!Q.looked_at || A.now - Q.looked_at > 3000) { /* a file they put there just now shows up */
        Q.looked_at = A.now;
        Q.nfound = body_find_vrms(Q.found, MAX_FOUND);
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

static bool tile(rect t, const char *title, const char *line, font *fb, font *fs, color ink, color muted, color accent) {
    bool over = pt_in(t, A.in.mx, A.in.my);
    fill_round(A.frame, t, 14, color_alpha(over ? accent : ink, over ? 0.22f : 0.06f));
    draw_text(A.frame, fb, t.x + 16, t.y + 12, title, ink);
    draw_text(A.frame, fs, t.x + 16, t.y + 18 + font_line_height(fb), line, muted);
    return over && A.in.clicked;
}

void body_draw(void) {
    if (!Q.took)
        return;
    A.in = Q.held;
    Q.took = false;
    bool desk = A.screen == SCR_DESKTOP;
    color panel = desk ? (D.surface | 0xff000000u) : RGBA(24, 18, 22, 255);
    color ink = desk ? D.surface_text : RGBA(242, 233, 226, 255);
    color muted = desk ? D.muted : RGBA(168, 150, 142, 255);
    color accent = desk ? D.accent : (A.prof.has_palette ? A.prof.palette[1] : RGBA(232, 131, 107, 255));
    float e = (float)(A.now - Q.shown_at) / 220.0f;
    e = e > 1 ? 1 : e;
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), 0.35f * e));

    font *ft = font_get(desk ? D.font_heading : "Fraunces", 26), *fb = font_get("Inter", 16), *fs = font_get("Inter", 13);
    int tile_w = 560, tile_h = 70, gap = 10, n = 2 + Q.nfound;
    int w = tile_w + 56, h = 116 + n * (tile_h + gap) + (Q.nfound ? 0 : 44) + 24;
    rect r = {(A.W - w) / 2, (A.H - h) / 2 + (int)((1 - e) * 16), w, h};
    shadow_round(A.frame, (rect){r.x, r.y + 16, r.w, r.h}, 22, 48, RGBA(0, 0, 0, 110));
    fill_round(A.frame, r, 22, panel);
    stroke_round(A.frame, r, 22, 1, color_alpha(ink, 0.12f));
    char title[96];
    snprintf(title, sizeof title, "How should %s look?", A.prof.ai_name[0] ? A.prof.ai_name : "your companion");
    draw_text(A.frame, ft, r.x + 28, r.y + 24, title, ink);
    draw_text(A.frame, fs, r.x + 28, r.y + 30 + font_line_height(ft), "You can change it any time in Settings.", muted);

    int y = r.y + 96;
    if (tile((rect){r.x + 28, y, tile_w, tile_h}, "A light", "A soft glow that breathes and listens, like in Her.", fb,
             fs, ink, muted, accent)) {
        answered(0, NULL);
        return;
    }
    y += tile_h + gap;
    if (tile((rect){r.x + 28, y, tile_w, tile_h}, "A face",
             "An anime-style figure in a round frame, who looks at you and talks.", fb, fs, ink, muted, accent)) {
        answered(1, NULL);
        return;
    }
    y += tile_h + gap;
    for (int i = 0; i < Q.nfound; i++, y += tile_h + gap) {
        const char *name = strrchr(Q.found[i], '/');
        char t[160];
        snprintf(t, sizeof t, "Your model: %s", name ? name + 1 : Q.found[i]);
        if (tile((rect){r.x + 28, y, tile_w, tile_h}, t, "A VRM model of yours, shown just as it was made.", fb, fs, ink,
                 muted, accent)) {
            answered(2, Q.found[i]);
            return;
        }
    }
    if (!Q.nfound)
        draw_text_wrapped(A.frame, fs, (rect){r.x + 28, y + 2, tile_w, 44},
                          "Have a VRM model (from VRoid Studio, say)? Put the .vrm file in your Downloads folder and it "
                          "shows here.",
                          muted, 1.3f);
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            answered(0, NULL);
}
