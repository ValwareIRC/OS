#define _GNU_SOURCE
#include "fmi.h"
#include "../../lib/json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The stylesheet: CSS syntax, a deliberately small property set, and both
 * short names and the real CSS spellings models already know. */

#define ST_HOVER 1
#define ST_ACTIVE 2
#define ST_OPEN 4
#define ST_FOCUS 8

struct compound {
    char tag[24];
    char id[48];
    char cls[4][32];
    int ncls;
    char attr[24], attr_val[48];
    uint8_t states;
    char comb; /* relation to the compound on its left: ' ' descendant, '>' child */
};

struct selector {
    struct compound parts[8];
    int n;
    int spec;
};

struct decl {
    char prop[32];
    char *value;
    bool important; /* !important: after everything else, the element's own style too */
};

struct fm_rule {
    struct selector sel;
    char text[100]; /* the selector as written, for telling the writer which rule wins */
    struct decl *decls;
    int ndecls;
    int order;
};

/* The OS's defaults: structure only. How anything looks is up to the AI. */
static const char *const builtin_css =
    "facet, desktop { color: #ffffff; font: Inter 16; }\n"
    "row, taskbar, menubar, dock, button, windowlist, search { direction: row; align: center; }\n"
    "column, popover, sheet, panel, window, stack { direction: column; }\n"
    "button { justify: center; gap: 6; }\n"
    "spacer { grow: 1; }\n"
    "taskbar, menubar, dock { gap: 6; }\n"
    "search { padding: 8 14; grow: 1; }\n" /* a field fills the row it sits in */
    "grid { display: grid; columns: 3; gap: 8; }\n"
    "stack { display: stack; }\n"
    "orbit { display: orbit; }\n"
    "applist { gap: 8; align: center; }\n" /* a row, unless it sits in a column (layout.c) */
    "item { direction: column; align: center; gap: 4; padding: 6; }\n"
    "windowlist item { direction: row; gap: 8; padding: 6 10; }\n"
    "text, label, title, clock { }\n"
    "divider { background: rgba(255,255,255,0.2); }\n"
    "slot { grow: 1; padding: 8 20 20 20; }\n"
    "view { grow: 1; }\n"
    "tip { background: rgba(10,12,18,0.85); color: #fff; padding: 4 9; radius: 7; font-size: 13; }\n"
    /* the window frame used when a design has no <template id=\"window\"> */
    "frame { direction: column; background: var(--surface, rgba(20,22,30,0.82)); radius: var(--radius, 14);"
    " blur: 18; shadow: 0 18 50 rgba(0,0,0,0.45); border: 1 rgba(255,255,255,0.12); overflow: hidden;"
    " color: var(--text, #fff); }\n"
    "titlebar { direction: row; align: center; padding: 6 10 6 14; gap: 6; height: 40;"
    " font-family: var(--font-heading, Inter); font-size: 15; }\n"
    "titlebar text { grow: 1; }\n"
    "titlebar button { size: 26; radius: 13; icon-size: 13; justify: center; }\n"
    "titlebar button:hover { background: rgba(255,255,255,0.16); }\n";

/* ---- values ---- */

static const struct {
    const char *name;
    color c;
} named[] = {
    {"transparent", 0x00000000u}, {"none", 0x00000000u}, {"white", 0xffffffffu}, {"black", 0xff000000u},
    {"red", 0xffff0000u},         {"green", 0xff008000u}, {"blue", 0xff0000ffu},  {"gray", 0xff808080u},
    {"grey", 0xff808080u},        {"silver", 0xffc0c0c0u}, {"orange", 0xffffa500u}, {"gold", 0xffffd700u},
    {"navy", 0xff000080u},        {"teal", 0xff008080u},   {"purple", 0xff800080u}, {"pink", 0xffffc0cbu},
    {"yellow", 0xffffff00u},      {"cyan", 0xff00ffffu},   {"magenta", 0xffff00ffu}, {"lime", 0xff00ff00u},
};

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Parses one colour at *s; advances *s. */
static bool parse_color_raw(const char **sp, color *out) {
    const char *s = skip_ws(*sp);
    if (*s == '#') {
        int n = 0;
        unsigned v = 0;
        s++;
        while (isxdigit((unsigned char)s[n]) && n < 8) {
            char c = s[n];
            v = v * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
            n++;
        }
        if (n == 3)
            *out = 0xff000000u | ((v >> 8 & 0xf) * 0x11) << 16 | ((v >> 4 & 0xf) * 0x11) << 8 | (v & 0xf) * 0x11;
        else if (n == 4)
            *out = ((v & 0xf) * 0x11u) << 24 | ((v >> 12 & 0xf) * 0x11) << 16 | ((v >> 8 & 0xf) * 0x11) << 8 |
                   (v >> 4 & 0xf) * 0x11;
        else if (n == 6)
            *out = 0xff000000u | v;
        else if (n == 8)
            *out = (v >> 8) | (v & 0xff) << 24;
        else
            return false;
        *sp = s + n;
        return true;
    }
    if (!strncasecmp(s, "rgb", 3)) {
        const char *p = strchr(s, '(');
        if (!p)
            return false;
        float v[4] = {0, 0, 0, 1};
        int k = 0;
        p++;
        while (*p && *p != ')' && k < 4) {
            p = skip_ws(p);
            char *e;
            v[k] = strtof(p, &e);
            if (e == p)
                break;
            if (*e == '%')
                v[k] = k < 3 ? v[k] * 2.55f : v[k] / 100, e++;
            k++;
            p = e;
            while (*p == ',' || *p == ' ' || *p == '/')
                p++;
        }
        while (*p && *p != ')')
            p++;
        if (*p == ')')
            p++;
        if (k < 3)
            return false;
        float a = v[3] > 1 ? v[3] / 255 : v[3];
        *out = RGBA((int)v[0], (int)v[1], (int)v[2], (int)(a * 255));
        *sp = p;
        return true;
    }
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        size_t l = strlen(named[i].name);
        if (!strncasecmp(s, named[i].name, l) && !isalnum((unsigned char)s[l])) {
            *out = named[i].c;
            *sp = s + l;
            return true;
        }
    }
    return false;
}

/* Light and dark (the shell's theme.c sets these): which colour-scheme
 * blocks apply, and a mapping for every colour the document names when the
 * shell derives the other scheme itself. Shadows read their colour raw: a
 * shadow stays a shadow in either scheme. */
int fm_scheme;
color (*fm_color_filter)(color c);

static bool parse_color(const char **sp, color *out) {
    if (!parse_color_raw(sp, out))
        return false;
    if (fm_color_filter)
        *out = fm_color_filter(*out);
    return true;
}

static float parse_num(const char **sp, bool *ok) {
    const char *s = skip_ws(*sp);
    char *e;
    float v = strtof(s, &e);
    *ok = e != s;
    if (*ok) {
        if (!strncasecmp(e, "px", 2))
            e += 2;
        else if (!strncasecmp(e, "rem", 3) || !strncasecmp(e, "em", 2))
            v *= 16, e += (e[0] == 'r' ? 3 : 2);
        else if (!strncasecmp(e, "ms", 2))
            e += 2;
        else if (*e == 's' && !isalpha((unsigned char)e[1]))
            v *= 1000, e++;
        else if (!strncasecmp(e, "deg", 3))
            e += 3;
        *sp = e;
    }
    return v;
}

static fm_len parse_len(const char *s, bool *ok) {
    s = skip_ws(s);
    *ok = true;
    if (!strncasecmp(s, "auto", 4) || !strncasecmp(s, "fit", 3) || !strncasecmp(s, "max-content", 11))
        return (fm_len){FM_AUTO, 0};
    if (!strncasecmp(s, "full", 4))
        return (fm_len){FM_FULL, 0};
    char *e;
    float v = strtof(s, &e);
    if (e == s) {
        *ok = false;
        return (fm_len){FM_AUTO, 0};
    }
    if (*e == '%')
        return (fm_len){FM_PCT, v};
    if (!strncasecmp(e, "vw", 2) || !strncasecmp(e, "vh", 2))
        return (fm_len){FM_PCT, v}; /* relative to the parent: close enough */
    if (!strncasecmp(e, "em", 2) || !strncasecmp(e, "rem", 3))
        v *= 16;
    return (fm_len){FM_PX, v};
}

