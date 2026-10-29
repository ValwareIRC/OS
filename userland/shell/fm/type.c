#include "fmi.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Typography: weight, slant, letter-spacing, case, text shadows and how
 * text that doesn't fit is wrapped or cut. The stylesheet hands these
 * properties here; layout and drawing ask here for the face to use. */

static const char *ws(const char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* "bold", "semibold", "600", ... as a weight; 0 if it isn't one. */
static int weight_of(const char *v, int current) {
    v = ws(v);
    if (isdigit((unsigned char)*v)) {
        int w = atoi(v);
        return w >= 1 && w <= 1000 ? w : 0;
    }
    static const struct {
        const char *n;
        int w;
    } names[] = {{"thin", 100},       {"hairline", 100},  {"extralight", 200}, {"extra-light", 200},
                 {"ultralight", 200}, {"light", 300},     {"normal", 400},     {"regular", 400},
                 {"book", 400},       {"medium", 500},    {"semibold", 600},   {"semi-bold", 600},
                 {"demibold", 600},   {"extrabold", 800}, {"extra-bold", 800}, {"ultrabold", 800},
                 {"bold", 700},       {"black", 900},     {"heavy", 900}};
    if (!strncasecmp(v, "bolder", 6))
        return current >= 600 ? 900 : 700;
    if (!strncasecmp(v, "lighter", 7))
        return current >= 600 ? 400 : 300;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t l = strlen(names[i].n);
        if (!strncasecmp(v, names[i].n, l) && !isalpha((unsigned char)v[l]))
            return names[i].w;
    }
    return 0;
}

/* The first shadow of a text-shadow list: "x y [blur] colour", the colour
 * first or last. */
static bool parse_shadow(fm_style *st, const char *v) {
    float f[3] = {0, 0, 0};
    int k = 0;
    color c = RGBA(0, 0, 0, 128);
    const char *p = ws(v);
    while (*p && *p != ',') {
        if (isdigit((unsigned char)*p) || *p == '-' || *p == '.' || *p == '+') {
            char *e;
            float x = strtof(p, &e);
            if (e == p)
                return false;
            if (!strncasecmp(e, "px", 2))
                e += 2;
            else if (!strncasecmp(e, "em", 2))
                x *= 16, e += 2;
            if (k < 3)
                f[k++] = x;
            p = e;
        } else {
            /* a colour: up to the closing bracket of rgba(...), else the word */
            const char *e = p;
            if (!strncasecmp(p, "rgb", 3) || !strncasecmp(p, "hsl", 3)) {
                while (*e && *e != ')')
                    e++;
                if (*e)
                    e++;
            } else {
                while (*e && !isspace((unsigned char)*e) && *e != ',')
                    e++;
            }
            char tok[64];
            snprintf(tok, sizeof tok, "%.*s", (int)(e - p), p);
            if (!fm_parse_color(tok, &c))
                return false;
            p = e;
        }
        p = ws(p);
    }
    if (k < 2)
        return false;
    st->tshadow_x = f[0];
    st->tshadow_y = f[1];
    st->tshadow_blur = k > 2 ? f[2] : 0;
    st->tshadow = c;
    return true;
}

int fm_type_apply(fm_style *st, const char *prop, const char *v) {
#define IS(name) (!strcasecmp(prop, name))
    v = ws(v);
    if (IS("font-weight")) {
        int w = weight_of(v, st->font_weight ? st->font_weight : 400);
        if (!w)
            return -1;
        st->font_weight = (short)w;
    } else if (IS("font-style")) {
        st->italic = !strncasecmp(v, "italic", 6) || !strncasecmp(v, "oblique", 7);
        if (!st->italic && strncasecmp(v, "normal", 6))
            return -1;
    } else if (IS("letter-spacing") || IS("tracking")) {
        st->letter_spacing = st->letter_spacing_em = 0;
        if (!strncasecmp(v, "normal", 6))
            return 1;
        char *e;
        float x = strtof(v, &e);
        if (e == v)
            return -1;
        if (!strncasecmp(e, "em", 2) || !strncasecmp(e, "rem", 3))
            st->letter_spacing_em = x;
        else if (*e == '%')
            st->letter_spacing_em = x / 100;
        else
            st->letter_spacing = x;
    } else if (IS("text-shadow")) {
        if (!strncasecmp(v, "none", 4)) {
            st->tshadow = 0;
            return 1;
        }
        if (!parse_shadow(st, v))
            return -1;
    } else if (IS("text-transform")) {
        st->text_case = strstr(v, "upper") ? 'u' : strstr(v, "lower") ? 'l' : strstr(v, "capital") ? 'c' : 0;
        st->uppercase = st->text_case == 'u';
    } else if (IS("text-overflow")) {
        st->clip_text = !strncasecmp(v, "clip", 4);
    } else if (IS("white-space")) {
        st->nowrap = !strncasecmp(v, "nowrap", 6) || !strncasecmp(v, "pre", 3);
    } else if (IS("line-clamp") || IS("-webkit-line-clamp") || IS("max-lines")) {
        st->line_clamp = isdigit((unsigned char)*v) ? atoi(v) : 0;
    } else {
        return 0;
    }
#undef IS
    return 1;
}

