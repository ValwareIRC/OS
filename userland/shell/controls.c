#include "controls.h"
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The tray's panels and Settings share these controls. They take every
 * colour from the design (accent, surface text, muted) and check it against
 * what is really painted behind them, because a design's surface may be
 * glass over a busy wallpaper rather than the flat colour it names. */

#define D (A.d)

color pal_sample(rect r) {
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 8; i++) {
            int x = r.x + r.w * (2 * i + 1) / 16, y = r.y + r.h * (2 * j + 1) / 8;
            if (x < 0 || y < 0 || x >= A.frame->w || y >= A.frame->h)
                continue;
            uint32_t p = A.frame->px[(size_t)y * (size_t)A.frame->w + (size_t)x];
            sr += C_R(p), sg += C_G(p), sb += C_B(p), n++;
        }
    return n ? RGBA(sr / n, sg / n, sb / n, 255) : D.surface;
}

/* The nearest colour to `want` (same hue) that reaches `need` on `bg`. */
static color reach(color want, color bg, float need) {
    want |= 0xff000000u;
    if (color_contrast(want, bg) >= need)
        return want;
    color target = color_luma(bg) < 0.45f ? RGBA(255, 255, 255, 255) : RGBA(0, 0, 0, 255);
    for (int k = 1; k <= 10; k++) {
        color t = color_mix(want, target, k * 0.1f);
        if (color_contrast(t, bg) >= need)
            return t;
    }
    return target;
}

palette pal_on(color under, float op) {
    palette p;
    under |= 0xff000000u;
    p.under = under;
    p.ink = reach(D.surface_text, under, 7.0f);
    p.muted = reach(D.muted, under, 3.6f);
    /* the accent fills switches and selections: it has to stand out from the panel */
    p.accent = color_contrast(D.accent | 0xff000000u, under) >= 2.2f ? D.accent | 0xff000000u : reach(D.accent, under, 2.2f);
    p.on_accent = readable_on(p.accent);
    p.line = color_alpha(p.ink, 0.12f);
    p.op = op;
    return p;
}

void ctl_panel(rect r, float radius, float op) {
    if (op <= 0.01f || r.w <= 0 || r.h <= 0)
        return;
    bool dark = color_luma(D.surface) < 0.3f;
    shadow_round(A.frame, (rect){r.x, r.y + 12, r.w, r.h}, radius, 40, RGBA(0, 0, 0, (int)((dark ? 120 : 70) * op)));
    if (D.material == MAT_NEON)
        glow_round(A.frame, r, radius, 10 + 22 * D.glow, color_alpha(D.accent, (0.16f + 0.26f * D.glow) * op));
    /* Floating panels are read, so they are always more opaque than the
     * design's surfaces: glass stays glass, but the words come first. */
    float o = D.surface_opacity < 0.78f ? 0.78f : D.surface_opacity;
    if (D.material != MAT_SOLID && D.blur >= 1 && op > 0.3f) {
        blur_fast(A.frame, r, radius, (int)(D.blur * 0.9f) + 6);
        vibrancy(A.frame, r, radius, D.material == MAT_FROST ? 1.1f : 1.35f, dark ? -4 : 6);
    } else {
        o = 1;
    }
    if (D.material == MAT_FROST)
        o = o + 0.1f > 1 ? 1 : o + 0.1f;
    color top = color_mix(D.surface, RGBA(255, 255, 255, 255), dark ? 0.07f : 0.2f);
    fill_round_vgradient(A.frame, r, radius, color_alpha(top, o * op), color_alpha(D.surface, o * op));
    stroke_round_vgradient(A.frame, r, radius, 1, RGBA(255, 255, 255, (int)((dark ? 64 : 150) * op)),
                           RGBA(255, 255, 255, (int)((dark ? 8 : 30) * op)));
    if (D.material == MAT_NEON)
        stroke_round(A.frame, r, radius, 1.3f, color_alpha(D.accent, 0.8f * op));
    else
        stroke_round(A.frame, r, radius, 1, RGBA(0, 0, 0, (int)((dark ? 60 : 22) * op)));
}

/* ---- slider ---- */

static int drag_id = -1;

bool ctl_dragging(void) { return drag_id >= 0 && A.in.down; }
int ctl_drag_id(void) { return drag_id; }