static int parse_nums(const char *s, float *out, int max) {
    int k = 0;
    while (*s && k < max) {
        bool ok;
        float v = parse_num(&s, &ok);
        if (!ok)
            break;
        out[k++] = v;
        while (*s == ',' || isspace((unsigned char)*s))
            s++;
    }
    return k;
}

static void four(float *dst, const float *v, int n) {
    if (n == 1)
        dst[0] = dst[1] = dst[2] = dst[3] = v[0];
    else if (n == 2)
        dst[0] = dst[2] = v[0], dst[1] = dst[3] = v[1];
    else if (n == 3)
        dst[0] = v[0], dst[1] = dst[3] = v[1], dst[2] = v[2];
    else if (n >= 4)
        dst[0] = v[0], dst[1] = v[1], dst[2] = v[2], dst[3] = v[3];
}

/* background: none | colour | linear(a, b[, angle]) | linear-gradient(...) |
 * radial(a, b) | radial-gradient(...) */
static bool parse_bg(const char *s, fm_bg *bg) {
    s = skip_ws(s);
    if (!strncasecmp(s, "none", 4)) {
        bg->kind = FM_BG_NONE;
        return true;
    }
    bool linear = !strncasecmp(s, "linear", 6), radial = !strncasecmp(s, "radial", 6);
    if (linear || radial) {
        const char *p = strchr(s, '(');
        if (!p)
            return false;
        p++;
        float angle = 180;
        color cs[6];
        int nc = 0;
        while (*p && *p != ')') {
            p = skip_ws(p);
            const char *before = p;
            color c;
            if (parse_color(&p, &c)) {
                if (nc < 6)
                    cs[nc++] = c;
                /* skip stop positions like "40%" */
                while (*p && *p != ',' && *p != ')')
                    p++;
            } else if (!strncasecmp(p, "to ", 3)) {
                p += 3;
                p = skip_ws(p);
                if (!strncasecmp(p, "bottom", 6)) angle = 180;
                else if (!strncasecmp(p, "top", 3)) angle = 0;
                else if (!strncasecmp(p, "right", 5)) angle = 90;
                else if (!strncasecmp(p, "left", 4)) angle = 270;
                while (*p && *p != ',' && *p != ')')
                    p++;
            } else {
                bool ok;
                float v = parse_num(&p, &ok);
                if (ok)
                    angle = v;
                while (*p && *p != ',' && *p != ')')
                    p++;
            }
            if (*p == ',')
                p++;
            if (p == before)
                p++;
        }
        if (nc == 0)
            return false;
        bg->kind = linear ? FM_BG_LINEAR : FM_BG_RADIAL;
        bg->a = cs[0];
        bg->b = nc > 1 ? cs[nc - 1] : cs[0];
        bg->angle = angle;
        return true;
    }
    color c;
    if (parse_color(&s, &c)) {
        bg->kind = C_A(c) ? FM_BG_SOLID : FM_BG_NONE;
        bg->a = bg->b = c;
        return true;
    }
    return false;
}

static int parse_align(const char *v) {
    v = skip_ws(v);
    if (!strncasecmp(v, "center", 6)) return FM_A_CENTER;
    if (!strncasecmp(v, "start", 5) || !strncasecmp(v, "flex-start", 10) || !strncasecmp(v, "top", 3) ||
        !strncasecmp(v, "left", 4))
        return FM_A_START;
    if (!strncasecmp(v, "end", 3) || !strncasecmp(v, "flex-end", 8) || !strncasecmp(v, "bottom", 6) ||
        !strncasecmp(v, "right", 5))
        return FM_A_END;
    if (!strncasecmp(v, "stretch", 7) || !strncasecmp(v, "normal", 6)) return FM_A_STRETCH;
    return -1;
}

static int parse_justify(const char *v) {
    v = skip_ws(v);
    if (!strncasecmp(v, "space-between", 13) || !strncasecmp(v, "between", 7)) return FM_J_BETWEEN;
    if (!strncasecmp(v, "space-around", 12) || !strncasecmp(v, "around", 6)) return FM_J_AROUND;
    if (!strncasecmp(v, "space-evenly", 12) || !strncasecmp(v, "evenly", 6)) return FM_J_EVENLY;
    if (!strncasecmp(v, "center", 6)) return FM_J_CENTER;
    if (!strncasecmp(v, "end", 3) || !strncasecmp(v, "flex-end", 8) || !strncasecmp(v, "right", 5)) return FM_J_END;
    if (!strncasecmp(v, "start", 5) || !strncasecmp(v, "flex-start", 10) || !strncasecmp(v, "left", 4) ||
        !strncasecmp(v, "normal", 6))
        return FM_J_START;
    return -1;
}

static void parse_font_family(const char *v, char *out, size_t n) {
    v = skip_ws(v);
    if (*v == '"' || *v == '\'') {
        char q = *v++;
        const char *e = strchr(v, q);
        snprintf(out, n, "%.*s", e ? (int)(e - v) : (int)strlen(v), v);
        return;
    }
    size_t l = 0;
    while (v[l] && v[l] != ',' && v[l] != ';')
        l++;
    while (l && isspace((unsigned char)v[l - 1]))
        l--;
    snprintf(out, n, "%.*s", (int)l, v);
}

/* Applies one declaration. Returns false for unknown properties or values
 * it can't read (the caller reports those to the AI). */
/* ---- borders ---- */

static bool border_style_of(const char *v, char out[4]) {
    static const char *const names[] = {"solid", "dashed", "dotted", "double", "inset", "outset", "ridge", "groove"};
    static const char codes[] = {0, 'd', 'o', '2', 'i', 'u', 'r', 'g'};
    for (size_t i = 0; i < 8; i++)
        if (!strncasecmp(v, names[i], strlen(names[i]))) {
            out[0] = codes[i];
            return true;
        }
    if (!strncasecmp(v, "none", 4) || !strncasecmp(v, "hidden", 6)) {
        out[0] = 0;
        return true;
    }
    return false;
}

static void sides_from_all(fm_style *st) {
    if (st->bsides)
        return;
    for (int k = 0; k < 4; k++)
        st->bside[k] = st->border_w, st->bcol[k] = st->border;
    st->bsides = true;
}

/* "2px solid #fff" for every side (side < 0) or one */
static void border_words(fm_style *st, const char *p, int side) {
    while (*p) {
        color c;
        char s4[4];
        const char *q = p;
        if (parse_color(&q, &c)) {
            if (side < 0)
                st->border = c;
            else
                st->bcol[side] = c;
            p = q;
        } else if (isdigit((unsigned char)*p) || *p == '.') {
            bool k;
            float w = parse_num(&p, &k);
            if (side < 0)
                st->border_w = w;
            else
                st->bside[side] = w;
        } else {
            if (border_style_of(p, s4))
                st->bstyle = s4[0];
            if (side >= 0 && !strncasecmp(p, "none", 4))
                st->bside[side] = 0;
            while (*p && !isspace((unsigned char)*p))
                p++;
        }
        p = skip_ws(p);
    }
}

static int numbers(const char *v, float *out, int max) {
    int n = 0;
    const char *p = skip_ws(v);
    while (*p && n < max) {
        if (!(isdigit((unsigned char)*p) || *p == '.' || *p == '-'))
            break;
        bool k;
        out[n++] = parse_num(&p, &k);
        p = skip_ws(p);
    }
    return n;
}

static void expand4(float *w, int n) { /* CSS's 1-4 values: top right bottom left */
    if (n == 2)
        w[2] = w[0], w[3] = w[1];
    else if (n == 3)
        w[3] = w[1];
}

