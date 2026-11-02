#include "app.h"
#include "controls.h"
#include "wallpaper.h"
#include "image.h"
#include "fm/fm.h"
#include <stdio.h>
#include <strings.h>
#include <math.h>
#include <string.h>

bool ui_hover(rect r) { return pt_in(r, A.in.mx, A.in.my); }

bool ui_button(rect r, const char *label, font *f, color bg, color fg, float radius, bool enabled) {
    bool hover = enabled && ui_hover(r);
    color b = enabled ? (hover ? color_mix(bg, RGBA(255, 255, 255, C_A(bg)), 0.12f) : bg) : color_alpha(bg, 0.4f);
    fill_round(A.frame, r, radius, b);
    draw_text_centered(A.frame, f, r, label, enabled ? fg : color_alpha(fg, 0.5f));
    return hover && A.in.clicked;
}

void ui_textfield(rect r, struct textfield *t, bool focused, const char *placeholder, font *f, color bg, color fg,
                  color muted, float radius) {
    fill_round(A.frame, r, radius, bg);
    if (focused)
        stroke_round(A.frame, r, radius, 1.5f, color_alpha(fg, 0.5f));
    rect inner = rect_inset(r, 14);
    int ty = r.y + (r.h - font_line_height(f)) / 2;
    t->len = (int)strlen(t->buf);
    te_mouse(&t->ed, t->buf, f, (rect){r.x, r.y, r.w, r.h}, inner.x, &A.in, t->secret, A.now);
    if (!t->len && placeholder)
        draw_text(A.frame, f, inner.x, ty, placeholder, muted);
    te_draw(A.frame, &t->ed, t->buf, f, (rect){inner.x - 2, r.y, inner.w + 4, r.h}, inner.x, ty, fg,
            color_alpha(fg, 0.22f), focused, t->secret, A.now);
}

bool ui_type(struct textfield *t) {
    bool enter = false;
    t->len = (int)strlen(t->buf);
    for (int i = 0; i < A.in.nkeys; i++) {
        int k = A.in.keys[i];
        if (k == K_ENTER)
            enter = true;
        else
            te_key(&t->ed, t->buf, (int)sizeof t->buf, k, (A.in.keymod[i] & KM_CTRL) != 0, (A.in.keymod[i] & KM_SHIFT) != 0, t->secret, A.now, NULL);
    }
    t->len = (int)strlen(t->buf);
    return enter;
}

/* ---- the right-click menu of a text field ---- */

static const struct {
    const char *label, *keys;
    int key, mod;
} text_items[] = {
    {"Undo", "Ctrl+Z", 'z', KM_CTRL},   {"Cut", "Ctrl+X", 'x', KM_CTRL},
    {"Copy", "Ctrl+C", 'c', KM_CTRL},   {"Paste", "Ctrl+V", K_PASTE, 0},
    {"Select all", "Ctrl+A", 'a', KM_CTRL},
};
#define NTEXT_ITEMS 5

static bool text_item_on(const te_menu *m, int i) {
    switch (i) {
    case 0: return m->undo;
    case 1: case 2: return m->selection && !m->secret;
    case 3: return clipboard_text()[0] != 0;
    default: return true;
    }
}

static rect text_menu_rect(const te_menu *m, font *f, int *row) {
    *row = font_line_height(f) + 14;
    int w = 220, h = NTEXT_ITEMS * *row + 12;
    int x = m->x + w > A.W - 8 ? A.W - 8 - w : m->x, y = m->y + h > A.H - 8 ? m->y - h : m->y;
    return (rect){x, y < 8 ? 8 : y, w, h};
}