bool fm_type_font_word(fm_style *st, const char *word) {
    word = ws(word);
    if (!strncasecmp(word, "italic", 6) || !strncasecmp(word, "oblique", 7)) {
        st->italic = true;
        return true;
    }
    if (!strncasecmp(word, "normal", 6) && !isalpha((unsigned char)word[6]))
        return true; /* "normal" says nothing about which part it is */
    if (!strncasecmp(word, "small-caps", 10))
        return true;
    int w = weight_of(word, st->font_weight ? st->font_weight : 400);
    if (w && !isdigit((unsigned char)*word)) {
        st->font_weight = (short)w;
        return true;
    }
    return false;
}

/* In "600 14px Inter" the first number is a weight: a round hundred with
 * another number still to come. */
bool fm_type_weight_number(fm_style *st, float num, const char *rest) {
    if (num < 100 || num > 900 || fmodf(num, 100) != 0)
        return false;
    bool more = false;
    for (const char *p = rest; *p && !more; p++)
        more = isdigit((unsigned char)*p) != 0;
    if (more)
        st->font_weight = (short)num;
    return more;
}

void fm_type_inherit(fm_style *st, const fm_style *parent) {
    if (!parent)
        return;
    st->font_weight = parent->font_weight;
    st->italic = parent->italic;
    st->text_case = parent->text_case;
    st->letter_spacing = parent->letter_spacing;
    st->letter_spacing_em = parent->letter_spacing_em;
    st->tshadow_x = parent->tshadow_x;
    st->tshadow_y = parent->tshadow_y;
    st->tshadow_blur = parent->tshadow_blur;
    st->tshadow = parent->tshadow;
    st->nowrap = parent->nowrap;
    st->clip_text = parent->clip_text;
    st->line_clamp = parent->line_clamp;
}

font *fmi_text_font(const fm_style *st, float scale) {
    font *base = fm_host_font(st->font, st->font_size * scale);
    float track = (st->letter_spacing + st->letter_spacing_em * st->font_size) * scale;
    return font_with(base, st->font_weight ? st->font_weight : 400, st->italic, track);
}

void fmi_text_case(const fm_style *st, char *s) {
    if (st->text_case == 'l') {
        for (; *s; s++)
            if (*s >= 'A' && *s <= 'Z')
                *s = (char)(*s + 32);
    } else if (st->text_case == 'c') {
        bool start = true;
        for (; *s; s++) {
            if (start && *s >= 'a' && *s <= 'z')
                *s = (char)(*s - 32);
            start = *s == ' ' || *s == '-' || *s == '\n';
        }
    }
}

static text_shadow shadow_for(const fm_style *st, color col, float scale) {
    color sc = st->tshadow;
    /* the shadow fades with the text */
    sc = color_alpha(sc, C_A(col) / 255.0f);
    return (text_shadow){st->tshadow_x * scale, st->tshadow_y * scale, st->tshadow_blur * scale, sc};
}

void fmi_draw_line_in(canvas *c, font *f, rect r, const char *s, color col, const fm_style *st, float scale) {
    char buf[512];
    int tw = text_width(f, s);
    int y = r.y + (r.h - font_line_height(f)) / 2;
    int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
    bool clipped = false;
    if (tw > r.w + 2) { /* doesn't fit: cut with an ellipsis, or at the edge */
        if (st->clip_text) {
            clipped = true;
            c->cx0 = r.x > cx0 ? r.x : cx0;
            c->cx1 = r.x + r.w < cx1 ? r.x + r.w : cx1;
            tw = r.w;
        } else {
            tw = text_ellipsize(f, s, r.w, buf, sizeof buf);
            s = buf;
        }
    }
    int x = r.x;
    if (st->text_align == FM_T_CENTER)
        x = r.x + (r.w - tw) / 2;
    else if (st->text_align == FM_T_END)
        x = r.x + r.w - tw;
    if (C_A(st->tshadow)) {
        text_shadow sh = shadow_for(st, col, scale);
        draw_text_shadow(c, f, x + (int)lroundf(sh.dx), y + (int)lroundf(sh.dy), s, (int)strlen(s), sh.col, sh.blur);
    }
    draw_text(c, f, x, y, s, col);
    if (clipped)
        c->cx0 = cx0, c->cy0 = cy0, c->cx1 = cx1, c->cy1 = cy1;
}

void fmi_draw_block(canvas *c, font *f, rect r, const char *s, color col, const fm_style *st, float scale) {
    int lh = (int)(font_line_height(f) * st->line_height);
    int fit = lh > 0 ? (r.h + lh / 3) / lh : 0; /* lines the box has room for */
    int max = st->line_clamp > 0 ? st->line_clamp : fit < 1 ? 1 : fit;
    if (st->clip_text && st->line_clamp <= 0)
        max = 0;
    int align = st->text_align == FM_T_CENTER ? TEXT_CENTER : st->text_align == FM_T_END ? TEXT_RIGHT : TEXT_LEFT;
    text_shadow sh = shadow_for(st, col, scale);
    draw_text_box_ex(c, f, r, s, col, st->line_height, align, max, C_A(st->tshadow) ? &sh : NULL);
}

int fmi_measure_block(font *f, int width, const char *s, const fm_style *st) {
    return draw_text_box(NULL, f, (rect){0, 0, width, 10000}, s, 0, st->line_height, TEXT_LEFT, st->line_clamp);
}