static int side_prop(const char *prop) {
    if (strstr(prop, "radius"))
        return -1;
    static const char *const s[] = {"border-top", "border-right", "border-bottom", "border-left"};
    for (int k = 0; k < 4; k++) {
        size_t l = strlen(s[k]);
        if (!strncmp(prop, s[k], l) && (prop[l] == 0 || prop[l] == '-'))
            return k;
    }
    return -1;
}

static bool apply(fm_style *st, const char *prop, const char *v) {
    bool ok = true;
    float nums[4];
    int n;
    int typo = fm_type_apply(st, prop, v); /* weight, slant, spacing, shadows: fm/type.c */
    if (typo)
        return typo > 0;
#define IS(name) (!strcasecmp(prop, name))
    if (IS("display")) {
        const char *dv = skip_ws(v);
        st->hidden = !strncasecmp(dv, "none", 4);
        if (strstr(dv, "grid"))
            st->mode = FM_GRID;
        else if (strstr(dv, "stack") || strstr(dv, "layer"))
            st->mode = FM_STACK;
        else if (strstr(dv, "orbit") || strstr(dv, "radial") || strstr(dv, "ring"))
            st->mode = FM_ORBIT;
        else if (strstr(dv, "flex") || strstr(dv, "block") || strstr(dv, "row") || strstr(dv, "column"))
            st->mode = FM_FLEX;
        if (strstr(dv, "column") && (st->dir_set = true))
            st->dir = FM_COLUMN;
    } else if (IS("direction") || IS("flex-direction")) {
        st->dir = strstr(v, "column") || strstr(v, "vertical") ? FM_COLUMN : FM_ROW;
        st->dir_set = true;
    } else if (IS("wrap") || IS("flex-wrap")) {
        st->wrap = !strstr(v, "nowrap") && !strstr(v, "false");
    } else if (IS("position")) {
        st->absolute = strstr(v, "absolute") || strstr(v, "fixed");
    } else if (IS("left")) st->left = parse_len(v, &ok);
    else if (IS("top")) st->top = parse_len(v, &ok);
    else if (IS("right")) st->right = parse_len(v, &ok);
    else if (IS("bottom")) st->bottom = parse_len(v, &ok);
    else if (IS("width")) st->width = parse_len(v, &ok);
    else if (IS("height")) st->height = parse_len(v, &ok);
    else if (IS("min-width")) st->min_w = parse_len(v, &ok);
    else if (IS("min-height")) st->min_h = parse_len(v, &ok);
    else if (IS("max-width")) st->max_w = parse_len(v, &ok);
    else if (IS("max-height")) st->max_h = parse_len(v, &ok);
    else if (IS("size")) {
        st->width = st->height = parse_len(v, &ok);
    } else if (IS("grow") || IS("flex-grow") || IS("flex")) {
        st->grow = strtof(skip_ws(v), NULL);
    } else if (IS("padding")) {
        n = parse_nums(v, nums, 4);
        if ((ok = n > 0))
            four(st->pad, nums, n);
    } else if (IS("padding-top")) st->pad[0] = strtof(v, NULL);
    else if (IS("padding-right")) st->pad[1] = strtof(v, NULL);
    else if (IS("padding-bottom")) st->pad[2] = strtof(v, NULL);
    else if (IS("padding-left")) st->pad[3] = strtof(v, NULL);
    else if (IS("margin")) {
        n = parse_nums(v, nums, 4);
        if ((ok = n > 0))
            four(st->margin, nums, n);
    } else if (IS("margin-top")) st->margin[0] = strtof(v, NULL);
    else if (IS("margin-right")) st->margin[1] = strtof(v, NULL);
    else if (IS("margin-bottom")) st->margin[2] = strtof(v, NULL);
    else if (IS("margin-left")) st->margin[3] = strtof(v, NULL);
    else if (IS("gap") || IS("row-gap") || IS("column-gap")) {
        st->gap = strtof(skip_ws(v), NULL);
    } else if (IS("align") || IS("align-items")) {
        int a = parse_align(v);
        if ((ok = a >= 0))
            st->align = (enum fm_align)a;
    } else if (IS("align-self")) {
        int a = parse_align(v);
        if ((ok = a >= 0))
            st->self_align = (enum fm_align)a, st->self_align_set = true;
    } else if (IS("justify") || IS("justify-content")) {
        int j = parse_justify(v);
        if ((ok = j >= 0))
            st->justify = (enum fm_justify)j;
    } else if (IS("overflow") || IS("overflow-y")) {
        st->overflow = strstr(v, "scroll") || strstr(v, "auto") ? FM_OV_SCROLL
                       : strstr(v, "hidden") || strstr(v, "clip") ? FM_OV_CLIP
                                                                   : FM_OV_VISIBLE;
    } else if (IS("background") || IS("background-color") || IS("background-image") || IS("bg")) {
        ok = parse_bg(v, &st->bg);
    } else if (IS("blur") || IS("backdrop-blur")) {
        st->blur = strtof(skip_ws(v), NULL);
    } else if (IS("backdrop-filter") || IS("-webkit-backdrop-filter")) {
        const char *b = strstr(v, "blur(");
        if (b)
            st->blur = strtof(b + 5, NULL);
        const char *sa = strstr(v, "saturate(");
        if (sa) {
            float s = strtof(sa + 9, NULL);
            st->saturate = s > 5 ? s / 100 : s;
        }
    } else if (IS("saturate")) {
        st->saturate = strtof(v, NULL);
    } else if (IS("border")) {
        const char *p = skip_ws(v);
        st->bsides = false;
        if (!strncasecmp(p, "none", 4) || (!strncmp(p, "0", 1) && !isdigit((unsigned char)p[1]) && p[1] != '.')) {
            st->border_w = 0;
        } else {
            border_words(st, p, -1);
        }
    } else if (IS("border-style")) {
        char s4[4];
        ok = border_style_of(skip_ws(v), s4);
        if (ok)
            st->bstyle = s4[0];
    } else if (IS("border-width")) {
        float w[4];
        int n = numbers(v, w, 4);
        ok = n > 0;
        if (n == 1) {
            st->border_w = w[0];
            if (st->bsides)
                for (int k = 0; k < 4; k++)
                    st->bside[k] = w[0];
        } else if (n > 1) {
            sides_from_all(st);
            expand4(w, n);
            for (int k = 0; k < 4; k++)
                st->bside[k] = w[k];
            st->border_w = fmaxf(fmaxf(w[0], w[1]), fmaxf(w[2], w[3]));
        }
    } else if (IS("border-color")) {
        color cs[4];
        int n = 0;
        const char *p = skip_ws(v);
        while (*p && n < 4 && parse_color(&p, &cs[n]))
            n++, p = skip_ws(p);
        ok = n > 0;
        if (n == 1) {
            st->border = cs[0];
            if (st->bsides)
                for (int k = 0; k < 4; k++)
                    st->bcol[k] = cs[0];
        } else if (n > 1) {
            sides_from_all(st);
            if (n == 2)
                cs[2] = cs[0], cs[3] = cs[1];
            else if (n == 3)
                cs[3] = cs[1];
            for (int k = 0; k < 4; k++)
                st->bcol[k] = cs[k];
            st->border = cs[0];
        }
    } else if (side_prop(prop) >= 0) { /* border-top, border-left-color, border-bottom-width, ... */
        int k = side_prop(prop);
        const char *what = strchr(prop + 7, '-');
        sides_from_all(st);
        if (!what) {
            border_words(st, skip_ws(v), k);
        } else if (!strcmp(what, "-width")) {
            st->bside[k] = strtof(v, NULL);
        } else if (!strcmp(what, "-color")) {
            ok = parse_color(&v, &st->bcol[k]);
        } else if (!strcmp(what, "-style")) {
            char s4[4];
            ok = border_style_of(skip_ws(v), s4);
            if (ok)
                st->bstyle = s4[0];
        } else {
            ok = false;
        }
        st->border_w = fmaxf(fmaxf(st->bside[0], st->bside[1]), fmaxf(st->bside[2], st->bside[3]));
    } else if (IS("border-image") || IS("border-gradient")) {
        ok = parse_bg(v, &st->border_bg);
    } else if (IS("radius") || IS("border-radius") || IS("corner-radius")) {
        if (strstr(v, "%")) { /* 50% is a pill or circle */
            float pc = strtof(v, NULL);
            st->radius[0] = st->radius[1] = st->radius[2] = st->radius[3] = pc >= 50 ? 9999 : pc * 2;
        } else {
            n = parse_nums(v, nums, 4);
            if ((ok = n > 0))
                four(st->radius, nums, n);
        }
    } else if (IS("shadow") || IS("box-shadow")) {
        const char *p = skip_ws(v);
        if (!strncasecmp(p, "none", 4)) {
            st->shadow_blur = 0;
            st->shadow = 0;
        } else {
            bool outer = false;
            st->ninset = 0;
            while (*p) { /* each shadow of the list */
                float f[4];
                int k = 0;
                color c = RGBA(0, 0, 0, 90);
                bool inset = false;
                while (*p && *p != ',') {
                    const char *q = p;
                    if (!strncasecmp(p, "inset", 5)) {
                        inset = true;
                        p += 5;
                    } else if (parse_color_raw(&q, &c)) {
                        p = q;
                    } else if (isdigit((unsigned char)*p) || *p == '-' || *p == '.') {
                        bool kk;
                        float val = parse_num(&p, &kk);
                        if (k < 4)
                            f[k++] = val;
                    } else {
                        while (*p && !isspace((unsigned char)*p) && *p != ',')
                            p++;
                    }
                    p = skip_ws(p);
                }
                if (inset && st->ninset < 6) {
                    st->inset_x[st->ninset] = k > 0 ? f[0] : 0;
                    st->inset_y[st->ninset] = k > 1 ? f[1] : 0;
                    st->inset_c[st->ninset++] = c;
                } else if (!inset && !outer) {
                    outer = true;
                    st->shadow_x = k > 0 ? f[0] : 0;
                    st->shadow_y = k > 1 ? f[1] : 0;
                    st->shadow_blur = k > 2 ? f[2] : 12;
                    st->shadow = c;
                }
                if (*p == ',')
                    p = skip_ws(p + 1);
            }
            if (!outer)
                st->shadow_blur = 0, st->shadow = 0;
        }
    } else if (IS("glow")) {
        const char *p = skip_ws(v);
        if (!strncasecmp(p, "none", 4)) {
            st->glow_size = 0;
        } else {
            st->glow_size = 16;
            while (*p) {
                const char *q = p;
                color c;
                if (parse_color(&q, &c)) {
                    st->glow = c;
                    p = q;
                } else if (isdigit((unsigned char)*p) || *p == '.') {
                    bool kk;
                    st->glow_size = parse_num(&p, &kk);
                } else {
                    p++;
                }
                p = skip_ws(p);
            }
        }
    } else if (IS("opacity")) {
        st->opacity = strtof(v, NULL);
        if (strchr(v, '%'))
            st->opacity /= 100;
    } else if (IS("color") || IS("foreground")) {
        ok = parse_color(&v, &st->color);
    } else if (IS("font")) {
        /* "Inter 16", "16px Inter", "bold 14px 'Space Grotesk'" */
        st->font_weight = 0; /* like CSS, the shorthand resets what it can say */
        st->italic = false;
        const char *p = skip_ws(v);
        char fam[32] = "", unq[96];
        /* the whole shorthand in quotes: font: "Sora 96" */
        size_t pl = strlen(p);
        if (pl >= 2 && (p[0] == '"' || p[0] == '\'') && p[pl - 1] == p[0] && memchr(p + 1, ' ', pl - 2)) {
            snprintf(unq, sizeof unq, "%.*s", (int)(pl - 2), p + 1);
            /* "Space Grotesk" is a family, "Sora 96" is a shorthand */
            bool has_num = false;
            for (const char *q = unq; *q; q++)
                has_num |= isdigit((unsigned char)*q) != 0;
            if (has_num)
                p = unq;
        }
        while (*p) {
            if (isdigit((unsigned char)*p)) {
                bool kk;
                float sz = parse_num(&p, &kk);
                if (fm_type_weight_number(st, sz, p))
                    ;
                else if (sz >= 6)
                    st->font_size = sz;
                if (*p == '/') { /* line-height after a slash */
                    p++;
                    parse_num(&p, &kk);
                }
            } else if (fm_type_font_word(st, p)) { /* bold, italic, 600, ... */
                while (*p && !isspace((unsigned char)*p))
                    p++;
            } else {
                parse_font_family(p, fam, sizeof fam);
                /* "Sora 96": family first, size after */
                char *sp = strrchr(fam, ' ');
                if (sp && isdigit((unsigned char)sp[1])) {
                    float sz = strtof(sp + 1, NULL);
                    if (sz >= 6)
                        st->font_size = sz;
                    *sp = 0;
                }
                break;
            }
            p = skip_ws(p);
        }
        if (fam[0])
            snprintf(st->font, sizeof st->font, "%s", fam);
    } else if (IS("place") || IS("dock")) {
        /* Really attributes, but models often write them as style. */
        const char *pv = skip_ws(v);
        char *dst = IS("place") ? st->place : st->dock;
        size_t k = 0;
        while (pv[k] && k < sizeof st->place - 1 && (isalpha((unsigned char)pv[k]) || pv[k] == '-'))
            k++;
        ok = k > 0;
        snprintf(dst, sizeof st->place, "%.*s", (int)k, pv);
    } else if (IS("font-family")) {
        parse_font_family(v, st->font, sizeof st->font);
    } else if (IS("font-size")) {
        bool kk;
        const char *p = v;
        st->font_size = parse_num(&p, &kk);
        ok = kk;
    } else if (IS("text-align")) {
        st->text_align = strstr(v, "center") ? FM_T_CENTER : (strstr(v, "right") || strstr(v, "end")) ? FM_T_END : FM_T_START;
    } else if (IS("line-height")) {
        float lh = strtof(v, NULL);
        st->line_height = lh > 4 ? lh / 16 : lh;
    } else if (IS("text-transform")) {
        st->uppercase = strstr(v, "upper") != NULL;
    } else if (IS("writing-mode")) {
        v = skip_ws(v);
        if (!strncasecmp(v, "sideways-lr", 11))
            st->text_rot = -90; /* bottom to top, like a book's spine on the left */
        else if (!strncasecmp(v, "vertical", 8) || !strncasecmp(v, "sideways-rl", 11) || !strncasecmp(v, "tb", 2))
            st->text_rot = 90;
        else if (!strncasecmp(v, "horizontal", 10) || !strncasecmp(v, "lr", 2))
            st->text_rot = 0;
        else
            ok = false;
    } else if (IS("text-rotate") || IS("rotate")) { /* "rotate: -90deg": the text in it turns */
        float a = strtof(skip_ws(v), NULL);
        int q = ((int)lroundf(a / 90.0f) % 4 + 4) % 4;
        ok = fabsf(a - 90.0f * lroundf(a / 90.0f)) < 1;
        st->text_rot = (short)(q == 1 ? 90 : q == 2 ? 180 : q == 3 ? -90 : 0);
    } else if (IS("transform")) {
        const char *rot = strstr(v, "rotate(");
        if (rot) { /* quarter turns turn the text; anything else is reported (no_effect) */
            float a = strtof(rot + 7, NULL);
            if (fabsf(a - 90.0f * lroundf(a / 90.0f)) < 1) {
                int q = ((int)lroundf(a / 90.0f) % 4 + 4) % 4;
                st->text_rot = (short)(q == 1 ? 90 : q == 2 ? 180 : q == 3 ? -90 : 0);
            }
        }
        const char *sc = strstr(v, "scale(");
        if (sc)
            st->scale = strtof(sc + 6, NULL);
        const char *tr = strstr(v, "translate(");
        if (tr) {
            const char *p = tr + 10;
            bool kk;
            st->tx = parse_num(&p, &kk);
            while (*p == ',' || *p == ' ')
                p++;
            st->ty = parse_num(&p, &kk);
        }
        const char *ty = strstr(v, "translateY(");
        if (ty) {
            char *e;
            float f = strtof(ty + 11, &e);
            if (*e == '%')
                st->ty_pct = f; /* of its own height */
            else
                st->ty = f;
        }
        const char *tx = strstr(v, "translateX(");
        if (tx) {
            char *e;
            float f = strtof(tx + 11, &e);
            if (*e == '%')
                st->tx_pct = f;
            else
                st->tx = f;
        }
        if (tr) { /* translate(-50%, -50%) */
            char *e;
            float f = strtof(tr + 10, &e);
            if (*e == '%') {
                st->tx_pct = f, st->tx = 0;
                while (*e == '%' || *e == ',' || *e == ' ')
                    e++;
                float g = strtof(e, &e);
                if (*e == '%')
                    st->ty_pct = g, st->ty = 0;
            }
        }
    } else if (IS("scale")) {
        st->scale = strtof(v, NULL);
    } else if (IS("translate")) {
        n = parse_nums(v, nums, 2);
        st->tx = n > 0 ? nums[0] : 0;
        st->ty = n > 1 ? nums[1] : 0;
    } else if (IS("transition")) {
        /* first time in the value: "200ms", "all .3s ease" */
        const char *p = v;
        while (*p && !(isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))))
            p++;
        bool kk;
        float t = parse_num(&p, &kk);
        st->transition = kk ? t : 150;
    } else if (IS("z") || IS("z-index")) {
        st->z = strtof(v, NULL);
    } else if (IS("icon-size")) {
        st->icon_size = strtof(v, NULL);
    } else if (IS("icon-style")) {
        snprintf(st->icon_style, sizeof st->icon_style, "%s", skip_ws(v));
    } else if (IS("icon-shape")) {
        snprintf(st->icon_shape, sizeof st->icon_shape, "%s", skip_ws(v));
    } else if (IS("columns") || IS("grid-columns") || IS("grid-template-columns")) {
        const char *rp = strstr(v, "repeat(");
        if (rp)
            st->columns = atoi(rp + 7);
        else if (isdigit((unsigned char)*skip_ws(v)) && !strstr(v, "px") && !strstr(v, "fr"))
            st->columns = atoi(skip_ws(v));
        else { /* count tracks: "1fr 1fr 1fr" */
            int k = 0;
            for (const char *p = skip_ws(v); *p;) {
                k++;
                while (*p && !isspace((unsigned char)*p))
                    p++;
                p = skip_ws(p);
            }
            st->columns = k;
        }
    } else if (IS("orbit") || IS("orbit-radius")) {
        st->orbit = strtof(v, NULL);
    } else if (IS("font-weight") || IS("letter-spacing") || IS("cursor") || IS("pointer-events") || IS("filter") ||
               IS("text-shadow") || IS("white-space") || IS("font-style") || IS("user-select") || IS("outline") ||
               IS("box-sizing") || IS("text-overflow") || IS("content") || IS("animation") || IS("will-change") ||
               IS("align-content") || IS("flex-shrink") || IS("flex-basis") || IS("object-fit") ||
               IS("text-decoration") || IS("vertical-align") || IS("list-style")) {
        /* understood and deliberately ignored */
    } else {
        return false;
    }