void ui_text_menu_input(void) {
    te_menu *m = te_menu_state();
    if (!m->open)
        return;
    font *f = fm_host_font(A.d.font_body, A.d.body_px - 1);
    int row;
    rect r = text_menu_rect(m, f, &row);
    int key = 0, mod = 0;
    for (int i = 0; i < A.in.nkeys && m->open; i++) { /* the arrows choose, Enter takes it, Esc closes */
        int k = A.in.keys[i];
        if (k == K_ESC) {
            m->open = false;
        } else if (k == K_DOWN || k == K_UP) {
            int d = k == K_DOWN ? 1 : -1, s = m->sel;
            for (int n = 0; n < NTEXT_ITEMS; n++) {
                s = s < 0 ? (d > 0 ? 0 : NTEXT_ITEMS - 1) : (s + d + NTEXT_ITEMS) % NTEXT_ITEMS;
                if (text_item_on(m, s))
                    break;
            }
            m->sel = s;
        } else if (k == K_ENTER && m->sel >= 0 && text_item_on(m, m->sel)) {
            key = text_items[m->sel].key, mod = text_items[m->sel].mod;
            m->open = false;
        }
    }
    if (pt_in(r, A.in.mx, A.in.my) && A.in.moved) {
        int i = (A.in.my - r.y - 6) / row;
        m->sel = i >= 0 && i < NTEXT_ITEMS && text_item_on(m, i) ? i : -1;
    }
    if (key) {
    } else if (A.in.clicked && pt_in(r, A.in.mx, A.in.my)) {
        int i = (A.in.my - r.y - 6) / row;
        if (i >= 0 && i < NTEXT_ITEMS && text_item_on(m, i)) {
            key = text_items[i].key, mod = text_items[i].mod;
            m->open = false;
        }
    } else if ((A.in.pressed || A.in.right_clicked) && !pt_in(r, A.in.mx, A.in.my)) {
        m->open = false;
    }
    /* while it is up, the pointer and the keys are the menu's; what it chose is typed into the field */
    A.in.pressed = A.in.clicked = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.nkeys = 0;
    if (key) {
        A.in.keys[0] = key, A.in.keymod[0] = (uint8_t)mod;
        A.in.nkeys = 1;
    }
}

void ui_text_menu_draw(void) {
    te_menu *m = te_menu_state();
    if (!m->open)
        return;
    font *f = fm_host_font(A.d.font_body, A.d.body_px - 1);
    int row;
    rect r = text_menu_rect(m, f, &row);
    if (!m->shown) {
        m->shown = true;
        LOGRECT("text-menu", r);
    }
    float rad = A.d.radius * 0.5f + 4;
    ctl_panel(r, rad > 14 ? 14 : rad, 1);
    palette p = pal_on(pal_sample(rect_inset(r, 6)), 1);
    for (int i = 0; i < NTEXT_ITEMS; i++) {
        rect it = {r.x + 6, r.y + 6 + i * row, r.w - 12, row};
        bool on = text_item_on(m, i);
        if (on && i == m->sel)
            fill_round(A.frame, it, rad > 8 ? 8 : rad * 0.6f, color_alpha(p.ink, 0.1f));
        int ty = it.y + (row - font_line_height(f)) / 2;
        color ink = on ? p.ink : color_alpha(p.ink, 0.38f);
        draw_text(A.frame, f, it.x + 12, ty, text_items[i].label, ink);
        int kw = text_width(f, text_items[i].keys);
        draw_text(A.frame, f, it.x + it.w - 12 - kw, ty, text_items[i].keys, color_alpha(p.muted, on ? 1 : 0.5f));
    }
}

/* The presence of the AI: a soft breathing light, livelier while it thinks. */
/* The presence of the AI: a luminous sphere with light swirling inside,
 * breathing slowly at rest and quickening while it thinks. */
struct orb_shade {
    float cx, cy, r;
    color a, b;
};

static color shade_orb(void *ctx, int x, int y) {
    struct orb_shade *o = ctx;
    float dx = (x + 0.5f - o->cx) / o->r, dy = (y + 0.5f - o->cy) / o->r;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > 1)
        d = 1;
    float k = 1 - d * d; /* bright core, soft falloff */
    /* ordered dither (4x4 Bayer), a fraction of one level */
    static const float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    float n = (bayer[(y & 3) * 4 + (x & 3)] / 16.0f - 0.5f) / 255.0f;
    color col = color_mix(o->a, o->b, k + n * 4);
    float alpha = 0.55f + 0.4f * k + n;
    return color_alpha(col, alpha > 1 ? 1 : alpha);
}