int ctl_slider(int id, rect r, float *v, const palette *p) {
    float t = *v < 0 ? 0 : *v > 1 ? 1 : *v;
    int knob = r.h < 22 ? r.h : 22;
    float x0 = r.x + knob / 2.0f, x1 = r.x + r.w - knob / 2.0f, cy = r.y + r.h / 2.0f;
    rect hit = {r.x - 4, r.y - 6, r.w + 8, r.h + 12};
    bool hover = ui_hover(hit);
    int res = CTL_IDLE;
    if (A.in.pressed && hover)
        drag_id = id;
    if (drag_id == id) {
        if (A.in.down) {
            float nt = (A.in.mx - x0) / (x1 - x0);
            nt = nt < 0 ? 0 : nt > 1 ? 1 : nt;
            if (nt != t) {
                t = nt;
                res = CTL_CHANGING;
            }
        } else {
            drag_id = -1;
            res = CTL_RELEASED;
        }
    } else if (hover && A.in.wheel) {
        t += A.in.wheel * 0.05f;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        res = CTL_RELEASED;
    }
    *v = t;
    float th = 6;
    rect track = {r.x, (int)(cy - th / 2), r.w, (int)th};
    fill_round(A.frame, track, th / 2, color_alpha(p->ink, 0.16f * p->op));
    float kx = x0 + (x1 - x0) * t;
    rect fillr = {r.x, track.y, (int)(kx - r.x), track.h};
    if (fillr.w > 0)
        fill_round(A.frame, fillr, th / 2, color_alpha(p->accent, p->op));
    bool active = drag_id == id;
    float kr = knob / 2.0f - (active ? 0 : 1);
    shadow_round(A.frame, (rect){(int)(kx - kr), (int)(cy - kr + 2), (int)(2 * kr), (int)(2 * kr)}, kr, 6,
                 RGBA(0, 0, 0, (int)(70 * p->op)));
    fill_circle(A.frame, kx, cy, kr, color_alpha(RGBA(255, 255, 255, 255), p->op));
    if (hover || active)
        draw_arc(A.frame, kx, cy, kr + 3, 0, 6.2832f, 2, color_alpha(p->accent, 0.45f * p->op));
    return res;
}

/* ---- switches, choices, buttons ---- */

bool ctl_switch(rect r, bool on, const palette *p) {
    bool hover = ui_hover(r);
    float rad = r.h / 2.0f;
    color track = on ? p->accent : color_alpha(p->ink, hover ? 0.26f : 0.18f);
    fill_round(A.frame, r, rad, color_alpha(track, p->op));
    float kx = on ? r.x + r.w - rad : r.x + rad;
    fill_circle(A.frame, kx, r.y + rad + 1, rad - 2.5f, RGBA(0, 0, 0, (int)(40 * p->op)));
    fill_circle(A.frame, kx, r.y + rad, rad - 3, color_alpha(RGBA(255, 255, 255, 255), p->op));
    return hover && A.in.clicked;
}

int ctl_segmented(rect r, const char *const *labels, int n, int sel, font *f, const palette *p) {
    if (n <= 0)
        return sel;
    float rad = r.h / 2.0f;
    fill_round(A.frame, r, rad, color_alpha(p->ink, 0.08f * p->op));
    int w = (r.w - 6) / n, picked = sel;
    for (int i = 0; i < n; i++) {
        rect s = {r.x + 3 + i * w, r.y + 3, w, r.h - 6};
        bool hover = ui_hover(s);
        if (i == sel) {
            shadow_round(A.frame, (rect){s.x, s.y + 1, s.w, s.h}, s.h / 2.0f, 6, RGBA(0, 0, 0, (int)(50 * p->op)));
            fill_round(A.frame, s, s.h / 2.0f, color_alpha(p->accent, p->op));
        } else if (hover) {
            fill_round(A.frame, s, s.h / 2.0f, color_alpha(p->ink, 0.08f * p->op));
        }
        draw_text_centered(A.frame, f, s, labels[i], color_alpha(i == sel ? p->on_accent : p->ink, p->op));
        if (hover && A.in.clicked)
            picked = i;
    }
    return picked;
}