#undef IS
    return ok;
}

/* "unset" (or initial, revert): what came before for this property is dropped */
static bool is_unset(const char *v) {
    v = skip_ws(v);
    return !strncasecmp(v, "unset", 5) || !strncasecmp(v, "initial", 7) || !strncasecmp(v, "revert", 6);
}

/* Properties taken without complaint (designs copied from the web are full of them) that draw nothing here:
 * the one who wrote them is told, with what works instead. */
static const char *no_effect(const char *prop, const char *v) {
    static const char *const t[][2] = {
        {"filter", "colours, opacity or saturate"},       {"outline", "border or box-shadow"},
        {"text-decoration", "a border-bottom"},           {"animation", "transition"},
        {"cursor", ""},                                   {"pointer-events", ""},
        {"user-select", ""},                              {"box-sizing", "(sizes here already include padding)"},
        {"content", "text in the markup"},                {"will-change", ""},
        {"align-content", "align-items"},                 {"flex-shrink", "min-width / max-width"},
        {"flex-basis", "width"},                          {"object-fit", ""},
        {"vertical-align", "align-items on the row"},     {"list-style", ""},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcasecmp(prop, t[i][0]))
            return t[i][1];
    if (!strcasecmp(prop, "transform") && strstr(v, "rotate(")) {
        float a = strtof(strstr(v, "rotate(") + 7, NULL);
        if (fabsf(a - 90.0f * lroundf(a / 90.0f)) >= 1)
            return "quarter turns only (rotate(90deg), rotate(-90deg), 180deg): they turn the text in it";
    }
    if (!strcasecmp(prop, "transform") && (strstr(v, "skew") || strstr(v, "matrix")))
        return "scale(), translate() and quarter-turn rotate()";
    return NULL;
}

