/* Facet 2's widgets, drawn by the shell into the boxes the engine laid out
 * (markup.c gives them class fx-w-<kind>): in the person's design, the
 * accent flowing into accent2, glowing as much as the design glows.
 *
 *   <ring value={done} max={goal} label="water"/>     a progress ring, the percentage (or label) inside
 *   <progress value={read} max={pages}/>              a bar
 *   <chart kind="bar" data={minutes} labels={days}/>  bars, or kind="line" for a line over a soft area */
#define _GNU_SOURCE
#include "markup.h"
#include "../font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f

static float num_attr(const fx_out *o, const char *name, float dflt) {
    const char *v = fx_get(o, name);
    return v && *v ? strtof(v, NULL) : dflt;
}

/* "[1, 2.5, 3]" (a list shown as text) into numbers */
static int nums(const char *s, float *out, int max) {
    int n = 0;
    for (const char *p = s ? s : ""; *p && n < max;) {
        while (*p && !(*p == '-' || *p == '.' || (*p >= '0' && *p <= '9')))
            p++;
        if (!*p)
            break;
        char *e;
        out[n++] = strtof(p, &e);
        p = e == p ? p + 1 : e;
    }
    return n;
}

/* "[Mon, Tue]" into words */
static int words(const char *s, char out[][16], int max) {
    int n = 0;
    const char *p = s ? s : "";
    if (*p == '[')
        p++;
    while (*p && n < max) {
        while (*p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ']')
            e++;
        snprintf(out[n++], 16, "%.*s", (int)(e - p) < 15 ? (int)(e - p) : 15, p);
        if (!*e || *e == ']')
            break;
        p = e + 1;
    }
    return n;
}