void ui_orb(canvas *c, float cx, float cy, float r, color a, color b, bool active) {
    float t = (float)(A.now % 1000000) / 1000.0f;
    float speed = active ? 2.6f : 0.7f;
    float breath = 0.5f + 0.5f * sinf(t * (active ? 4.0f : 1.3f));
    /* halo */
    glow_circle(c, cx, cy, r * (3.2f + 0.4f * breath), color_alpha(a, 0.16f + 0.08f * breath));
    glow_circle(c, cx, cy, r * 1.9f, color_alpha(color_mix(a, b, 0.5f), 0.28f + 0.1f * breath));
    /* body: one smooth radial gradient, rim to core, dithered so it never bands */
    struct orb_shade sh = {cx, cy, r, a, b};
    int ir = (int)ceilf(r);
    fill_round_shaded(c, (rect){(int)cx - ir, (int)cy - ir, ir * 2, ir * 2}, (float)ir, shade_orb, &sh);
    /* light swirling inside */
    for (int i = 0; i < 4; i++) {
        float ang = t * speed * (0.6f + i * 0.25f) + i * 1.57f;
        float rr = r * (0.28f + 0.12f * sinf(t * 0.7f + i));
        glow_circle(c, cx + cosf(ang) * rr, cy + sinf(ang) * rr * 0.8f, r * 0.55f,
                    color_alpha(i % 2 ? b : RGBA(255, 255, 255, 255), 0.22f));
    }
    /* specular highlight and rim */
    glow_circle(c, cx - r * 0.32f, cy - r * 0.38f, r * 0.42f, RGBA(255, 255, 255, 120));
    draw_arc(c, cx, cy, r * 0.98f, 3.6f, 5.4f, 1.2f, RGBA(255, 255, 255, 70));
    if (active)
        for (int k = 0; k < 3; k++) {
            float ang = t * (1.4f + k * 0.5f) + k * 2.1f;
            float orbit = r * (1.45f + 0.1f * k);
            fill_circle(c, cx + cosf(ang) * orbit, cy + sinf(ang) * orbit, r * 0.06f, color_alpha(b, 0.9f));
            glow_circle(c, cx + cosf(ang) * orbit, cy + sinf(ang) * orbit, r * 0.25f, color_alpha(b, 0.35f));
        }
}

/* Pointers a design can ask for (cursor: ...). Drawn as small vector shapes, black outlined in white
 * (or white in black for the classic arrow), so they read on anything. */
static void poly(canvas *c, const float *pts, int n, float x, float y, float s, color fill, color edge, float ew) {
    float p[64];
    for (int i = 0; i < n && i < 32; i++)
        p[2 * i] = x + pts[2 * i] * s, p[2 * i + 1] = y + pts[2 * i + 1] * s;
    fill_polygon(c, p, n, fill);
    for (int i = 0; i < n; i++)
        draw_line(c, p[2 * i], p[2 * i + 1], p[2 * ((i + 1) % n)], p[2 * ((i + 1) % n) + 1], ew, edge);
}

