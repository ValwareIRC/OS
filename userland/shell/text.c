#include "font.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Setting text in boxes: wrapped paragraphs aligned line by line, labels
 * cut short with an ellipsis, and soft text shadows. Everything here is
 * built on font.c's measuring and drawing, so it works for any face,
 * weight, slant or letter-spacing. */

/* Bytes of s that go on the first line of a block `w` px wide: as many
 * words as fit, at least one, stopping at a newline. */
static int line_len(font *f, const char *s, int w) {
    int best = 0, i = 0;
    while (s[i] && s[i] != '\n') {
        int j = i;
        while (s[j] == ' ')
            j++;
        while (s[j] && s[j] != ' ' && s[j] != '\n')
            j++;
        if (text_width_n(f, s, j) > w && best > 0)
            break;
        best = j;
        i = j;
        if (text_width_n(f, s, j) > w)
            break;
    }
    return best ? best : i; /* a single word longer than the line */
}

static const char *skip_break(const char *s) {
    if (*s == '\n')
        s++;
    while (*s == ' ')
        s++;
    return s;
}

int text_ellipsize(font *f, const char *s, int width, char *out, int outlen) {
    int n = (int)strlen(s);
    if (n >= outlen)
        n = outlen - 1;
    int w = text_width_n(f, s, n);
    if (w <= width && n == (int)strlen(s)) {
        memcpy(out, s, (size_t)n);
        out[n] = 0;
        return w;
    }
    int ew = text_width(f, "\xe2\x80\xa6");
    while (n > 0 && text_width_n(f, s, n) + ew > width)
        n--;
    while (n > 0 && ((s[n] & 0xC0) == 0x80 || s[n - 1] == ' ')) /* whole characters, no space before the dots */
        n--;
    if (n > outlen - 4)
        n = outlen - 4;
    memcpy(out, s, (size_t)n);
    memcpy(out + n, "\xe2\x80\xa6", 4);
    return text_width(f, out);
}

int text_line_count(font *f, int width, const char *s) {
    int n = 0;
    while (f && *s) {
        s = skip_break(s + line_len(f, s, width));
        n++;
    }
    return n;
}

int draw_text_box(canvas *c, font *f, rect box, const char *s, color col, float spacing, int align, int max_lines) {
    return draw_text_box_ex(c, f, box, s, col, spacing, align, max_lines, NULL);
}

int draw_text_box_ex(canvas *c, font *f, rect box, const char *s, color col, float spacing, int align, int max_lines,
                     const text_shadow *sh) {
    if (!f)
        return 0;
    int lh = (int)(font_line_height(f) * spacing), y = box.y, lines = 0;
    while (*s) {
        int n = line_len(f, s, box.w);
        const char *rest = skip_break(s + n);
        bool last = max_lines > 0 && lines + 1 >= max_lines && *rest;
        if (c) {
            char buf[1024];
            int w;
            if (last || text_width_n(f, s, n) > box.w) {
                /* the last line it may have takes the rest, cut to fit */
                char tmp[1024];
                int k = last ? (int)strlen(s) : n;
                if (k > (int)sizeof tmp - 1)
                    k = (int)sizeof tmp - 1;
                for (int i = 0; i < k; i++)
                    tmp[i] = s[i] == '\n' ? ' ' : s[i];
                tmp[k] = 0;
                w = text_ellipsize(f, tmp, box.w, buf, sizeof buf);
            } else {
                snprintf(buf, sizeof buf, "%.*s", n, s);
                w = text_width(f, buf);
            }
            int x = align == TEXT_CENTER ? box.x + (box.w - w) / 2 : align == TEXT_RIGHT ? box.x + box.w - w : box.x;
            if (sh && C_A(sh->col))
                draw_text_shadow(c, f, x + (int)lroundf(sh->dx), y + (int)lroundf(sh->dy), buf, (int)strlen(buf), sh->col,
                                 sh->blur);
            draw_text(c, f, x, y, buf, col);
        }
        y += lh;
        lines++;
        s = rest;
        if (last)
            break;
    }
    return y - box.y;
}

/* Three box blurs in a row: close enough to a Gaussian. */
static void box_blur(uint8_t *a, uint8_t *tmp, int w, int h, int r) {
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) {
            uint8_t *row = a + (size_t)y * (size_t)w;
            int sum = 0;
            for (int x = -r; x <= r; x++)
                sum += x >= 0 && x < w ? row[x] : 0;
            for (int x = 0; x < w; x++) {
                tmp[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)(sum / (2 * r + 1));
                int add = x + r + 1, sub = x - r;
                sum += (add < w ? row[add] : 0) - (sub >= 0 ? row[sub] : 0);
            }
        }
        for (int x = 0; x < w; x++) {
            int sum = 0;
            for (int y = -r; y <= r; y++)
                sum += y >= 0 && y < h ? tmp[(size_t)y * (size_t)w + (size_t)x] : 0;
            for (int y = 0; y < h; y++) {
                a[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)(sum / (2 * r + 1));
                int add = y + r + 1, sub = y - r;
                sum += (add < h ? tmp[(size_t)add * (size_t)w + (size_t)x] : 0) -
                       (sub >= 0 ? tmp[(size_t)sub * (size_t)w + (size_t)x] : 0);
            }
        }
    }
}

void draw_text_shadow(canvas *c, font *f, int x, int y, const char *s, int n, color col, float blur) {
    if (!f || !C_A(col) || n <= 0)
        return;
    if (blur < 0.75f) {
        draw_text_n(c, f, x, y, s, n, col);
        return;
    }
    /* The text in white on black is the shadow's shape; blurred, it is how
     * much of the shadow colour each pixel gets. */
    int r = (int)lroundf(blur / 2);
    if (r < 1)
        r = 1;
    int pad = 3 * r + 2 + (int)(font_px(f) * 0.25f);
    int w = text_width_n(f, s, n) + 2 * pad, h = font_line_height(f) + 2 * pad;
    if (w <= 0 || h <= 0 || x - pad >= c->cx1 || y - pad >= c->cy1 || x - pad + w <= c->cx0 || y - pad + h <= c->cy0)
        return;
    canvas *m = canvas_new(w, h);
    uint8_t *a = malloc((size_t)w * (size_t)h * 2);
    if (!m || !a) {
        canvas_free(m);
        free(a);
        return;
    }
    draw_text_n(m, f, pad, pad, s, n, RGBA(255, 255, 255, 255));
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        a[i] = (uint8_t)C_R(m->px[i]);
    box_blur(a, a + (size_t)w * (size_t)h, w, h, r);
    for (int j = 0; j < h; j++) {
        int py = y - pad + j;
        if (py < c->cy0 || py >= c->cy1)
            continue;
        for (int i = 0; i < w; i++) {
            int px = x - pad + i;
            if (px < c->cx0 || px >= c->cx1)
                continue;
            uint32_t k = a[(size_t)j * (size_t)w + (size_t)i] * C_A(col) / 255;
            if (!k)
                continue;
            uint32_t *p = &c->px[(size_t)py * (size_t)c->w + (size_t)px], ik = 255 - k;
            *p = 0xff000000u | ((C_R(col) * k + C_R(*p) * ik) / 255) << 16 | ((C_G(col) * k + C_G(*p) * ik) / 255) << 8 |
                 ((C_B(col) * k + C_B(*p) * ik) / 255);
        }
    }
    canvas_free(m);
    free(a);
}