void fx_widget_draw(canvas *c, rect r, const fx_out *o, const design *d, float shown) {
    color a = d->accent, b = d->accent2, ink = d->surface_text;
    color track = color_alpha(ink, 0.12f);
    float value = num_attr(o, "value", 0), max = num_attr(o, "max", 100);
    if (max <= 0)
        max = 1;
    float v = shown >= 0 ? shown : value;
    float frac = v / max < 0 ? 0 : v / max > 1 ? 1 : v / max;
    if (!strcmp(o->tag, "ring")) {
        float side = (float)(r.w < r.h ? r.w : r.h), cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
        float w = side * 0.1f, rad = side / 2 - w / 2 - 2;
        if (d->glow > 0.05f && frac > 0)
            glow_circle(c, cx, cy, side * 0.62f, color_alpha(a, 0.12f + 0.2f * d->glow));
        draw_arc(c, cx, cy, rad, 0, 2 * PI, w, track);
        /* the arc from the top, clockwise, accent turning to accent2 along it */
        int steps = (int)(64 * frac) + 1;
        float a0 = -PI / 2;
        for (int i = 0; i < steps; i++) {
            float t0 = (float)i / steps, t1 = (float)(i + 1) / steps;
            draw_arc(c, cx, cy, rad, a0 + 2 * PI * frac * t0, a0 + 2 * PI * frac * t1 + 0.01f, w,
                     color_mix(a, b, (t0 + t1) / 2));
        }
        if (frac > 0) { /* a round cap at the end */
            float ea = a0 + 2 * PI * frac;
            fill_circle(c, cx + cosf(ea) * rad, cy + sinf(ea) * rad, w / 2, b);
            fill_circle(c, cx + cosf(a0) * rad, cy + sinf(a0) * rad, w / 2, a);
        }
        const char *label = fx_get(o, "label");
        char pct[16];
        snprintf(pct, sizeof pct, "%.0f%%", (double)(frac * 100));
        font *big = font_get(d->font_heading, side * 0.22f), *small = font_get(d->font_body, side * 0.11f);
        int bh = font_line_height(big), sh = label ? font_line_height(small) : 0;
        int y = (int)(cy - (bh + sh) / 2.0f);
        draw_text_centered(c, big, (rect){r.x, y, r.w, bh}, pct, ink);
        if (label)
            draw_text_centered(c, small, (rect){r.x, y + bh, r.w, sh}, label, color_alpha(ink, 0.65f));
        return;
    }
    if (!strcmp(o->tag, "progress")) {
        float h = (float)r.h, rad = h / 2;
        fill_round(c, r, rad, track);
        int fw = (int)(r.w * frac);
        if (fw > 0) {
            rect f = {r.x, r.y, fw < (int)h ? (int)h : fw, r.h};
            if (d->glow > 0.05f)
                for (int i = 0; i < 3; i++)
                    glow_circle(c, f.x + f.w - h / 2, f.y + h / 2, h * (1.5f + i), color_alpha(b, 0.07f * d->glow));
            /* accent into accent2 across its length: round ends, a gradient between them */
            fill_circle(c, f.x + rad, f.y + rad, rad, a);
            fill_circle(c, f.x + f.w - rad, f.y + rad, rad, color_mix(a, b, frac));
            int x0 = (int)(f.x + rad), x1 = (int)(f.x + f.w - rad);
            for (int x = x0; x < x1; x++) {
                float t = r.w > 0 ? (float)(x - r.x) / r.w : 0;
                fill_rect(c, (rect){x, f.y, 1, f.h}, color_mix(a, b, t));
            }
        }
        return;
    }
    if (!strcmp(o->tag, "chart")) {
        float vals[64];
        int n = nums(fx_get(o, "data"), vals, 64);
        char labels[64][16];
        int nl = words(fx_get(o, "labels"), labels, 64);
        if (n == 0)
            return;
        float hi = vals[0], lo = 0;
        for (int i = 0; i < n; i++)
            hi = vals[i] > hi ? vals[i] : hi, lo = vals[i] < lo ? vals[i] : lo;
        if (hi - lo < 1e-6f)
            hi = lo + 1;
        font *small = font_get(d->font_body, 11);
        int lh = nl ? font_line_height(small) + 6 : 0;
        rect plot = {r.x + 4, r.y + 8, r.w - 8, r.h - 12 - lh};
        float grow = shown < 0 ? 1 : value > 0 ? shown / value : shown; /* bars and line rise as they arrive */
        grow = grow < 0 ? 0 : grow > 1 ? 1 : grow;
        for (int g = 1; g <= 3; g++) /* faint guides */
            draw_line(c, plot.x, plot.y + plot.h * g / 4.0f, plot.x + plot.w, plot.y + plot.h * g / 4.0f, 1,
                      color_alpha(ink, 0.06f));
        const char *kind = fx_get(o, "kind");
        bool line = kind && !strcmp(kind, "line");
        float slot = (float)plot.w / n;
        if (!line) {
            float bw = slot * 0.58f;
            for (int i = 0; i < n; i++) {
                float hh = (vals[i] - lo) / (hi - lo) * plot.h * grow;
                rect bar = {(int)(plot.x + slot * i + (slot - bw) / 2), (int)(plot.y + plot.h - hh), (int)bw,
                            (int)hh};
                if (bar.h > 0) {
                    float t = n > 1 ? (float)i / (n - 1) : 0;
                    fill_round_vgradient(c, bar, bw * 0.3f < 8 ? bw * 0.3f : 8, color_mix(a, b, t),
                                         color_alpha(color_mix(a, b, t), 0.55f));
                }
            }
        } else {
            float xy[2 * 66];
            int k = 0;
            for (int i = 0; i < n; i++) {
                xy[k++] = plot.x + slot * (i + 0.5f);
                xy[k++] = plot.y + plot.h - (vals[i] - lo) / (hi - lo) * plot.h * grow;
            }
            /* the soft area below, then the line, then the points */
            float area[2 * 68];
            int m = 0;
            area[m++] = xy[0], area[m++] = plot.y + plot.h;
            for (int i = 0; i < k; i++)
                area[m++] = xy[i];
            area[m++] = xy[k - 2], area[m++] = plot.y + plot.h;
            fill_polygon(c, area, m / 2, color_alpha(a, 0.16f));
            for (int i = 0; i + 3 < k; i += 2)
                draw_line(c, xy[i], xy[i + 1], xy[i + 2], xy[i + 3], 3, color_mix(a, b, (float)i / k));
            for (int i = 0; i < k; i += 2) {
                if (d->glow > 0.05f)
                    glow_circle(c, xy[i], xy[i + 1], 10, color_alpha(b, 0.25f * d->glow));
                fill_circle(c, xy[i], xy[i + 1], 4, b);
                fill_circle(c, xy[i], xy[i + 1], 2, d->surface);
            }
        }
        for (int i = 0; i < nl && i < n; i++)
            draw_text_centered(c, small, (rect){(int)(plot.x + slot * i), plot.y + plot.h + 4, (int)slot, lh},
                               labels[i], color_alpha(ink, 0.6f));
    }
}
