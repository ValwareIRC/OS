/* Onboarding's one fixed question: would they like a web browser, and which?
 *
 * Asked while the AI is making their space (so it costs no time), once, and
 * only when Flathub can be reached: its catalog is what lists the browsers,
 * so offline the question simply doesn't come (the launcher offers browsers
 * later, and the OS offers some when a link has nothing to open it). The
 * one they pick installs in the background and is there when they arrive.
 * Each is labelled honestly: made and published by its maker (Flathub
 * checked it), or packaged by volunteers. */
#include "browsers.h"
#include "app.h"
#include "body.h"
#include "launcher.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>

#define D (A.d)

static const struct {
    const char *id, *maker;
} wanted[] = {
    {"org.mozilla.firefox", "Mozilla"},     {"org.chromium.Chromium", "the Chromium project"},
    {"com.google.Chrome", "Google"},        {"com.brave.Browser", "Brave"},
    {"com.vivaldi.Vivaldi", "Vivaldi"},     {"com.opera.Opera", "Opera"},
    {"com.microsoft.Edge", "Microsoft"},
};
#define NWANTED ((int)(sizeof wanted / sizeof wanted[0]))

static struct {
    bool onboarding; /* they are new here (the designing screen was seen) */
    bool done;       /* answered, or nothing to ask */
    bool took;
    input_state held;
    uint64_t shown_at;
} B;

static bool asked_before(void) { return settings_get_int("browser_asked", 0) != 0; }

static void answered(const char *id, const char *name) {
    B.done = true;
    settings_set_int("browser_asked", 1);
    if (id) {
        launcher_install_quietly(id, name);
        LOG("browsers: %s chosen in onboarding\n", id);
    } else {
        LOG("browsers: none for now\n");
    }
}

/* Whether the question is up this frame. */
static bool showing(void) {
    if (B.done)
        return false;
    if (A.screen == SCR_DESIGNING)
        B.onboarding = true;
    if (!B.onboarding || (A.screen != SCR_DESIGNING && A.screen != SCR_DESKTOP) || body_asking())
        return false; /* one question at a time: how it looks comes first */
    if (asked_before()) {
        B.done = true;
        return false;
    }
    launcher_flathub_poll(); /* the catalog, fetched before the desktop exists too */
    if (!launcher_flathub_ready())
        return false; /* not yet (or offline): no question */
    int offered = 0;
    for (int i = 0; i < NWANTED; i++) {
        if (launcher_installed(wanted[i].id)) { /* they have one already */
            B.done = true;
            return false;
        }
        const char *n, *d;
        bool v;
        offered += launcher_flathub_offer(wanted[i].id, &n, &d, &v);
    }
    if (!offered) {
        B.done = true;
        return false;
    }
    if (!B.shown_at) {
        B.shown_at = A.now;
        LOG("browsers: asking which browser they'd like\n");
    }
    return true;
}

void browsers_take_input(void) {
    B.took = showing();
    if (!B.took)
        return;
    B.held = A.in;
    A.in.clicked = A.in.pressed = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.nkeys = 0;
    A.in.nraw = 0;
    A.in.mx = A.in.my = -10000;
}

void browsers_draw(void) {
    if (!B.took)
        return;
    A.in = B.held;
    B.took = false;
    /* in onboarding's own colours before the desktop exists, the design's after */
    bool desk = A.screen == SCR_DESKTOP;
    color panel = desk ? (D.surface | 0xff000000u) : RGBA(24, 18, 22, 255);
    color ink = desk ? D.surface_text : RGBA(242, 233, 226, 255);
    color muted = desk ? D.muted : RGBA(168, 150, 142, 255);
    color accent = desk ? D.accent : (A.prof.has_palette ? A.prof.palette[1] : RGBA(232, 131, 107, 255));
    float e = (float)(A.now - B.shown_at) / 220.0f;
    e = e > 1 ? 1 : e;
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), 0.35f * e));

    font *ft = font_get(desk ? D.font_heading : "Fraunces", 26), *fb = font_get("Inter", 16), *fs = font_get("Inter", 13);
    int cols = 2, tile_w = 330, tile_h = 74, gap = 12;
    int n = 0;
    for (int i = 0; i < NWANTED; i++) {
        const char *nm, *dv;
        bool v;
        n += launcher_flathub_offer(wanted[i].id, &nm, &dv, &v);
    }
    int rows = (n + cols - 1) / cols;
    int w = cols * tile_w + (cols - 1) * gap + 56, h = 128 + rows * (tile_h + gap) + 70;
    rect r = {(A.W - w) / 2, (A.H - h) / 2 + (int)((1 - e) * 16), w, h};
    shadow_round(A.frame, (rect){r.x, r.y + 16, r.w, r.h}, 22, 48, RGBA(0, 0, 0, 110));
    fill_round(A.frame, r, 22, panel);
    stroke_round(A.frame, r, 22, 1, color_alpha(ink, 0.12f));
    draw_text(A.frame, ft, r.x + 28, r.y + 24, "Would you like a web browser?", ink);
    draw_text(A.frame, fs, r.x + 28, r.y + 30 + font_line_height(ft),
              "It downloads in the background while your desktop is set up.", muted);

    int k = 0;
    for (int i = 0; i < NWANTED; i++) {
        const char *name, *dev;
        bool verified;
        if (!launcher_flathub_offer(wanted[i].id, &name, &dev, &verified))
            continue;
        rect t = {r.x + 28 + (k % cols) * (tile_w + gap), r.y + 100 + (k / cols) * (tile_h + gap), tile_w, tile_h};
        k++;
        bool over = pt_in(t, A.in.mx, A.in.my);
        fill_round(A.frame, t, 14, color_alpha(over ? accent : ink, over ? 0.22f : 0.06f));
        draw_text(A.frame, fb, t.x + 16, t.y + 12, name, ink);
        char line[160];
        if (verified)
            snprintf(line, sizeof line, "From %s, checked by Flathub", dev[0] ? dev : wanted[i].maker);
        else
            snprintf(line, sizeof line, "Packaged by volunteers, not by %s", wanted[i].maker);
        draw_text(A.frame, fs, t.x + 16, t.y + 18 + font_line_height(fb), line, muted);
        if (over && A.in.clicked) {
            answered(wanted[i].id, name);
            return;
        }
    }
    rect no = {r.x + r.w - 28 - 120, r.y + r.h - 58, 120, 40};
    if (ui_button(no, "Not now", fb, color_alpha(ink, 0.08f), ink, 12, true))
        answered(NULL, NULL);
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            answered(NULL, NULL);
}