bool ctl_button(rect r, const char *label, font *f, bool primary, bool enabled, const palette *p) {
    bool hover = enabled && ui_hover(r);
    float rad = r.h / 2.0f;
    color bg = primary ? p->accent : color_alpha(p->ink, 0.1f);
    if (hover)
        bg = primary ? color_mix(bg, RGBA(255, 255, 255, 255), 0.12f) : color_alpha(p->ink, 0.16f);
    if (A.in.down && hover)
        bg = primary ? color_mix(p->accent, RGBA(0, 0, 0, 255), 0.12f) : color_alpha(p->ink, 0.22f);
    fill_round(A.frame, r, rad, color_alpha(bg, (enabled ? 1 : 0.45f) * p->op));
    color fg = primary ? p->on_accent : p->ink;
    draw_text_centered(A.frame, f, r, label, color_alpha(fg, (enabled ? 1 : 0.5f) * p->op));
    return hover && A.in.clicked;
}

bool ctl_tile(rect r, bool on, const palette *p) {
    bool hover = ui_hover(r);
    float rad = r.h * 0.3f > 18 ? 18 : r.h * 0.3f;
    color bg = on ? p->accent : color_alpha(p->ink, hover ? 0.14f : 0.08f);
    if (on && hover)
        bg = color_mix(bg, RGBA(255, 255, 255, 255), 0.1f);
    fill_round(A.frame, r, rad, color_alpha(bg, p->op));
    return hover && A.in.clicked;
}

void ctl_text_fit(canvas *c, font *f, int x, int y, int w, const char *s, color col) {
    if (text_width(f, s) <= w) {
        draw_text(c, f, x, y, s, col);
        return;
    }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", s);
    int n = (int)strlen(buf), ew = text_width(f, "…");
    while (n > 0 && text_width_n(f, buf, n) + ew > w)
        n--;
    while (n > 0 && (buf[n] & 0xC0) == 0x80)
        n--;
    snprintf(buf + n, sizeof buf - (size_t)n, "…");
    draw_text(c, f, x, y, buf, col);
}

/* ---- symbols ---- */

static void slash(canvas *c, float cx, float cy, float h, float sw, color col) {
    draw_line(c, cx - h, cy - h, cx + h, cy + h, sw, col);
}