bool ui_cursor_shape(canvas *c, int x, int y, const char *shape) {
    if (!shape || !shape[0] || !strncasecmp(shape, "dot", 3) || !strncasecmp(shape, "auto", 4))
        return false;
    float X = (float)x, Y = (float)y;
    color w = RGBA(255, 255, 255, 255), k = RGBA(0, 0, 0, 255);
    if (!strncasecmp(shape, "none", 4))
        return true;
    if (!strncasecmp(shape, "url(", 4)) { /* a picture: url(file) hotspot-x hotspot-y */
        char path[200] = "";
        int hx = 0, hy = 0;
        const char *q = shape + 4;
        int n = 0;
        while (*q && *q != ')' && n < 199)
            if (*q != '"' && *q != '\'')
                path[n++] = *q++;
            else
                q++;
        if (*q == ')')
            sscanf(q + 1, "%d %d", &hx, &hy);
        int iw, ih;
        extern const uint32_t *fmi_picture(const char *src, int want_w, int want_h, int *w, int *h);
        const uint32_t *px = fmi_picture(path, 32, 32, &iw, &ih);
        if (!px)
            return false;
        image im = {iw, ih, (uint32_t *)px, iw, ih};
        image_draw(c, &im, x - hx, y - hy, 1);
        return true;
    }
    if (!strncasecmp(shape, "pointer", 7) || !strncasecmp(shape, "hand", 4)) { /* a pointing hand */
        static const float hand[] = {4, 0, 6, 0, 7, 1, 7, 6, 9, 5.5f, 11, 6, 13, 6.5f, 14, 7.5f, 14, 13, 12, 16, 5, 16, 1, 10,
                                     1, 8.5f, 2.5f, 8.5f, 3, 9.5f, 3, 1};
        poly(c, hand, 16, X - 5, Y, 1.2f, w, k, 1.1f);
        return true;
    }
    if (!strncasecmp(shape, "text", 4)) { /* an I-beam */
        draw_line(c, X, Y - 9, X, Y + 9, 3, w), draw_line(c, X, Y - 9, X, Y + 9, 1.2f, k);
        draw_line(c, X - 4, Y - 9, X + 4, Y - 9, 1.2f, k), draw_line(c, X - 4, Y + 9, X + 4, Y + 9, 1.2f, k);
        return true;
    }
    if (!strncasecmp(shape, "crosshair", 9)) {
        draw_line(c, X - 10, Y, X + 10, Y, 3, w), draw_line(c, X, Y - 10, X, Y + 10, 3, w);
        draw_line(c, X - 10, Y, X + 10, Y, 1.2f, k), draw_line(c, X, Y - 10, X, Y + 10, 1.2f, k);
        return true;
    }
    if (!strncasecmp(shape, "move", 4) || !strncasecmp(shape, "grab", 4) || !strncasecmp(shape, "all-scroll", 10)) {
        for (int i = 0; i < 4; i++) {
            float dx = i == 0 ? 1.0f : i == 1 ? -1.0f : 0, dy = i == 2 ? 1.0f : i == 3 ? -1.0f : 0;
            float tri[] = {dx * 11, dy * 11, dx * 6 - dy * 4, dy * 6 - dx * 4, dx * 6 + dy * 4, dy * 6 + dx * 4};
            poly(c, tri, 3, X, Y, 1, k, w, 1);
        }
        draw_line(c, X - 7, Y, X + 7, Y, 1.6f, k), draw_line(c, X, Y - 7, X, Y + 7, 1.6f, k);
        return true;
    }
    if (!strncasecmp(shape, "wait", 4) || !strncasecmp(shape, "progress", 8)) { /* an hourglass */
        static const float glass[] = {0, 0, 10, 0, 10, 2, 6, 7, 10, 12, 10, 14, 0, 14, 0, 12, 4, 7, 0, 2};
        poly(c, glass, 10, X - 5, Y - 7, 1.2f, w, k, 1.1f);
        return true;
    }
    if (!strncasecmp(shape, "not-allowed", 11) || !strncasecmp(shape, "no-drop", 7)) {
        draw_arc(c, X, Y, 8, 0, 6.2832f, 4, w), draw_arc(c, X, Y, 8, 0, 6.2832f, 2, RGBA(220, 40, 40, 255));
        draw_line(c, X - 5.5f, Y - 5.5f, X + 5.5f, Y + 5.5f, 2, RGBA(220, 40, 40, 255));
        return true;
    }
    /* default / arrow: the classic arrow, white with a black edge */
    static const float arrow[] = {0, 0, 0, 16, 4, 12.5f, 7, 19, 9.5f, 18, 6.5f, 11.5f, 11.5f, 11.5f};
    poly(c, arrow, 7, X, Y, 1.05f, w, k, 1.1f);
    return true;
}

void ui_cursor(canvas *c, int x, int y) {
    /* A soft luminous dot: calmer than an arrow (unless the design asks for another pointer). */
    if (A.screen == SCR_DESKTOP && A.ui && ui_cursor_shape(c, x, y, fm_cursor(A.ui, x, y)))
        return;
    glow_circle(c, (float)x, (float)y, 14, RGBA(255, 255, 255, 40));
    fill_circle(c, (float)x, (float)y, 6.0f, RGBA(20, 18, 24, 110)); /* a faint rim, for light backgrounds */
    fill_circle(c, (float)x, (float)y, 5.0f, RGBA(255, 255, 255, 240));
}

color readable_on(color bg) { return color_luma(bg) > 0.4f ? RGBA(20, 18, 22, 255) : RGBA(252, 250, 247, 255); }

/* An animated luminous backdrop for the screens before the AI has designed
 * anything (and behind onboarding), tinted by whatever palette is chosen. */
void ui_backdrop(color base, color l1, color l2, color depth) {
    static design d;
    d.wallpaper = WP_NEBULA;
    d.wp[0] = base, d.wp[1] = l1, d.wp[2] = l2, d.wp[3] = depth;
    d.wp_seed = 7;
    d.wp_intensity = 0.55f;
    d.motion = 0.35f;
    wallpaper_frame(A.frame, &d, (float)A.now / 1000.0f);
}