/* ---- selectors ---- */

static bool parse_compound(const char **sp, struct compound *c) {
    const char *s = *sp;
    memset(c, 0, sizeof *c);
    bool any = false;
    while (*s) {
        if (*s == '*') {
            s++;
            any = true;
        } else if (isalpha((unsigned char)*s) || *s == '_') {
            int k = 0;
            while ((isalnum((unsigned char)*s) || *s == '-' || *s == '_') && k < 23)
                c->tag[k++] = (char)tolower((unsigned char)*s++);
            any = true;
        } else if (*s == '#') {
            s++;
            int k = 0;
            while ((isalnum((unsigned char)*s) || *s == '-' || *s == '_') && k < 47)
                c->id[k++] = *s++;
            any = true;
        } else if (*s == '.') {
            s++;
            int k = 0;
            char tmp[32] = {0};
            while ((isalnum((unsigned char)*s) || *s == '-' || *s == '_') && k < 31)
                tmp[k++] = *s++;
            if (c->ncls < 4)
                memcpy(c->cls[c->ncls++], tmp, sizeof tmp);
            any = true;
        } else if (*s == '[') {
            s++;
            int k = 0;
            while (*s && *s != '=' && *s != ']' && k < 23)
                c->attr[k++] = *s++;
            if (*s == '=') {
                s++;
                if (*s == '"' || *s == '\'')
                    s++;
                k = 0;
                while (*s && *s != ']' && *s != '"' && *s != '\'' && k < 47)
                    c->attr_val[k++] = *s++;
            }
            while (*s && *s != ']')
                s++;
            if (*s)
                s++;
            any = true;
        } else if (*s == ':') {
            s++;
            if (*s == ':')
                s++;
            if (!strncasecmp(s, "hover", 5)) c->states |= ST_HOVER;
            else if (!strncasecmp(s, "active", 6) || !strncasecmp(s, "pressed", 7)) c->states |= ST_ACTIVE;
            else if (!strncasecmp(s, "open", 4) || !strncasecmp(s, "checked", 7)) c->states |= ST_OPEN;
            else if (!strncasecmp(s, "focus", 5)) c->states |= ST_FOCUS;
            else if (!strncasecmp(s, "root", 4)) strcpy(c->tag, "facet");
            while (isalnum((unsigned char)*s) || *s == '-')
                s++;
            any = true;
        } else {
            break;
        }
    }
    *sp = s;
    return any;
}