void ctl_symbol(canvas *c, rect b, int sym, float level, color col) {
    float s = (float)(b.w < b.h ? b.w : b.h), cx = b.x + b.w / 2.0f, cy = b.y + b.h / 2.0f, h = s * 0.36f;
    float sw = s / 13 < 1.3f ? 1.3f : s / 13;
    switch (sym) {
    case SYM_SPEAKER:
    case SYM_SPEAKER_MUTED: {
        float x0 = cx - h * 0.95f, bx = x0 + h * 0.42f, tip = x0 + h * 0.95f;
        float bh = h * 0.36f, th = h * 0.8f;
        /* box and cone, closed */
        draw_line(c, x0, cy - bh, bx, cy - bh, sw, col);
        draw_line(c, bx, cy - bh, tip, cy - th, sw, col);
        draw_line(c, tip, cy - th, tip, cy + th, sw, col);
        draw_line(c, tip, cy + th, bx, cy + bh, sw, col);
        draw_line(c, bx, cy + bh, x0, cy + bh, sw, col);
        draw_line(c, x0, cy + bh, x0, cy - bh, sw, col);
        if (sym == SYM_SPEAKER_MUTED) {
            float mx = tip + h * 0.62f, m = h * 0.32f;
            draw_line(c, mx - m, cy - m, mx + m, cy + m, sw, col);
            draw_line(c, mx - m, cy + m, mx + m, cy - m, sw, col);
        } else {
            int waves = level <= 0.01f ? 0 : level < 0.34f ? 1 : level < 0.67f ? 2 : 3;
            for (int i = 1; i <= 3; i++)
                draw_arc(c, tip - h * 0.1f, cy, h * (0.32f + 0.3f * i), -0.75f, 0.75f, sw,
                         i <= waves ? col : color_alpha(col, 0.22f));
        }
        break;
    }
    case SYM_NETWORK:
    case SYM_NETWORK_OFF: {
        color k = sym == SYM_NETWORK_OFF ? color_alpha(col, 0.55f) : col;
        float u = h * 0.42f;
        stroke_round(c, (rect){(int)(cx - u * 0.8f), (int)(cy - h), (int)(u * 1.6f), (int)(u * 1.3f)}, u * 0.3f, sw, k);
        draw_line(c, cx, cy - h + u * 1.3f, cx, cy + h * 0.1f, sw, k);
        draw_line(c, cx - h * 0.75f, cy + h * 0.1f, cx + h * 0.75f, cy + h * 0.1f, sw, k);
        for (int i = -1; i <= 1; i += 2) {
            float x = cx + i * h * 0.75f;
            draw_line(c, x, cy + h * 0.1f, x, cy + h * 0.38f, sw, k);
            stroke_round(c, (rect){(int)(x - u * 0.6f), (int)(cy + h * 0.38f), (int)(u * 1.2f), (int)(u * 1.1f)}, u * 0.25f,
                         sw, k);
        }
        if (sym == SYM_NETWORK_OFF)
            slash(c, cx, cy, h * 1.05f, sw * 1.2f, col);
        break;
    }
    case SYM_BELL:
    case SYM_BELL_OFF:
        draw_arc(c, cx, cy - h * 0.15f, h * 0.62f, 3.1416f, 6.2832f, sw, col);
        draw_line(c, cx - h * 0.62f, cy - h * 0.15f, cx - h * 0.72f, cy + h * 0.5f, sw, col);
        draw_line(c, cx + h * 0.62f, cy - h * 0.15f, cx + h * 0.72f, cy + h * 0.5f, sw, col);
        draw_line(c, cx - h * 0.95f, cy + h * 0.55f, cx + h * 0.95f, cy + h * 0.55f, sw, col);
        draw_arc(c, cx, cy + h * 0.62f, h * 0.24f, 0.2f, 2.94f, sw, col);
        draw_line(c, cx, cy - h * 0.77f, cx, cy - h * 0.95f, sw, col);
        if (sym == SYM_BELL_OFF)
            slash(c, cx, cy, h * 1.05f, sw * 1.2f, col);
        break;
    case SYM_CALENDAR: {
        rect r = {(int)(cx - h), (int)(cy - h * 0.85f), (int)(2 * h), (int)(h * 1.85f)};
        stroke_round(c, r, h * 0.25f, sw, col);
        draw_line(c, r.x, cy - h * 0.35f, r.x + r.w, cy - h * 0.35f, sw, col);
        draw_line(c, cx - h * 0.5f, cy - h * 1.05f, cx - h * 0.5f, cy - h * 0.65f, sw, col);
        draw_line(c, cx + h * 0.5f, cy - h * 1.05f, cx + h * 0.5f, cy - h * 0.65f, sw, col);
        for (int j = 0; j < 2; j++)
            for (int i = -1; i <= 1; i++)
                fill_circle(c, cx + i * h * 0.5f, cy + h * 0.1f + j * h * 0.45f, sw * 0.8f, col);
        break;
    }
    case SYM_SLIDERS:
        for (int i = -1; i <= 1; i += 2) {
            float y = cy + i * h * 0.45f, kx = cx + (i < 0 ? h * 0.35f : -h * 0.35f);
            draw_line(c, cx - h, y, cx + h, y, sw, col);
            fill_circle(c, kx, y, h * 0.28f, col);
        }
        break;
    case SYM_MOON:
        draw_arc(c, cx, cy, h * 0.85f, 0.95f, 5.35f, sw, col);
        draw_arc(c, cx + h * 0.62f, cy - h * 0.38f, h * 0.62f, 1.75f, 3.9f, sw, col);
        break;
    case SYM_SUN:
        draw_arc(c, cx, cy, h * 0.42f, 0, 6.2832f, sw, col);
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7854f;
            draw_line(c, cx + cosf(a) * h * 0.7f, cy + sinf(a) * h * 0.7f, cx + cosf(a) * h, cy + sinf(a) * h, sw, col);
        }
        break;
    case SYM_DISPLAY:
        stroke_round(c, (rect){(int)(cx - h), (int)(cy - h * 0.8f), (int)(2 * h), (int)(h * 1.3f)}, h * 0.15f, sw, col);
        draw_line(c, cx, cy + h * 0.5f, cx, cy + h * 0.85f, sw, col);
        draw_line(c, cx - h * 0.45f, cy + h * 0.88f, cx + h * 0.45f, cy + h * 0.88f, sw, col);
        break;
    case SYM_KEYBOARD:
        stroke_round(c, (rect){(int)(cx - h * 1.05f), (int)(cy - h * 0.6f), (int)(2.1f * h), (int)(h * 1.2f)}, h * 0.2f, sw,
                     col);
        for (int j = 0; j < 2; j++)
            for (int i = -2; i <= 2; i++)
                fill_circle(c, cx + i * h * 0.38f, cy - h * 0.25f + j * h * 0.32f, sw * 0.7f, col);
        draw_line(c, cx - h * 0.45f, cy + h * 0.32f, cx + h * 0.45f, cy + h * 0.32f, sw, col);
        break;
    case SYM_MIND:
        for (int i = 0; i < 4; i++) {
            float a = i * 1.5708f;
            draw_line(c, cx, cy, cx + cosf(a) * h, cy + sinf(a) * h, sw, col);
        }
        for (int i = 0; i < 4; i++) {
            float a = 0.7854f + i * 1.5708f;
            draw_line(c, cx, cy, cx + cosf(a) * h * 0.45f, cy + sinf(a) * h * 0.45f, sw, col);
        }
        fill_circle(c, cx, cy, sw * 1.4f, col);
        break;
    case SYM_SHIELD:
        draw_line(c, cx, cy - h, cx + h * 0.8f, cy - h * 0.65f, sw, col);
        draw_line(c, cx + h * 0.8f, cy - h * 0.65f, cx + h * 0.7f, cy + h * 0.2f, sw, col);
        draw_line(c, cx + h * 0.7f, cy + h * 0.2f, cx, cy + h, sw, col);
        draw_line(c, cx, cy + h, cx - h * 0.7f, cy + h * 0.2f, sw, col);
        draw_line(c, cx - h * 0.7f, cy + h * 0.2f, cx - h * 0.8f, cy - h * 0.65f, sw, col);
        draw_line(c, cx - h * 0.8f, cy - h * 0.65f, cx, cy - h, sw, col);
        break;
    case SYM_INFO:
        draw_arc(c, cx, cy, h, 0, 6.2832f, sw, col);
        fill_circle(c, cx, cy - h * 0.45f, sw * 0.9f, col);
        draw_line(c, cx, cy - h * 0.1f, cx, cy + h * 0.5f, sw, col);
        break;
    case SYM_PALETTE:
        draw_arc(c, cx, cy, h, 0.6f, 5.9f, sw, col);
        for (int i = 0; i < 3; i++) {
            float a = 3.6f + i * 0.85f;
            fill_circle(c, cx + cosf(a) * h * 0.55f, cy + sinf(a) * h * 0.55f, sw * 1.1f, col);
        }
        fill_circle(c, cx + h * 0.55f, cy + h * 0.25f, sw * 1.3f, col);
        break;
    case SYM_CHEVRON_LEFT:
        draw_line(c, cx + h * 0.3f, cy - h * 0.6f, cx - h * 0.3f, cy, sw, col);
        draw_line(c, cx - h * 0.3f, cy, cx + h * 0.3f, cy + h * 0.6f, sw, col);
        break;
    case SYM_CHEVRON_RIGHT:
        draw_line(c, cx - h * 0.3f, cy - h * 0.6f, cx + h * 0.3f, cy, sw, col);
        draw_line(c, cx + h * 0.3f, cy, cx - h * 0.3f, cy + h * 0.6f, sw, col);
        break;
    case SYM_CLOSE:
        draw_line(c, cx - h * 0.5f, cy - h * 0.5f, cx + h * 0.5f, cy + h * 0.5f, sw, col);
        draw_line(c, cx - h * 0.5f, cy + h * 0.5f, cx + h * 0.5f, cy - h * 0.5f, sw, col);
        break;
    case SYM_CHECK:
        draw_line(c, cx - h * 0.6f, cy, cx - h * 0.15f, cy + h * 0.45f, sw * 1.2f, col);
        draw_line(c, cx - h * 0.15f, cy + h * 0.45f, cx + h * 0.65f, cy - h * 0.5f, sw * 1.2f, col);
        break;
    case SYM_GEAR:
        draw_arc(c, cx, cy, h * 0.62f, 0, 6.2832f, sw, col);
        draw_arc(c, cx, cy, h * 0.22f, 0, 6.2832f, sw, col);
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7854f;
            draw_line(c, cx + cosf(a) * h * 0.62f, cy + sinf(a) * h * 0.62f, cx + cosf(a) * h * 0.95f,
                      cy + sinf(a) * h * 0.95f, sw * 1.3f, col);
        }
        break;
    case SYM_SENSE: {
        float px[7] = {-1, -0.5f, -0.3f, 0, 0.25f, 0.45f, 1}, py[7] = {0, 0, -0.7f, 0.75f, -0.35f, 0, 0};
        for (int i = 0; i < 6; i++)
            draw_line(c, cx + px[i] * h, cy + py[i] * h, cx + px[i + 1] * h, cy + py[i + 1] * h, sw, col);
        break;
    }
    }
}
