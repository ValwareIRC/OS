#include "app.h"
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
    canvas_clip(A.frame, inner);
    int ty = r.y + (r.h - font_line_height(f)) / 2;
    char shown[1024];
    if (t->secret) {
        int n = t->len < 64 ? t->len : 64;
        for (int i = 0; i < n; i++)
            shown[i] = '*';
        shown[n] = 0;
    } else {
        memcpy(shown, t->buf, (size_t)t->len + 1);
    }
    int w = text_width(f, shown), x = inner.x;
    if (w > inner.w - 4)
        x = inner.x + inner.w - 4 - w; /* keep the end of long text visible */
    if (t->len)
        draw_text(A.frame, f, x, ty, shown, fg);
    else if (placeholder)
        draw_text(A.frame, f, inner.x, ty, placeholder, muted);
    if (focused && (A.now / 530) % 2 == 0)
        fill_rect(A.frame, (rect){x + (t->len ? w : 0) + 1, ty + 2, 2, font_line_height(f) - 4}, fg);
    canvas_unclip(A.frame);
}

bool ui_type(struct textfield *t) {
    bool enter = false;
    for (int i = 0; i < A.in.nkeys; i++) {
        int k = A.in.keys[i];
        if (k == K_ENTER)
            enter = true;
        else if (k == K_BACKSPACE) {
            if (t->len) {
                /* remove one UTF-8 character */
                do
                    t->len--;
                while (t->len && (t->buf[t->len] & 0xc0) == 0x80);
                t->buf[t->len] = 0;
            }
        } else if (k == K_PASTE) {
            /* printable characters only: line breaks and tabs in a pasted key are noise */
            for (const char *p = clipboard_text(); *p && t->len < (int)sizeof t->buf - 1; p++)
                if ((unsigned char)*p >= 32 && *p != 127)
                    t->buf[t->len++] = *p;
            t->buf[t->len] = 0;
        } else if (k >= 32 && k < 127 && t->len < (int)sizeof t->buf - 1) {
            t->buf[t->len++] = (char)k;
            t->buf[t->len] = 0;
        } else if (((k >= 0xa0 && k < K_ENTER) || (k > K_TALK && k < 0x10000)) && t->len < (int)sizeof t->buf - 4) {
            /* accented letters and symbols from other keyboard layouts, as UTF-8 */
            if (k < 0x800) {
                t->buf[t->len++] = (char)(0xc0 | k >> 6);
            } else {
                t->buf[t->len++] = (char)(0xe0 | k >> 12);
                t->buf[t->len++] = (char)(0x80 | ((k >> 6) & 0x3f));
            }
            t->buf[t->len++] = (char)(0x80 | (k & 0x3f));
            t->buf[t->len] = 0;
        }
    }
    return enter;
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
