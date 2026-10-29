#include "app.h"
#include <math.h>

/* Vector icons, drawn from geometry so they stay crisp at any size and in
 * any colour. The AI chooses the tile shape and the rendering style; the
 * symbols themselves are the OS's (an icon must still say what it opens). */

static void line(canvas *c, rect b, float x0, float y0, float x1, float y1, float w, color col) {
    draw_line(c, b.x + x0 * b.w, b.y + y0 * b.h, b.x + x1 * b.w, b.y + y1 * b.h, w, col);
}

void icon_symbol(canvas *c, rect b, int app, float w, color col) {
    float cx = b.x + b.w / 2.0f, cy = b.y + b.h / 2.0f, s = (float)b.w;
    switch (app) {
    case APP_FILES:
        stroke_round(c, (rect){(int)(b.x + s * 0.12f), (int)(b.y + s * 0.3f), (int)(s * 0.76f), (int)(s * 0.52f)}, s * 0.08f, w, col);
        line(c, b, 0.14f, 0.3f, 0.2f, 0.18f, w, col);
        line(c, b, 0.2f, 0.18f, 0.44f, 0.18f, w, col);
        line(c, b, 0.44f, 0.18f, 0.5f, 0.3f, w, col);
        break;
    case APP_COMPANION: /* a spark: the companion's presence */
        line(c, b, 0.5f, 0.12f, 0.5f, 0.88f, w, col);
        line(c, b, 0.12f, 0.5f, 0.88f, 0.5f, w, col);
        line(c, b, 0.28f, 0.28f, 0.72f, 0.72f, w * 0.7f, color_alpha(col, 0.7f));
        line(c, b, 0.72f, 0.28f, 0.28f, 0.72f, w * 0.7f, color_alpha(col, 0.7f));
        fill_circle(c, cx, cy, s * 0.12f, col);
        break;
    case APP_NOTES:
        stroke_round(c, (rect){(int)(b.x + s * 0.2f), (int)(b.y + s * 0.12f), (int)(s * 0.6f), (int)(s * 0.76f)}, s * 0.08f, w, col);
        line(c, b, 0.32f, 0.36f, 0.68f, 0.36f, w, col);
        line(c, b, 0.32f, 0.52f, 0.68f, 0.52f, w, col);
        line(c, b, 0.32f, 0.68f, 0.54f, 0.68f, w, col);
        break;
    case APP_SETTINGS:
        for (int i = 0; i < 8; i++) {
            float a = i * 0.785398f;
            draw_line(c, cx + cosf(a) * s * 0.26f, cy + sinf(a) * s * 0.26f, cx + cosf(a) * s * 0.38f, cy + sinf(a) * s * 0.38f,
                      w * 1.4f, col);
        }
        draw_arc(c, cx, cy, s * 0.24f, 0, 6.2831f, w, col);
        draw_arc(c, cx, cy, s * 0.09f, 0, 6.2831f, w, col);
        break;
    case APP_ABOUT:
        draw_arc(c, cx, cy, s * 0.36f, 0, 6.2831f, w, col);
        fill_circle(c, cx, cy - s * 0.15f, w * 0.9f, col);
        line(c, b, 0.5f, 0.42f, 0.5f, 0.68f, w, col);
        break;
    case APP_TERMINAL: /* a prompt: chevron and cursor */
        stroke_round(c, (rect){(int)(b.x + s * 0.1f), (int)(b.y + s * 0.18f), (int)(s * 0.8f), (int)(s * 0.64f)}, s * 0.1f, w, col);
        line(c, b, 0.26f, 0.38f, 0.38f, 0.5f, w, col);
        line(c, b, 0.38f, 0.5f, 0.26f, 0.62f, w, col);
        line(c, b, 0.46f, 0.64f, 0.66f, 0.64f, w, col);
        break;
    case APP_REDESIGN: /* three overlapping circles: colour and form */
        draw_arc(c, cx - s * 0.12f, cy + s * 0.08f, s * 0.2f, 0, 6.2831f, w, col);
        draw_arc(c, cx + s * 0.12f, cy + s * 0.08f, s * 0.2f, 0, 6.2831f, w, col);
        draw_arc(c, cx, cy - s * 0.13f, s * 0.2f, 0, 6.2831f, w, col);
        break;
    case APP_POWER:
        draw_arc(c, cx, cy + s * 0.04f, s * 0.3f, -1.05f - 3.1416f * 0.5f + 3.1416f, 4.19f - 3.1416f * 0.5f + 3.1416f, w, col);
        line(c, b, 0.5f, 0.14f, 0.5f, 0.46f, w, col);
        break;
    }
}

void ui_icon(canvas *c, rect r, const design *d, int app, bool hover) {
    float s = (float)r.w;
    color a = color_mix(d->accent, d->accent2, (float)app / (APP_COUNT - 1));
    float rad = d->icons == ICON_CIRCLE ? s / 2 : d->icons == ICON_SQUIRCLE ? s * 0.3f : d->icons == ICON_SQUARE ? s * 0.12f : 0;
    float lift = hover ? 1.0f : 0.0f;
    float w = s * 0.055f + 0.6f;
    rect sym = rect_inset(r, (int)(s * 0.24f));

    switch (d->icon_style) {
    case ICONSTYLE_GLOW:
        if (d->icons != ICON_GLYPH) {
            fill_round(c, r, rad, color_alpha(RGBA(0, 0, 0, 255), 0.25f));
            stroke_round(c, r, rad, 1.2f, color_alpha(a, 0.55f + 0.35f * lift));
        }
        glow_circle(c, r.x + s / 2, r.y + s / 2, s * (0.75f + 0.25f * lift), color_alpha(a, 0.22f + 0.2f * lift));
        icon_symbol(c, sym, app, w * 1.6f, color_alpha(a, 0.35f));
        icon_symbol(c, sym, app, w, color_mix(a, RGBA(255, 255, 255, 255), 0.55f));
        break;
    case ICONSTYLE_LINE:
        if (d->icons != ICON_GLYPH) {
            fill_round(c, r, rad, color_alpha(d->surface, 0.35f + 0.2f * lift));
            stroke_round_vgradient(c, r, rad, 1, RGBA(255, 255, 255, 90), RGBA(255, 255, 255, 15));
        }
        icon_symbol(c, sym, app, w, hover ? a : d->surface_text);
        break;
    case ICONSTYLE_DUOTONE:
        fill_round(c, r, rad, color_alpha(a, 0.22f + 0.15f * lift));
        stroke_round(c, r, rad, 1, color_alpha(a, 0.5f));
        icon_symbol(c, sym, app, w * 1.15f, a);
        break;
    default: /* filled: luminous gradient tile with a white symbol */
        if (d->shadows)
            shadow_round(c, (rect){r.x, r.y + (int)(s * 0.08f), r.w, r.h}, rad, s * 0.25f, color_alpha(a, 0.45f));
        fill_round_vgradient(c, r, rad, color_mix(a, RGBA(255, 255, 255, 255), 0.25f + 0.1f * lift), color_mix(a, RGBA(0, 0, 0, 255), 0.15f));
        stroke_round_vgradient(c, r, rad, 1, RGBA(255, 255, 255, 120), RGBA(255, 255, 255, 0));
        /* White symbols on dark tiles, deep ones on light tiles. */
        icon_symbol(c, sym, app, w, color_luma(a) > 0.45f ? color_mix(a, RGBA(0, 0, 0, 255), 0.7f) : RGBA(255, 255, 255, 245));
        break;
    }
}