static bool parse_selector(const char *text, struct selector *sel) {
    memset(sel, 0, sizeof *sel);
    const char *s = skip_ws(text);
    char comb = ' ';
    while (*s && sel->n < 8) {
        struct compound c;
        if (!parse_compound(&s, &c))
            return false;
        c.comb = comb;
        sel->parts[sel->n++] = c;
        s = skip_ws(s);
        comb = ' ';
        if (*s == '>' || *s == '+' || *s == '~') {
            comb = *s == '>' ? '>' : ' ';
            s = skip_ws(s + 1);
        }
    }
    if (*s || sel->n == 0)
        return false;
    for (int i = 0; i < sel->n; i++) {
        struct compound *c = &sel->parts[i];
        sel->spec += (c->id[0] ? 100 : 0) + 10 * (c->ncls + (c->attr[0] ? 1 : 0) + __builtin_popcount(c->states)) +
                     (c->tag[0] ? 1 : 0);
    }
    return true;
}

static bool has_class(const fm_node *n, const char *cls) {
    const char *list = fm_attr_get(n, "class");
    if (!list)
        return false;
    size_t l = strlen(cls);
    for (const char *p = list; (p = strstr(p, cls)); p += l)
        if ((p == list || p[-1] == ' ') && (p[l] == 0 || p[l] == ' '))
            return true;
    return false;
}

static bool match_compound(const fm_node *n, const struct compound *c) {
    if (n->kind != FM_ELEM)
        return false;
    if (c->tag[0] && !same_tag(c->tag, n->tag))
        return false;
    if (c->id[0] && strcmp(c->id, n->id))
        return false;
    for (int i = 0; i < c->ncls; i++)
        if (!has_class(n, c->cls[i]))
            return false;
    if (c->attr[0]) {
        const char *v = fm_attr_get(n, c->attr);
        if (!v || (c->attr_val[0] && strcmp(v, c->attr_val)))
            return false;
    }
    if ((c->states & ST_HOVER) && !n->hover)
        return false;
    if ((c->states & ST_ACTIVE) && !n->active)
        return false;
    if ((c->states & ST_OPEN) && !n->open)
        return false;
    if ((c->states & ST_FOCUS) && !n->focus)
        return false;
    return true;
}

static bool match_from(const fm_node *n, const struct selector *s, int i) {
    if (!match_compound(n, &s->parts[i]))
        return false;
    if (i == 0)
        return true;
    if (s->parts[i].comb == '>')
        return n->parent && match_from(n->parent, s, i - 1);
    for (const fm_node *p = n->parent; p; p = p->parent)
        if (match_from(p, s, i - 1))
            return true;
    return false;
}

/* ---- stylesheet loading ---- */

void fm_css_free_rules(fm_doc *d) {
    for (int i = 0; i < d->nrules; i++) {
        for (int k = 0; k < d->rules[i].ndecls; k++)
            free(d->rules[i].decls[k].value);
        free(d->rules[i].decls);
    }
    free(d->rules);
    d->rules = NULL;
    d->nrules = 0;
}

const char *fm_var(const fm_doc *d, const char *name) {
    for (int i = 0; i < d->nvars; i++)
        if (!strcmp(d->vars[i][0], name))
            return d->vars[i][1];
    return NULL;
}

static void set_var(fm_doc *d, const char *name, const char *value) {
    for (int i = 0; i < d->nvars; i++)
        if (!strcmp(d->vars[i][0], name)) {
            free(d->vars[i][1]);
            d->vars[i][1] = strdup(value);
            return;
        }
    if (d->nvars < 64) {
        d->vars[d->nvars][0] = strdup(name);
        d->vars[d->nvars][1] = strdup(value);
        d->nvars++;
    }
}

/* Replaces var(--x[, fallback]) with the variable's value. */
static char *resolve_vars(const fm_doc *d, const char *v, int depth) {
    sbuf b;
    sb_init(&b);
    const char *p = v;
    while (*p) {
        const char *s = strstr(p, "var(");
        if (!s) {
            sb_add(&b, p);
            break;
        }
        sb_addn(&b, p, (size_t)(s - p));
        const char *q = s + 4;
        int paren = 1;
        const char *e = q;
        while (*e && paren) {
            if (*e == '(') paren++;
            if (*e == ')') paren--;
            if (paren)
                e++;
        }
        char inner[160];
        snprintf(inner, sizeof inner, "%.*s", (int)(e - q), q);
        char *comma = strchr(inner, ',');
        char *fallback = NULL;
        if (comma) {
            *comma = 0;
            fallback = comma + 1;
        }
        char name[64];
        sscanf(inner, " %63s", name);
        const char *val = fm_var(d, name);
        if (!val)
            val = fallback ? fallback : "";
        if (depth < 4) {
            char *r = resolve_vars(d, val, depth + 1);
            sb_add(&b, r);
            free(r);
        } else {
            sb_add(&b, val);
        }
        p = *e ? e + 1 : e;
    }
    return b.p ? b.p : strdup("");
}

static char *strip_comments(const char *css) {
    sbuf b;
    sb_init(&b);
    for (const char *p = css; *p;) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        sb_addn(&b, p, 1);
        p++;
    }
    return b.p ? b.p : strdup("");
}

static int line_at(const char *base, const char *p) {
    int l = 1;
    for (const char *c = base; c < p; c++)
        l += *c == '\n';
    return l;
}

/* Which scheme an at-rule is for: "@dark", "@light", or "@media
 * (prefers-color-scheme: dark|light)". 2 dark, 1 light, 0 neither. */
static int at_scheme(const char *at) {
    at = skip_ws(at);
    if (!strncasecmp(at, "@dark", 5))
        return 2;
    if (!strncasecmp(at, "@light", 6))
        return 1;
    const char *m = strcasestr(at, "prefers-color-scheme");
    if (!m)
        return 0;
    return strcasestr(m, "dark") ? 2 : strcasestr(m, "light") ? 1 : 0;
}

/* The brace that closes the block opened at `open` (blocks may nest). */
static const char *block_end(const char *open) {
    int depth = 0;
    for (const char *p = open; *p; p++) {
        if (*p == '{')
            depth++;
        else if (*p == '}' && --depth == 0)
            return p;
    }
    return open + strlen(open);
}

/* Two passes: variables first (so they may be used before they are
 * defined), then the rules. */
static void load_sheet(fm_doc *d, const char *css, bool report, int *order, bool vars_only) {
    char *clean = strip_comments(css);
    const char *later[8][2];
    int nlater = 0;
    for (int pass = 0; pass < (vars_only ? 1 : 2); pass++) {
        const char *p = clean;
        while (*p) {
            const char *open = strchr(p, '{');
            if (!open)
                break;
            const char *close = strchr(open, '}');
            if (!close)
                close = open + strlen(open);
            char selectors[512];
            snprintf(selectors, sizeof selectors, "%.*s", (int)(open - p), p);
            if (!strncmp(skip_ws(selectors), "@", 1)) {
                /* An at-rule's whole block, nested rules and all. A block
                 * for the colour scheme the shell is in applies (its
                 * variables override :root's); anything else is skipped. */
                const char *end = block_end(open);
                int scheme = at_scheme(selectors);
                if (scheme && scheme == fm_scheme && pass == 0 && nlater < 8) {
                    /* it wins wherever it sits: read after the rest */
                    later[nlater][0] = open + 1;
                    later[nlater++][1] = end;
                }
                p = *end ? end + 1 : end;
                continue;
            }
            char *body = strndup(open + 1, (size_t)(close - open - 1));
            /* declarations */
            struct decl decls[64];
            int nd = 0;
            for (char *save, *tok = strtok_r(body, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
                char *colon = strchr(tok, ':');
                if (!colon)
                    continue;
                *colon = 0;
                char prop[32];
                if (sscanf(tok, " %31s", prop) != 1)
                    continue;
                for (char *c = prop; *c; c++)
                    *c = (char)tolower((unsigned char)*c);
                char *val = colon + 1;
                while (isspace((unsigned char)*val))
                    val++;
                size_t vl = strlen(val);
                while (vl && isspace((unsigned char)val[vl - 1]))
                    val[--vl] = 0;
                char *imp = strstr(val, "!important");
                if (imp) {
                    *imp = 0;
                    for (char *e = imp; e > val && isspace((unsigned char)e[-1]);)
                        *--e = 0;
                }
                if (prop[0] == '-' && prop[1] == '-') {
                    if (pass == 0)
                        set_var(d, prop, val);
                    continue;
                }
                if (pass == 1 && nd < 64) {
                    snprintf(decls[nd].prop, sizeof decls[nd].prop, "%s", prop);
                    decls[nd].value = strdup(val);
                    decls[nd].important = imp != NULL;
                    nd++;
                }
            }
            free(body);
            if (pass == 1 && nd) {
                /* resolve variables now that all are known; check values */
                for (int k = 0; k < nd; k++) {
                    char *r = resolve_vars(d, decls[k].value, 0);
                    free(decls[k].value);
                    decls[k].value = r;
                    fm_style scratch;
                    memset(&scratch, 0, sizeof scratch);
                    /* `terminal { }` styles the terminal app, which has
                     * properties of its own (palette, caret-color, ...) */
                    bool term = !strncasecmp(skip_ws(selectors), "terminal", 8) &&
                                !skip_ws(skip_ws(selectors) + 8)[0];
                    const char *instead;
                    if (report && !term && !is_unset(r) && !apply(&scratch, decls[k].prop, r))
                        fm_report(d, line_at(clean, p), false, "can't use \"%s: %s\"", decls[k].prop, r);
                    else if (report && !term && (instead = no_effect(decls[k].prop, r)))
                        fm_report(d, line_at(clean, p), false, "\"%s: %.40s\" has no effect here%s%s",
                                  decls[k].prop, r, instead[0] ? "; use " : "", instead);
                }
                for (char *save, *sel = strtok_r(selectors, ",", &save); sel; sel = strtok_r(NULL, ",", &save)) {
                    struct selector s;
                    if (!parse_selector(sel, &s)) {
                        if (report)
                            fm_report(d, line_at(clean, p), false, "can't read selector \"%s\"", sel);
                        continue;
                    }
                    d->rules = realloc(d->rules, sizeof *d->rules * (size_t)(d->nrules + 1));
                    struct fm_rule *r = &d->rules[d->nrules++];
                    r->sel = s;
                    {
                        const char *t0 = skip_ws(sel);
                        size_t tl = strlen(t0);
                        while (tl && isspace((unsigned char)t0[tl - 1]))
                            tl--;
                        snprintf(r->text, sizeof r->text, "%.*s", (int)tl, t0);
                    }
                    r->order = (*order)++;
                    r->ndecls = nd;
                    r->decls = malloc(sizeof *r->decls * (size_t)nd);
                    for (int k = 0; k < nd; k++) {
                        r->decls[k] = decls[k];
                        r->decls[k].value = strdup(decls[k].value);
                    }
                }
                for (int k = 0; k < nd; k++)
                    free(decls[k].value);
            }
            p = *close ? close + 1 : close;
        }
        for (int i = 0; i < nlater; i++) {
            char *inner = strndup(later[i][0], (size_t)(later[i][1] - later[i][0]));
            load_sheet(d, inner, report && pass == 1, order, pass == 0);
            free(inner);
        }
    }
    free(clean);
}

static int rule_cmp(const void *a, const void *b) {
    const struct fm_rule *x = a, *y = b;
    if (x->sel.spec != y->sel.spec)
        return x->sel.spec - y->sel.spec;
    return x->order - y->order;
}

static bool doc_has(fm_node *n, const char *cls, const char *id) {
    for (; n; n = n->next) {
        if (n->kind == FM_ELEM) {
            if (id && !strcmp(n->id, id))
                return true;
            const char *c = cls ? fm_attr_get(n, "class") : NULL;
            if (c) {
                size_t l = strlen(cls);
                for (const char *p = strstr(c, cls); p; p = strstr(p + 1, cls))
                    if ((p == c || p[-1] == ' ') && (p[l] == 0 || p[l] == ' '))
                        return true;
            }
            if (doc_has(n->first, cls, id))
                return true;
        }
    }
    return false;
}

/* A common slip: ".orbit" for the element whose id is "orbit". When no
 * element has that class but one has that id, the rule means the id. */
static void fix_class_for_id(fm_doc *d) {
    fm_node *top = d->root;
    for (int r = 0; r < d->nrules; r++)
        for (int p = 0; p < d->rules[r].sel.n; p++) {
            struct compound *c = &d->rules[r].sel.parts[p];
            if (c->ncls != 1 || c->id[0] || doc_has(top, c->cls[0], NULL) || !doc_has(top, NULL, c->cls[0]))
                continue;
            memcpy(c->id, c->cls[0], sizeof c->cls[0]); /* both NUL-terminated; id is larger */
            c->ncls = 0;
        }
}

void fm_css_load(fm_doc *d) {
    fm_css_free_rules(d);
    /* Variables start over too: a switch of colour scheme must not leave
     * the other scheme's values behind. */
    for (int i = 0; i < d->nvars; i++) {
        free(d->vars[i][0]);
        free(d->vars[i][1]);
    }
    d->nvars = 0;
    int order = 0;
    load_sheet(d, d->css ? d->css : "", false, &order, true); /* variables first: defaults use them */
    load_sheet(d, builtin_css, false, &order, false);
    load_sheet(d, d->css ? d->css : "", true, &order, false);
    fix_class_for_id(d);
    qsort(d->rules, (size_t)d->nrules, sizeof *d->rules, rule_cmp);
}

void fm_css_patch(fm_doc *d, const char *css) {
    sbuf b;
    sb_init(&b);
    sb_add(&b, d->css ? d->css : "");
    sb_add(&b, "\n");
    sb_add(&b, css);
    free(d->css);
    d->css = b.p;
    fm_css_load(d);
}

/* ---- cascade ---- */

static void initial(fm_style *st, const fm_style *parent) {
    memset(st, 0, sizeof *st);
    st->opacity = 1;
    st->scale = 1;
    st->saturate = 1;
    st->line_height = parent ? parent->line_height : 1.3f;
    st->transition = 160;
    st->align = FM_A_STRETCH;
    st->border = RGBA(255, 255, 255, 60);
    st->glow = RGBA(255, 255, 255, 120);
    if (parent) { /* inherited properties */
        st->color = parent->color;
        memcpy(st->font, parent->font, sizeof st->font);
        st->font_size = parent->font_size;
        st->text_align = parent->text_align;
        st->text_rot = parent->text_rot;
        st->uppercase = parent->uppercase;
        memcpy(st->icon_style, parent->icon_style, sizeof st->icon_style);
        memcpy(st->icon_shape, parent->icon_shape, sizeof st->icon_shape);
        fm_type_inherit(st, parent);
    } else {
        st->color = RGBA(255, 255, 255, 255);
        snprintf(st->font, sizeof st->font, "Inter");
        st->font_size = 16;
    }
}

static void apply_inline(fm_style *st, const char *style) {
    char *copy = strdup(style);
    for (char *save, *tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon)
            continue;
        *colon = 0;
        char prop[32];
        if (sscanf(tok, " %31s", prop) == 1)
            apply(st, prop, colon + 1);
    }
    free(copy);
}

static void style_node(fm_doc *d, fm_node *n, const fm_style *parent) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM) {
            n->st = parent ? *parent : n->st;
            continue;
        }
        initial(&n->st, parent);
        /* what applies to it, in cascade order; "unset" drops a property's earlier declarations, and
         * !important ones come after everything (its own style="" too) */
        static const struct decl *got[768];
        int ng = 0;
        bool any_unset = false, any_important = false;
        for (int i = 0; i < d->nrules; i++) {
            struct fm_rule *r = &d->rules[i];
            if (!match_from(n, &r->sel, r->sel.n - 1))
                continue;
            for (int k = 0; k < r->ndecls; k++) {
                if (ng < (int)(sizeof got / sizeof got[0]))
                    got[ng++] = &r->decls[k];
                else
                    apply(&n->st, r->decls[k].prop, r->decls[k].value);
                any_unset |= is_unset(r->decls[k].value);
                any_important |= r->decls[k].important;
            }
        }
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                const char *hid = fm_attr_get(n, "hidden");
                if (hid)
                    n->st.hidden = strcasecmp(hid, "false") && strcmp(hid, "0");
                const char *inl = fm_attr_get(n, "style");
                if (inl) {
                    char *res = resolve_vars(d, inl, 0);
                    apply_inline(&n->st, res);
                    free(res);
                }
                if (!any_important)
                    break;
            }
            for (int i = 0; i < ng; i++) {
                const struct decl *x = got[i];
                if (x->important != (pass == 1) || is_unset(x->value))
                    continue;
                bool dropped = false;
                for (int j = i + 1; any_unset && j < ng && !dropped; j++)
                    dropped = is_unset(got[j]->value) && !strcmp(got[j]->prop, x->prop) &&
                              (got[j]->important || !x->important);
                if (!dropped)
                    apply(&n->st, x->prop, x->value);
            }
        }
        if (n->st.opacity < 0) n->st.opacity = 0;
        if (n->st.opacity > 1) n->st.opacity = 1;
        if (n->st.scale <= 0.05f) n->st.scale = 1;
        if (n->st.font_size < 6) n->st.font_size = 6;
        if (n->st.font_size > 220) n->st.font_size = 220;
        style_node(d, n->first, &n->st);
    }
}

void fm_style_tree(fm_doc *d) {
    if (d->root)
        style_node(d, d->root, NULL);
}

/* Re-styles one subtree (window frames are styled per window). */
void fmi_style_subtree(fm_doc *d, fm_node *n) {
    const fm_style *parent = n->parent && n->parent->kind == FM_ELEM ? &n->parent->st : NULL;
    fm_node *saved = n->next;
    n->next = NULL;
    style_node(d, n, parent);
    n->next = saved;
}

bool fm_parse_color(const char *s, color *out) { return parse_color(&s, out); }
bool fm_parse_color_raw(const char *s, color *out) { return parse_color_raw(&s, out); }

/* After `sel { ... }` was added: for each property it sets, a rule that still wins on node n (more specific,
 * or !important) is named in out. Returns how many. */
int fm_css_who_wins(fm_doc *d, fm_node *n, const char *sel, char *out, size_t cap) {
    out[0] = 0;
    if (!n)
        return 0;
    int mine = -1;
    for (int i = 0; i < d->nrules; i++)
        if (!strcmp(d->rules[i].text, sel) && (mine < 0 || d->rules[i].order > d->rules[mine].order))
            mine = i;
    if (mine < 0 || !match_from(n, &d->rules[mine].sel, d->rules[mine].sel.n - 1))
        return 0;
    const struct fm_rule *me = &d->rules[mine];
    int found = 0;
    size_t l = 0;
    for (int k = 0; k < me->ndecls; k++) {
        const struct decl *x = &me->decls[k];
        for (int i = 0; i < d->nrules; i++) {
            const struct fm_rule *r = &d->rules[i];
            if (i == mine || !match_from(n, &r->sel, r->sel.n - 1))
                continue;
            for (int j = 0; j < r->ndecls; j++) {
                const struct decl *y = &r->decls[j];
                if (strcmp(y->prop, x->prop) || is_unset(y->value))
                    continue;
                bool beats = y->important ? !x->important || i > mine : !x->important && i > mine;
                if (beats && l < cap - 1) {
                    l += (size_t)snprintf(out + l, cap - l, "%s\"%s { %s }\" %s", found ? "; " : "", r->text,
                                          y->prop, y->important ? "(!important)" : "(more specific)");
                    found++;
                }
            }
        }
    }
    return found;
}

static void squash_ws(const char *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    bool sp = false;
    for (size_t i = 0; i < n && o < cap - 1; i++) {
        if (isspace((unsigned char)in[i])) {
            sp = o > 0;
            continue;
        }
        if (sp && o < cap - 1)
            out[o++] = ' ';
        sp = false;
        out[o++] = in[i];
    }
    out[o] = 0;
}

/* Takes `selector`'s rules out of the design's own sheet: all of them, or (props: "height; width") only
 * those properties. A rule written for several selectors keeps the others. Returns how many rules changed. */
int fm_css_unstyle(fm_doc *d, const char *selector, const char *props) {
    if (!d->css)
        return 0;
    char want[128];
    squash_ws(selector, strlen(selector), want, sizeof want);
    sbuf o;
    sb_init(&o);
    int changed = 0;
    const char *p = d->css;
    while (*p) {
        const char *open = strchr(p, '{');
        if (!open) {
            sb_add(&o, p);
            break;
        }
        const char *head = p;
        while (head < open && isspace((unsigned char)*head))
            sb_addn(&o, head++, 1);
        if (*head == '@' || (head + 1 < open && head[0] == '/' && head[1] == '*')) { /* at-rules stay as they are */
            const char *end = *head == '@' ? block_end(open) : strchr(open, '}');
            end = end && *end ? end + 1 : open + strlen(open);
            sb_addn(&o, head, (size_t)(end - head));
            p = end;
            continue;
        }
        const char *close = strchr(open, '}');
        if (!close)
            close = open + strlen(open);
        /* the selectors, one by one */
        char others[600] = "";
        bool hit = false;
        for (const char *s = head; s < open;) {
            const char *comma = memchr(s, ',', (size_t)(open - s));
            const char *e = comma ? comma : open;
            char one[128];
            squash_ws(s, (size_t)(e - s), one, sizeof one);
            if (!strcmp(one, want))
                hit = true;
            else if (one[0])
                snprintf(others + strlen(others), sizeof others - strlen(others), "%s%s", others[0] ? ", " : "", one);
            s = comma ? comma + 1 : open;
        }
        const char *after = *close ? close + 1 : close;
        if (!hit) {
            sb_addn(&o, head, (size_t)(after - head));
            p = after;
            continue;
        }
        changed++;
        char *body = strndup(open + 1, (size_t)(close - open - 1));
        if (others[0])
            sb_printf(&o, "%s {%s}", others, body);
        if (props && *props) { /* the rest of its declarations stay, for it alone */
            sbuf keep;
            sb_init(&keep);
            for (char *save, *t = strtok_r(body, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
                char name[40] = "";
                const char *colon = strchr(t, ':');
                if (!colon || sscanf(t, " %39[^: \t\n]", name) != 1)
                    continue;
                bool drop = false;
                char list[300];
                snprintf(list, sizeof list, "%s", props);
                for (char *s2, *q = strtok_r(list, ";, ", &s2); q && !drop; q = strtok_r(NULL, ";, ", &s2))
                    drop = !strcasecmp(q, name);
                if (!drop)
                    sb_printf(&keep, " %s;", skip_ws(t));
            }
            if (keep.p)
                sb_printf(&o, "%s%s {%s }", others[0] ? "\n" : "", want, keep.p);
            free(keep.p);
        }
        free(body);
        p = after;
    }
    free(d->css);
    d->css = o.p ? o.p : strdup("");
    fm_css_load(d);
    return changed;
}

/* A value from a rule for something that is not an element (`terminal {
 * background: #000 }`): the last plain `tag { }` rule that sets prop. */
const char *fm_rule_value(const fm_doc *d, const char *tag, const char *prop) {
    const char *found = NULL;
    int best = -1;
    for (int i = 0; i < d->nrules; i++) {
        const struct fm_rule *r = &d->rules[i];
        const struct compound *c = &r->sel.parts[0];
        if (r->sel.n != 1 || strcasecmp(c->tag, tag) || c->id[0] || c->ncls || c->attr[0] || c->states)
            continue;
        for (int k = 0; k < r->ndecls; k++)
            if (!strcmp(r->decls[k].prop, prop) && r->order > best) {
                found = r->decls[k].value;
                best = r->order;
            }
    }
    return found;
}
