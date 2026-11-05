#define _GNU_SOURCE
#include "fmi.h"
#include "../../lib/json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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
    char attr_op; /* '=' exact, '^' starts, '$' ends, '*' contains, '~' a word of it, 0 any value */
    uint8_t states;
    char comb; /* relation to the compound on its left: ' ' descendant, '>' child, '+' next sibling, '~' a later sibling */
    /* structure: :first-child (nth 0n+1), :last-child (from the end), :nth-child(an+b), :only-child, :empty */
    bool nth, nth_last, only, empty;
    int nth_a, nth_b;
    /* :not(x), x simple: a tag, #id, .class, [attr] or a state */
    bool has_not;
    char not_tag[24], not_id[48], not_cls[32], not_attr[24];
    uint8_t not_states;
    char pseudo; /* ::before 'b', ::after 'a' */
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

#define KF_STOPS 16
struct fm_keyframes {
    char name[40];
    int n;
    float at[KF_STOPS];          /* 0..1, ascending */
    struct decl *decls[KF_STOPS];
    int nd[KF_STOPS];
};

bool fm_animating;
int fm_media_w = 1600, fm_media_h = 900;

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
    "b, strong { font-weight: bold; } i, em, cite { font-style: italic; } u, ins { text-decoration: underline; }\n"
    "s, del { text-decoration: line-through; } code, kbd { font-family: JetBrainsMono; }\n"
    "mark { background: #ffe066; color: #1a1a1a; padding: 0 2; } small { font-size: 12; }\n"
    "a { text-decoration: underline; }\n"
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
    "titlebar button:hover { background: rgba(255,255,255,0.16); }\n"
    /* a right-click menu, until the design has styled its own (menus.c has the companion write them): its
     * colours, font and corners, nothing more */
    "menu { direction: column; align: stretch; padding: 6; gap: 2; min-width: 210; background: var(--surface, rgba(20,22,30,0.94));"
    " color: var(--text, #fff); radius: var(--radius, 12); border: 1 rgba(255,255,255,0.12);"
    " shadow: 0 14 36 rgba(0,0,0,0.42); font-family: var(--font-body, Inter); font-size: 14; }\n"
    "menu button { justify: start; padding: 7 12; radius: 8; gap: 18; }\n"
    "menu button:hover, menu button:focus { background: rgba(255,255,255,0.13); }\n"
    "menu button.danger { color: #ff8a80; }\n"
    "menu button.off { opacity: 0.4; }\n"
    "menu button.quiet { font-size: 12; opacity: 0.72; padding: 5 12; }\n"
    "menu text.keys { opacity: 0.6; font-size: 12; }\n"
    "menu text.note { font-size: 12; opacity: 0.72; padding: 4 12 6 12; }\n"
    "menu divider { height: 1; margin: 4 6; }\n";

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
    bool repeating = !strncasecmp(s, "repeating-", 10);
    const char *k = repeating ? s + 10 : s;
    bool linear = !strncasecmp(k, "linear", 6), radial = !strncasecmp(k, "radial", 6), conic = !strncasecmp(k, "conic", 5);
    if (linear || radial || conic) {
        const char *p = strchr(s, '(');
        if (!p)
            return false;
        p++;
        fm_bg g;
        memset(&g, 0, sizeof g);
        g.kind = linear ? FM_BG_LINEAR : radial ? FM_BG_RADIAL : FM_BG_CONIC;
        g.repeating = repeating;
        g.angle = linear ? 180 : 0;
        g.cx = g.cy = 0.5f;
        float pos[FM_STOPS];
        bool has_pos[FM_STOPS];
        /* the first part may say which way (linear), what shape and where (radial), where from (conic) */
        while (*p && *p != ')') {
            p = skip_ws(p);
            const char *part = p;
            int depth = 0;
            while (*p && !(depth == 0 && (*p == ',' || *p == ')'))) {
                if (*p == '(') depth++;
                if (*p == ')') depth--;
                p++;
            }
            char item[160];
            snprintf(item, sizeof item, "%.*s", (int)(p - part), part);
            if (*p == ',')
                p++;
            const char *q = skip_ws(item);
            color c;
            const char *cq = q;
            if (parse_color(&cq, &c)) {
                if (g.nstops < FM_STOPS) {
                    g.stop_c[g.nstops] = c;
                    cq = skip_ws(cq);
                    has_pos[g.nstops] = false;
                    if (*cq) { /* "40%" (or px, taken as a share of 300) */
                        bool ok;
                        float v = parse_num(&cq, &ok);
                        if (ok) {
                            pos[g.nstops] = *cq == '%' ? v / 100 : v / 300;
                            has_pos[g.nstops] = true;
                        }
                    }
                    g.nstops++;
                }
                continue;
            }
            if (g.nstops) /* words after the colours begin: nothing more to read here */
                continue;
            if (linear) {
                if (!strncasecmp(q, "to ", 3)) {
                    q = skip_ws(q + 3);
                    bool t = strcasestr(q, "top") != NULL, b = strcasestr(q, "bottom") != NULL;
                    bool l = strcasestr(q, "left") != NULL, r = strcasestr(q, "right") != NULL;
                    g.angle = t ? (l ? 315 : r ? 45 : 0) : b ? (l ? 225 : r ? 135 : 180) : l ? 270 : r ? 90 : 180;
                } else {
                    bool ok;
                    float v = parse_num(&q, &ok);
                    if (ok)
                        g.angle = !strncasecmp(q, "turn", 4) ? v * 360 : !strncasecmp(q, "rad", 3) ? v * 57.29578f : v;
                }
            } else {
                if (radial && strcasestr(q, "circle"))
                    g.circle = true;
                const char *from = conic ? strcasestr(q, "from") : NULL;
                if (from) {
                    const char *f2 = skip_ws(from + 4);
                    bool ok;
                    float v = parse_num(&f2, &ok);
                    if (ok)
                        g.angle = !strncasecmp(f2, "turn", 4) ? v * 360 : v;
                }
                const char *at = strcasestr(q, "at ");
                if (at) {
                    const char *a2 = skip_ws(at + 3);
                    float xy[2] = {0.5f, 0.5f};
                    for (int i = 0; i < 2 && *a2; i++) {
                        a2 = skip_ws(a2);
                        if (!strncasecmp(a2, "left", 4) || !strncasecmp(a2, "top", 3)) xy[i] = 0;
                        else if (!strncasecmp(a2, "right", 5) || !strncasecmp(a2, "bottom", 6)) xy[i] = 1;
                        else if (!strncasecmp(a2, "center", 6)) xy[i] = 0.5f;
                        else {
                            bool ok;
                            float v = parse_num(&a2, &ok);
                            if (ok)
                                xy[i] = v / 100;
                        }
                        while (*a2 && !isspace((unsigned char)*a2))
                            a2++;
                    }
                    g.cx = xy[0], g.cy = xy[1];
                }
            }
        }
        if (g.nstops == 0)
            return false;
        /* positions not given are spread evenly between the ones that are, as CSS does */
        if (!has_pos[0])
            pos[0] = 0, has_pos[0] = true;
        if (!has_pos[g.nstops - 1])
            pos[g.nstops - 1] = g.nstops > 1 ? 1 : 0, has_pos[g.nstops - 1] = true;
        for (int i = 1; i < g.nstops; i++) {
            if (has_pos[i]) {
                if (pos[i] < pos[i - 1])
                    pos[i] = pos[i - 1];
                continue;
            }
            int j = i;
            while (!has_pos[j])
                j++;
            for (int m = i; m < j; m++)
                pos[m] = pos[i - 1] + (pos[j] - pos[i - 1]) * (float)(m - i + 1) / (float)(j - i + 1);
            i = j - 1;
        }
        for (int i = 0; i < g.nstops; i++)
            g.stop_p[i] = pos[i];
        g.a = g.stop_c[0];
        g.b = g.stop_c[g.nstops - 1];
        *bg = g;
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
/* ---- animation ---- */

static char *resolve_vars(const fm_doc *d, const char *v, int depth);

static float time_ms(const char *v) {
    char *e;
    float x = strtof(v, &e);
    return !strncasecmp(e, "ms", 2) ? x : x * 1000;
}

/* one word of an animation: a timing, a direction, a fill mode; true if it was one */
static bool anim_word(fm_style *st, const char *w) {
    static const struct { const char *w; char what, v; } t[] = {
        {"linear", 'e', 'l'}, {"ease-in-out", 'e', 'b'}, {"ease-in", 'e', 'i'}, {"ease-out", 'e', 'o'}, {"ease", 'e', 'e'},
        {"alternate-reverse", 'd', 'A'}, {"alternate", 'd', 'a'}, {"reverse", 'd', 'r'}, {"normal", 'd', 'n'},
        {"forwards", 'f', 'f'}, {"backwards", 'f', 'b'}, {"both", 'f', 'B'}, {"none", 'f', 'n'}};
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strncasecmp(w, t[i].w, strlen(t[i].w))) {
            if (t[i].what == 'e') st->anim_ease = t[i].v;
            else if (t[i].what == 'd') st->anim_dir = t[i].v;
            else st->anim_fill = t[i].v;
            return true;
        }
    if (!strncasecmp(w, "steps(", 6)) {
        st->anim_ease = 's';
        st->anim_steps = atoi(w + 6);
        return true;
    }
    if (!strncasecmp(w, "cubic-bezier(", 13)) {
        st->anim_ease = 'c';
        sscanf(w + 13, "%f , %f , %f , %f", &st->anim_bez[0], &st->anim_bez[1], &st->anim_bez[2], &st->anim_bez[3]);
        return true;
    }
    if (!strncasecmp(w, "step-end", 8) || !strncasecmp(w, "step-start", 10)) {
        st->anim_ease = 's';
        st->anim_steps = 1;
        return true;
    }
    return false;
}

/* "pulse 2s ease-in-out 0.5s infinite alternate": the first time is the duration, the second the delay */
static void anim_shorthand(fm_style *st, const char *v) {
    st->anim_name[0] = 0;
    st->anim_dur = st->anim_delay = 0;
    st->anim_count = 1;
    st->anim_dir = 'n', st->anim_ease = 'e', st->anim_fill = 'n';
    int times = 0;
    for (const char *p = skip_ws(v); *p;) {
        char w[48];
        int k = 0;
        int depth = 0;
        while (*p && (depth || (*p != ' ' && *p != ',')) && k < 47) {
            if (*p == '(') depth++;
            if (*p == ')') depth--;
            w[k++] = *p++;
        }
        w[k] = 0;
        while (*p == ' ' || *p == ',')
            p++;
        if (!w[0])
            break;
        if (isdigit((unsigned char)w[0]) || w[0] == '.') {
            char *e;
            float x = strtof(w, &e);
            if (!strncasecmp(e, "ms", 2) || *e == 's') {
                if (times++ == 0) st->anim_dur = time_ms(w);
                else st->anim_delay = time_ms(w);
            } else {
                st->anim_count = x;
            }
        } else if (!strncasecmp(w, "infinite", 8)) {
            st->anim_count = -1;
        } else if (!anim_word(st, w) && !st->anim_name[0]) {
            snprintf(st->anim_name, sizeof st->anim_name, "%s", w);
        }
    }
}

/* cubic-bezier(x1, y1, x2, y2) at x = t (Newton, then bisection) */
static float bezier(float x1, float y1, float x2, float y2, float t) {
    float u = t;
    for (int i = 0; i < 8; i++) {
        float x = 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u - t;
        float dx = 3 * (1 - u) * (1 - u) * x1 + 6 * (1 - u) * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (fabsf(x) < 1e-4f)
            break;
        if (fabsf(dx) < 1e-6f)
            break;
        u -= x / dx;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
    }
    return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u;
}

static float ease(const fm_style *st, float t) {
    switch (st->anim_ease) {
    case 'l': return t;
    case 'i': return bezier(0.42f, 0, 1, 1, t);
    case 'o': return bezier(0, 0, 0.58f, 1, t);
    case 'b': return bezier(0.42f, 0, 0.58f, 1, t);
    case 's': return st->anim_steps > 0 ? floorf(t * st->anim_steps) / st->anim_steps : t;
    case 'c': return bezier(st->anim_bez[0], st->anim_bez[1], st->anim_bez[2], st->anim_bez[3], t);
    default: return bezier(0.25f, 0.1f, 0.25f, 1, t); /* "ease" */
    }
}

static color mixc(color a, color b, float t) { return color_mix(a, b, t); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }

/* What moves between two keyframes: colours, sizes, offsets, light; the rest switches halfway */
static void blend(fm_style *a, const fm_style *b, float t) {
    fm_style keep = *a;
    if (t >= 0.5f) /* what can't blend switches over halfway */
        *a = *b;
    a->opacity = mixf(keep.opacity, b->opacity, t);
    a->scale = mixf(keep.scale, b->scale, t);
    a->tx = mixf(keep.tx, b->tx, t), a->ty = mixf(keep.ty, b->ty, t);
    a->tx_pct = mixf(keep.tx_pct, b->tx_pct, t), a->ty_pct = mixf(keep.ty_pct, b->ty_pct, t);
    a->rotate = mixf(keep.rotate, b->rotate, t);
    a->color = mixc(keep.color, b->color, t);
    a->border = mixc(keep.border, b->border, t);
    a->border_w = mixf(keep.border_w, b->border_w, t);
    a->shadow = mixc(keep.shadow, b->shadow, t);
    a->shadow_x = mixf(keep.shadow_x, b->shadow_x, t), a->shadow_y = mixf(keep.shadow_y, b->shadow_y, t);
    a->shadow_blur = mixf(keep.shadow_blur, b->shadow_blur, t);
    a->glow = mixc(keep.glow, b->glow, t);
    a->glow_size = mixf(keep.glow_size, b->glow_size, t);
    a->blur = mixf(keep.blur, b->blur, t);
    a->letter_spacing = mixf(keep.letter_spacing, b->letter_spacing, t);
    a->font_size = mixf(keep.font_size, b->font_size, t);
    for (int k = 0; k < 4; k++)
        a->radius[k] = mixf(keep.radius[k], b->radius[k], t);
    if (keep.bg.kind == b->bg.kind || keep.bg.kind == FM_BG_NONE || b->bg.kind == FM_BG_NONE) {
        fm_bg x = t >= 0.5f ? b->bg : keep.bg;
        color ka = keep.bg.kind == FM_BG_NONE ? color_alpha(b->bg.a, 0) : keep.bg.a;
        color kb = keep.bg.kind == FM_BG_NONE ? color_alpha(b->bg.b, 0) : keep.bg.b;
        color ba = b->bg.kind == FM_BG_NONE ? color_alpha(keep.bg.a, 0) : b->bg.a;
        color bb = b->bg.kind == FM_BG_NONE ? color_alpha(keep.bg.b, 0) : b->bg.b;
        if (x.kind == FM_BG_NONE)
            x = keep.bg.kind == FM_BG_NONE ? b->bg : keep.bg;
        x.a = mixc(ka, ba, t), x.b = mixc(kb, bb, t);
        x.angle = mixf(keep.bg.angle, b->bg.angle, t);
        if (keep.bg.nstops == b->bg.nstops)
            for (int i = 0; i < x.nstops; i++) {
                x.stop_c[i] = mixc(keep.bg.stop_c[i], b->bg.stop_c[i], t);
                x.stop_p[i] = mixf(keep.bg.stop_p[i], b->bg.stop_p[i], t);
            }
        else if (x.nstops == 0 && x.kind == FM_BG_SOLID)
            x.a = x.b = mixc(ka, ba, t);
        a->bg = x;
    }
    if (keep.width.unit == FM_PX && b->width.unit == FM_PX)
        a->width.v = mixf(keep.width.v, b->width.v, t);
    if (keep.height.unit == FM_PX && b->height.unit == FM_PX)
        a->height.v = mixf(keep.height.v, b->height.v, t);
    if (keep.width.unit == FM_PCT && b->width.unit == FM_PCT)
        a->width.v = mixf(keep.width.v, b->width.v, t);
}

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Runs n's animation on its style, now */
static bool apply(fm_style *st, const char *prop, const char *v);
static void animate(fm_doc *d, fm_node *n) {
    fm_style *st = &n->st;
    if (!st->anim_name[0] || st->anim_dur <= 0) {
        n->anim_was[0] = 0;
        return;
    }
    struct fm_keyframes *k = NULL;
    for (int i = 0; i < d->nkf && !k; i++)
        if (!strcmp(d->kf[i].name, st->anim_name))
            k = &d->kf[i];
    if (!k)
        return;
    double now = mono_ms();
    if (strcmp(n->anim_was, st->anim_name)) { /* a new animation starts from now */
        snprintf(n->anim_was, sizeof n->anim_was, "%s", st->anim_name);
        n->anim_t0 = now;
    }
    double e = now - n->anim_t0 - st->anim_delay;
    if (e < 0 && st->anim_fill != 'b' && st->anim_fill != 'B') {
        fm_animating = true;
        return;
    }
    if (e < 0)
        e = 0;
    double iters = e / st->anim_dur;
    bool done = st->anim_count >= 0 && iters >= st->anim_count;
    if (done && st->anim_fill != 'f' && st->anim_fill != 'B')
        return;
    if (done)
        iters = st->anim_count > 0 ? st->anim_count - 1e-6 : 0;
    else
        fm_animating = true;
    int it = (int)iters;
    float p = (float)(iters - it);
    if (done && st->anim_count > 0 && fabs(st->anim_count - floor(st->anim_count)) < 1e-6)
        p = 1;
    bool rev = st->anim_dir == 'r' || (st->anim_dir == 'a' && it % 2) || (st->anim_dir == 'A' && !(it % 2));
    if (rev)
        p = 1 - p;
    /* the keyframes either side of p; a missing 0% or 100% is the element's own style */
    float at0 = 0, at1 = 1;
    int i0 = -1, i1 = -1;
    for (int i = 0; i < k->n; i++) {
        if (k->at[i] <= p && (i0 < 0 || k->at[i] >= at0))
            i0 = i, at0 = k->at[i];
        if (k->at[i] >= p && (i1 < 0 || k->at[i] <= at1))
            i1 = i, at1 = k->at[i];
    }
    if (i0 < 0)
        at0 = 0;
    if (i1 < 0)
        at1 = 1;
    fm_style a = *st, b = *st;
    if (i0 >= 0)
        for (int j = 0; j < k->nd[i0]; j++)
            apply(&a, k->decls[i0][j].prop, k->decls[i0][j].value);
    if (i1 >= 0)
        for (int j = 0; j < k->nd[i1]; j++)
            apply(&b, k->decls[i1][j].prop, k->decls[i1][j].value);
    float t = at1 > at0 ? (p - at0) / (at1 - at0) : 0;
    t = ease(st, t < 0 ? 0 : t > 1 ? 1 : t);
    blend(&a, &b, t);
    /* the animation itself stays as it was */
    memcpy(a.anim_name, st->anim_name, sizeof a.anim_name);
    *st = a;
}

/* @keyframes NAME { from { } 50% { } to { } } */
static void read_keyframes(fm_doc *d, const char *head, const char *body, const char *end) {
    const char *nm = skip_ws(head + 10);
    char name[40];
    int k = 0;
    while (*nm && !isspace((unsigned char)*nm) && *nm != '{' && k < 39)
        name[k++] = *nm++;
    name[k] = 0;
    if (!name[0])
        return;
    d->kf = realloc(d->kf, sizeof *d->kf * (size_t)(d->nkf + 1));
    struct fm_keyframes *f = &d->kf[d->nkf++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    for (const char *p = body; p < end;) {
        const char *o = memchr(p, '{', (size_t)(end - p));
        if (!o)
            break;
        const char *c = memchr(o, '}', (size_t)(end - o));
        if (!c)
            break;
        char sel[64];
        snprintf(sel, sizeof sel, "%.*s", (int)(o - p) < 63 ? (int)(o - p) : 63, p);
        char *decl_text = strndup(o + 1, (size_t)(c - o - 1));
        /* "0%, 50%" share a block */
        for (char *save, *w = strtok_r(sel, ",", &save); w && f->n < KF_STOPS; w = strtok_r(NULL, ",", &save)) {
            const char *s = skip_ws(w);
            float at = !strncasecmp(s, "from", 4) ? 0 : !strncasecmp(s, "to", 2) ? 1 : strtof(s, NULL) / 100;
            int i = f->n++;
            f->at[i] = at;
            f->decls[i] = malloc(sizeof(struct decl) * 32);
            char *dt = strdup(decl_text);
            for (char *s2, *t = strtok_r(dt, ";", &s2); t && f->nd[i] < 32; t = strtok_r(NULL, ";", &s2)) {
                char *colon = strchr(t, ':');
                if (!colon)
                    continue;
                *colon = 0;
                struct decl *x = &f->decls[i][f->nd[i]];
                memset(x, 0, sizeof *x);
                if (sscanf(t, " %31s", x->prop) != 1)
                    continue;
                char *v = colon + 1;
                while (isspace((unsigned char)*v))
                    v++;
                x->value = resolve_vars(d, v, 0);
                f->nd[i]++;
            }
            free(dt);
        }
        free(decl_text);
        p = c + 1;
    }
}

static void free_keyframes(fm_doc *d) {
    for (int i = 0; i < d->nkf; i++)
        for (int s = 0; s < d->kf[i].n; s++) {
            for (int j = 0; j < d->kf[i].nd[s]; j++)
                free(d->kf[i].decls[s][j].value);
            free(d->kf[i].decls[s]);
        }
    free(d->kf);
    d->kf = NULL;
    d->nkf = 0;
}

/* @media (min-width: 900px) and (orientation: landscape): judged against the screen */
static bool media_holds(const char *q) {
    if (!strcasestr(q, "width") && !strcasestr(q, "height") && !strcasestr(q, "orientation") &&
        !strcasestr(q, "aspect"))
        return strcasestr(q, "screen") || strcasestr(q, "all");
    bool ok = true;
    for (const char *p = q; (p = strchr(p, '(')); p++) {
        char feat[32] = "", val[32] = "";
        sscanf(p + 1, " %31[^:) ] : %31[^)]", feat, val);
        float v = strtof(val, NULL);
        if (!strcasecmp(feat, "min-width")) ok &= fm_media_w >= v;
        else if (!strcasecmp(feat, "max-width")) ok &= fm_media_w <= v;
        else if (!strcasecmp(feat, "min-height")) ok &= fm_media_h >= v;
        else if (!strcasecmp(feat, "max-height")) ok &= fm_media_h <= v;
        else if (!strcasecmp(feat, "orientation"))
            ok &= strcasestr(val, "portrait") ? fm_media_h > fm_media_w : fm_media_w >= fm_media_h;
    }
    if (strcasestr(q, " or ") || strchr(q, ',')) { /* any of them */
        ok = false;
        char copy[256];
        snprintf(copy, sizeof copy, "%s", q);
        for (char *s, *part = strtok_r(copy, ",", &s); part; part = strtok_r(NULL, ",", &s))
            ok |= media_holds(part) && !strcasestr(part, " or ");
    }
    return ok;
}

/* ---- filters, clip paths ---- */

static float angle_of(const char *v) {
    char *e;
    float a = strtof(skip_ws(v), &e);
    if (!strncasecmp(e, "turn", 4)) return a * 360;
    if (!strncasecmp(e, "rad", 3)) return a * 57.29578f;
    if (!strncasecmp(e, "grad", 4)) return a * 0.9f;
    return a;
}

static bool apply(fm_style *st, const char *prop, const char *v);
static void parse_shadow_into(fm_style *st, const char *v) {
    char inner[160];
    const char *e = strchr(v, ')');
    snprintf(inner, sizeof inner, "%.*s", e ? (int)(e - v) : (int)strlen(v), v);
    fm_style tmp = *st;
    apply(&tmp, "box-shadow", inner);
    if (tmp.nshadow && st->nshadow < 4) {
        int i = st->nshadow++;
        st->sh_x[i] = tmp.sh_x[0], st->sh_y[i] = tmp.sh_y[0], st->sh_blur[i] = tmp.sh_blur[0];
        st->sh_spread[i] = 0, st->sh_c[i] = tmp.sh_c[0];
        if (i == 0)
            st->shadow_x = tmp.shadow_x, st->shadow_y = tmp.shadow_y, st->shadow_blur = tmp.shadow_blur,
            st->shadow = tmp.shadow;
    }
}

/* "blur(4px) brightness(1.2) grayscale(100%) ..." */
static bool parse_filter(fm_style *st, const char *v) {
    st->has_filter = false;
    st->f_blur = st->f_gray = st->f_sepia = st->f_hue = st->f_invert = 0;
    st->f_bright = st->f_contrast = st->f_sat = st->f_opacity = 1;
    st->bg_size = 'a', st->bg_px = st->bg_py = 0.5f, st->bg_repeat = 'r';
    st->row_gap = st->col_gap = -1;
    v = skip_ws(v);
    if (!strncasecmp(v, "none", 4))
        return true;
    bool any = false;
    for (const char *p = v; *p;) {
        p = skip_ws(p);
        const char *o = strchr(p, '(');
        if (!o)
            break;
        char name[24];
        snprintf(name, sizeof name, "%.*s", (int)(o - p) < 23 ? (int)(o - p) : 23, p);
        char *e;
        float x = strtof(o + 1, &e);
        if (*e == '%')
            x /= 100;
        if (!strcasecmp(name, "blur")) st->f_blur = x;
        else if (!strcasecmp(name, "brightness")) st->f_bright = x;
        else if (!strcasecmp(name, "contrast")) st->f_contrast = x;
        else if (!strcasecmp(name, "grayscale")) st->f_gray = x;
        else if (!strcasecmp(name, "sepia")) st->f_sepia = x;
        else if (!strcasecmp(name, "saturate")) st->f_sat = x;
        else if (!strcasecmp(name, "hue-rotate")) st->f_hue = angle_of(o + 1);
        else if (!strcasecmp(name, "invert")) st->f_invert = x;
        else if (!strcasecmp(name, "opacity")) st->f_opacity = x;
        else if (!strcasecmp(name, "drop-shadow")) { /* a shadow that follows the shape: drawn as its box's */
            parse_shadow_into(st, o + 1);
        } else
            return false;
        any = true;
        const char *c = strchr(o, ')');
        p = c ? c + 1 : o + strlen(o);
    }
    st->has_filter = any;
    return any;
}

/* circle(40% at 50% 50%), ellipse(50% 30% at 50% 50%), inset(10px 20px round 8px), polygon(50% 0, 100% 100%, 0 100%) */
static float clip_num(const char **p, bool *pct) {
    *p = skip_ws(*p);
    char *e;
    float x = strtof(*p, &e);
    *pct = *e == '%';
    *p = e + (*pct ? 1 : (!strncasecmp(e, "px", 2) ? 2 : 0));
    return *pct ? x / 100 : x;
}

static bool parse_clip(fm_style *st, const char *v) {
    st->clip = 0;
    st->clip_n = 0;
    if (!strncasecmp(v, "none", 4))
        return true;
    const char *o = strchr(v, '(');
    if (!o)
        return false;
    const char *p = o + 1;
    bool pct;
    if (!strncasecmp(v, "circle", 6) || !strncasecmp(v, "ellipse", 7)) {
        bool circle = v[0] == 'c' || v[0] == 'C';
        float r1 = 0.5f, r2 = 0.5f, cx = 0.5f, cy = 0.5f;
        const char *at = strcasestr(p, "at ");
        if (skip_ws(p) != at && *skip_ws(p) != ')') {
            r1 = clip_num(&p, &pct);
            r2 = circle ? r1 : clip_num(&p, &pct);
        }
        if (at) {
            p = at + 3;
            cx = clip_num(&p, &pct);
            cy = clip_num(&p, &pct);
        }
        st->clip = circle ? 'c' : 'e';
        st->clip_v[0] = r1, st->clip_v[1] = r2, st->clip_v[2] = cx, st->clip_v[3] = cy;
        st->clip_n = 4;
        return true;
    }
    if (!strncasecmp(v, "inset", 5)) {
        float t[4] = {0}, r = 0;
        int n = 0;
        while (*skip_ws(p) && *skip_ws(p) != ')' && strncasecmp(skip_ws(p), "round", 5) && n < 4) {
            t[n++] = clip_num(&p, &pct);
            if (!pct)
                t[n - 1] = -t[n - 1]; /* px: negative, told apart from fractions */
        }
        float tt[4];
        tt[0] = t[0], tt[1] = n > 1 ? t[1] : t[0], tt[2] = n > 2 ? t[2] : t[0], tt[3] = n > 3 ? t[3] : tt[1];
        const char *rd = strcasestr(p, "round");
        if (rd) {
            const char *q = rd + 5;
            r = clip_num(&q, &pct);
        }
        st->clip = 'i';
        memcpy(st->clip_v, tt, sizeof tt);
        st->clip_v[4] = r;
        st->clip_n = 5;
        return true;
    }
    if (!strncasecmp(v, "polygon", 7)) {
        st->clip = 'p';
        while (*skip_ws(p) && *skip_ws(p) != ')' && st->clip_n < 34) {
            float x = clip_num(&p, &pct);
            if (!pct)
                x = -x;
            float y = clip_num(&p, &pct);
            if (!pct)
                y = -y;
            st->clip_v[st->clip_n++] = x;
            st->clip_v[st->clip_n++] = y;
            p = skip_ws(p);
            if (*p == ',')
                p++;
        }
        return st->clip_n >= 6;
    }
    return false;
}

/* "span 2", "1 / 3", "2 / span 3", "1 / -1" */
static void grid_lines(const char *v, short *a, short *b, short *span) {
    *a = *b = *span = 0;
    const char *slash = strchr(v, '/');
    char first[32] = "", second[32] = "";
    snprintf(first, sizeof first, "%.*s", slash ? (int)(slash - v) : (int)strlen(v), v);
    if (slash)
        snprintf(second, sizeof second, "%s", skip_ws(slash + 1));
    const char *f = skip_ws(first);
    if (!strncasecmp(f, "span", 4))
        *span = (short)atoi(f + 4);
    else
        *a = (short)atoi(f);
    if (second[0]) {
        if (!strncasecmp(second, "span", 4))
            *span = (short)atoi(second + 4);
        else
            *b = (short)atoi(second);
    }
}

/* "anchor(bottom)", "anchor(--menu left)", "calc(anchor(top) - 8px)" */
static void anchor_inset(fm_style *st, int k, const char *v) {
    const char *a = strcasestr(v, "anchor(") + 7;
    char w1[32] = "", w2[32] = "";
    sscanf(a, " %31[^ ,)] %31[^ ,)]", w1, w2);
    const char *side = w1;
    st->anc_of[k][0] = 0;
    if (w1[0] == '-' && w1[1] == '-') {
        snprintf(st->anc_of[k], sizeof st->anc_of[k], "%s", w1);
        side = w2;
    }
    st->anc_side[k] = !strcasecmp(side, "top") || !strcasecmp(side, "start") ? 't'
                    : !strcasecmp(side, "bottom") || !strcasecmp(side, "end") ? 'b'
                    : !strcasecmp(side, "left") ? 'l' : !strcasecmp(side, "right") ? 'r' : 'c';
    /* top/bottom insets read the anchor's rows; left/right its columns: "start"/"end" follow that */
    if ((k == 1 || k == 3) && (st->anc_side[k] == 't' || st->anc_side[k] == 'b'))
        st->anc_side[k] = st->anc_side[k] == 't' ? 'l' : 'r';
    st->anc_off[k] = 0;
    const char *close = strchr(a, ')');
    if (close) {
        const char *q = close + 1;
        while (*q == ' ')
            q++;
        if (*q == '+' || *q == '-')
            st->anc_off[k] = (*q == '-' ? -1 : 1) * strtof(q + 1, NULL);
    }
}

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
    } else if ((IS("left") || IS("top") || IS("right") || IS("bottom")) && strcasestr(v, "anchor(")) {
        int k = IS("top") ? 0 : IS("right") ? 1 : IS("bottom") ? 2 : 3;
        anchor_inset(st, k, v);
        st->absolute = true;
    } else if (IS("anchor-name")) {
        sscanf(skip_ws(v), "%31[^ ;,]", st->anchor_name);
    } else if (IS("position-anchor")) {
        sscanf(skip_ws(v), "%31[^ ;,]", st->pos_anchor);
        st->absolute = true;
    } else if (IS("position-area") || IS("inset-area")) {
        v = skip_ws(v);
        st->area_v = st->area_h = 0;
        char w1[24] = "", w2[24] = "";
        sscanf(v, "%23s %23s", w1, w2);
        for (int i = 0; i < 2; i++) {
            const char *w = i ? w2 : w1;
            if (!w[0]) continue;
            bool span = !strncasecmp(w, "span-", 5);
            const char *b = span ? w + 5 : w;
            if (!strcasecmp(b, "top") || !strcasecmp(b, "block-start")) st->area_v = span ? 'T' : 't';
            else if (!strcasecmp(b, "bottom") || !strcasecmp(b, "block-end")) st->area_v = span ? 'B' : 'b';
            else if (!strcasecmp(b, "left") || !strcasecmp(b, "inline-start")) st->area_h = span ? 'L' : 'l';
            else if (!strcasecmp(b, "right") || !strcasecmp(b, "inline-end")) st->area_h = span ? 'R' : 'r';
            else if (!strcasecmp(b, "center")) {
                if (!st->area_v) st->area_v = 'c';
                else st->area_h = 'c';
            } else if (!strcasecmp(b, "all")) st->area_v = 'c', st->area_h = 'c';
        }
        if (st->area_v && !st->area_h) st->area_h = 'c';
        if (st->area_h && !st->area_v) st->area_v = 'c';
        st->absolute = true;
    } else if (IS("position-try-fallbacks") || IS("position-try") || IS("position-try-options")) {
        st->flip_block = strcasestr(v, "flip-block") != NULL || strcasestr(v, "flip-y") != NULL;
        st->flip_inline = strcasestr(v, "flip-inline") != NULL || strcasestr(v, "flip-x") != NULL;
    } else if ((IS("width") || IS("height")) && strcasestr(v, "anchor-size(")) {
        const char *a = strcasestr(v, "anchor-size(") + 12;
        char which = strcasestr(a, "height") || strcasestr(a, "block") ? 'h' : 'w';
        if (IS("width")) st->anc_w = which;
        else st->anc_h = which;
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
    else if (IS("gap") || IS("grid-gap")) {
        float a = 0, b = -1;
        int k = sscanf(skip_ws(v), "%f%*[^0-9.]%f", &a, &b);
        st->gap = a;
        st->row_gap = a, st->col_gap = k > 1 ? b : a;
    } else if (IS("row-gap") || IS("grid-row-gap")) {
        st->row_gap = strtof(skip_ws(v), NULL);
    } else if (IS("column-gap") || IS("grid-column-gap")) {
        st->col_gap = strtof(skip_ws(v), NULL);
        if (st->dir == FM_ROW && st->mode != FM_GRID)
            st->gap = st->col_gap;
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
        const char *url = strcasestr(v, "url(");
        if (url) { /* a picture, maybe under or over a gradient or colour: "linear-gradient(...), url(x) center/cover" */
            const char *q = url + 4;
            while (*q == ' ' || *q == '"' || *q == '\'')
                q++;
            int k = 0;
            while (*q && *q != ')' && *q != '"' && *q != '\'' && k < 159)
                st->bg_img[k++] = *q++;
            st->bg_img[k] = 0;
            if (IS("background")) {
                st->bg_size = 'a', st->bg_px = st->bg_py = 0.5f, st->bg_repeat = 'r';
                const char *after = strchr(url, ')');
                after = after ? after + 1 : url + 4;
                if (strcasestr(after, "cover")) st->bg_size = 'c';
                else if (strcasestr(after, "contain")) st->bg_size = 'C';
                if (strcasestr(after, "no-repeat")) st->bg_repeat = 'n';
                else if (strcasestr(after, "repeat-x")) st->bg_repeat = 'x';
                else if (strcasestr(after, "repeat-y")) st->bg_repeat = 'y';
                if (strcasestr(after, "left")) st->bg_px = 0;
                if (strcasestr(after, "right")) st->bg_px = 1;
                if (strcasestr(after, "top")) st->bg_py = 0;
                if (strcasestr(after, "bottom")) st->bg_py = 1;
            }
            /* the rest of the list: a gradient or colour, before the picture (over it) or after (under it) */
            char rest[400] = "";
            size_t pre = (size_t)(url - v);
            const char *close = strchr(url, ')');
            const char *post = close ? close + 1 : url + 4;
            const char *comma = strchr(post, ',');
            if (pre > 0) {
                snprintf(rest, sizeof rest, "%.*s", (int)pre, v);
                char *e = rest + strlen(rest);
                while (e > rest && (e[-1] == ' ' || e[-1] == ','))
                    *--e = 0;
                st->bg_img_over = true;
            } else if (comma) {
                snprintf(rest, sizeof rest, "%s", comma + 1);
                st->bg_img_over = false;
            }
            if (!rest[0] || !parse_bg(rest, &st->bg)) {
                /* "url(x) #222": a colour word after it */
                const char *w = post;
                color cc;
                while (*w) {
                    const char *q2 = skip_ws(w);
                    if (parse_color(&q2, &cc)) {
                        st->bg.kind = FM_BG_SOLID, st->bg.a = st->bg.b = cc, st->bg.nstops = 0;
                        break;
                    }
                    while (*w && *w != ' ')
                        w++;
                    w = skip_ws(w);
                }
            }
        } else {
            if (IS("background-image") || IS("background"))
                st->bg_img[0] = 0;
            ok = parse_bg(v, &st->bg);
        }
    } else if (IS("background-size")) {
        v = skip_ws(v);
        if (!strncasecmp(v, "cover", 5)) st->bg_size = 'c';
        else if (!strncasecmp(v, "contain", 7)) st->bg_size = 'C';
        else if (!strncasecmp(v, "auto", 4)) st->bg_size = 'a';
        else {
            char *e;
            st->bg_sw = strtof(v, &e);
            st->bg_size = *e == '%' ? 'f' : 'p';
            if (*e == '%') st->bg_sw /= 100, e++;
            else if (!strncasecmp(e, "px", 2)) e += 2;
            const char *h = skip_ws(e);
            st->bg_sh = *h && *h != 'a' ? strtof(h, NULL) / (st->bg_size == 'f' ? 100 : 1) : -1;
        }
    } else if (IS("background-position")) {
        v = skip_ws(v);
        st->bg_px = st->bg_py = 0.5f;
        if (strcasestr(v, "left")) st->bg_px = 0;
        if (strcasestr(v, "right")) st->bg_px = 1;
        if (strcasestr(v, "top")) st->bg_py = 0;
        if (strcasestr(v, "bottom")) st->bg_py = 1;
        if (isdigit((unsigned char)*v)) {
            char *e;
            st->bg_px = strtof(v, &e) / 100;
            if (*e == '%') e++;
            st->bg_py = *skip_ws(e) ? strtof(skip_ws(e), NULL) / 100 : 0.5f;
        }
    } else if (IS("background-repeat")) {
        v = skip_ws(v);
        st->bg_repeat = !strncasecmp(v, "no-repeat", 9) ? 'n' : !strncasecmp(v, "repeat-x", 8) ? 'x'
                      : !strncasecmp(v, "repeat-y", 8) ? 'y' : 'r';
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
        st->nshadow = 0;
        if (!strncasecmp(p, "none", 4)) {
            st->shadow_blur = 0;
            st->shadow = 0;
            st->ninset = 0;
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
                } else if (!inset && st->nshadow < 4) {
                    int i = st->nshadow++;
                    st->sh_x[i] = k > 0 ? f[0] : 0;
                    st->sh_y[i] = k > 1 ? f[1] : 0;
                    st->sh_blur[i] = k > 2 ? f[2] : (k == 2 ? 0 : 12);
                    st->sh_spread[i] = k > 3 ? f[3] : 0;
                    st->sh_c[i] = c;
                    if (!outer) {
                        outer = true;
                        st->shadow_x = st->sh_x[i], st->shadow_y = st->sh_y[i];
                        st->shadow_blur = st->sh_blur[i] > 0 ? st->sh_blur[i] : 0.01f; /* sharp, yet there */
                        st->shadow = c;
                    }
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
    } else if (IS("font-smooth") || IS("-webkit-font-smoothing") || IS("font-smoothing")) {
        v = skip_ws(v);
        st->crisp_text = !strncasecmp(v, "none", 4) || !strncasecmp(v, "never", 5) || !strncasecmp(v, "crisp", 5) ||
                         !strncasecmp(v, "pixel", 5);
    } else if (IS("image-rendering")) {
        v = skip_ws(v);
        st->pixelated = !strncasecmp(v, "pixel", 5) || !strncasecmp(v, "crisp", 5) || !strncasecmp(v, "optimizespeed", 13);
    } else if (IS("animation")) {
        anim_shorthand(st, v);
    } else if (IS("animation-name")) {
        sscanf(skip_ws(v), "%39[^ ;,]", st->anim_name);
        if (!strcasecmp(st->anim_name, "none"))
            st->anim_name[0] = 0;
    } else if (IS("animation-duration")) {
        st->anim_dur = time_ms(skip_ws(v));
    } else if (IS("animation-delay")) {
        st->anim_delay = time_ms(skip_ws(v));
    } else if (IS("animation-iteration-count")) {
        v = skip_ws(v);
        st->anim_count = !strncasecmp(v, "infinite", 8) ? -1 : strtof(v, NULL);
    } else if (IS("animation-direction")) {
        anim_word(st, skip_ws(v));
    } else if (IS("animation-timing-function") || IS("animation-fill-mode")) {
        anim_word(st, skip_ws(v));
    } else if (IS("cursor")) {
        snprintf(st->cursor, sizeof st->cursor, "%s", skip_ws(v));
    } else if (IS("scrollbar-color")) {
        const char *p = skip_ws(v);
        if (!strncasecmp(p, "auto", 4))
            st->sb_thumb = st->sb_track = 0;
        else {
            ok = parse_color(&p, &st->sb_thumb);
            p = skip_ws(p);
            if (*p)
                parse_color(&p, &st->sb_track);
        }
    } else if (IS("scrollbar-width")) {
        v = skip_ws(v);
        st->sb_width = !strncasecmp(v, "none", 4) ? -1 : !strncasecmp(v, "thin", 4) ? 6 : !strncasecmp(v, "auto", 4) ? 0
                                                                                     : strtof(v, NULL);
    } else if (IS("text-decoration") || IS("text-decoration-line")) {
        v = skip_ws(v);
        st->decoration = strcasestr(v, "underline") ? 'u' : strcasestr(v, "line-through") ? 'l'
                       : strcasestr(v, "overline") ? 'o' : 0;
        ok = st->decoration || !strncasecmp(v, "none", 4);
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
    } else if (IS("text-rotate")) { /* "text-rotate: -90deg": the text in it turns, and its layout with it */
        float a = strtof(skip_ws(v), NULL);
        int q = ((int)lroundf(a / 90.0f) % 4 + 4) % 4;
        ok = fabsf(a - 90.0f * lroundf(a / 90.0f)) < 1;
        st->text_rot = (short)(q == 1 ? 90 : q == 2 ? 180 : q == 3 ? -90 : 0);
    } else if (IS("filter")) {
        ok = parse_filter(st, v);
    } else if (IS("mix-blend-mode")) {
        static const char *const names[] = {"normal", "multiply", "screen", "overlay", "darken", "lighten",
                                            "difference", "exclusion", "color-dodge", "color-burn", "hard-light",
                                            "soft-light", NULL};
        static const char codes[] = {0, 'm', 's', 'o', 'd', 'l', 'D', 'x', 'c', 'b', 'h', 'S'};
        ok = false;
        for (int i = 0; names[i]; i++)
            if (!strncasecmp(skip_ws(v), names[i], strlen(names[i])))
                st->blend = codes[i], ok = true;
    } else if (IS("clip-path")) {
        ok = parse_clip(st, skip_ws(v));
    } else if (IS("mask") || IS("mask-image") || IS("-webkit-mask-image") || IS("-webkit-mask")) {
        v = skip_ws(v);
        if (!strncasecmp(v, "none", 4))
            st->mask.kind = FM_BG_NONE;
        else
            ok = parse_bg(v, &st->mask) && st->mask.kind != FM_BG_SOLID;
    } else if (IS("transform")) {
        const char *rot = strstr(v, "rotate(");
        st->rotate = rot ? angle_of(rot + 7) : 0;
        const char *sk = strstr(v, "skew(");
        const char *skx = strstr(v, "skewX("), *sky = strstr(v, "skewY(");
        st->skew_x = st->skew_y = 0;
        if (sk) {
            const char *q = sk + 5;
            st->skew_x = angle_of(q);
            const char *comma = strchr(q, ',');
            if (comma && comma < strchr(q, ')'))
                st->skew_y = angle_of(comma + 1);
        }
        if (skx)
            st->skew_x = angle_of(skx + 6);
        if (sky)
            st->skew_y = angle_of(sky + 6);
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
    } else if (IS("columns") || IS("grid-columns")) {
        st->columns = atoi(skip_ws(v));
        st->grid_cols[0] = 0;
    } else if (IS("grid-template-columns")) {
        snprintf(st->grid_cols, sizeof st->grid_cols, "%s", skip_ws(v));
        st->mode = FM_GRID;
    } else if (IS("grid-template-rows")) {
        snprintf(st->grid_rows, sizeof st->grid_rows, "%s", skip_ws(v));
        st->mode = FM_GRID;
    } else if (IS("grid-template-areas")) {
        snprintf(st->grid_areas, sizeof st->grid_areas, "%s", skip_ws(v));
        st->mode = FM_GRID;
    } else if (IS("grid-column") || IS("grid-row")) {
        bool col = IS("grid-column");
        short a = 0, b = 0, span = 0;
        grid_lines(skip_ws(v), &a, &b, &span);
        if (col) st->gc0 = a, st->gc1 = b, st->gcs = span;
        else st->gr0 = a, st->gr1 = b, st->grs = span;
    } else if (IS("grid-column-start")) {
        st->gc0 = (short)atoi(skip_ws(v));
    } else if (IS("grid-row-start")) {
        st->gr0 = (short)atoi(skip_ws(v));
    } else if (IS("grid-area")) {
        v = skip_ws(v);
        if (isdigit((unsigned char)*v)) { /* row-start / col-start / row-end / col-end */
            int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
            sscanf(v, "%d / %d / %d / %d", &r0, &c0, &r1, &c1);
            st->gr0 = (short)r0, st->gc0 = (short)c0, st->gr1 = (short)r1, st->gc1 = (short)c1;
        } else {
            sscanf(v, "%19[^ ;]", st->grid_area);
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
        {"outline", "border or box-shadow"},
        {"pointer-events", ""},
        {"user-select", ""},                              {"box-sizing", "(sizes here already include padding)"},
        {"will-change", ""},
        {"align-content", "align-items"},                 {"flex-shrink", "min-width / max-width"},
        {"flex-basis", "width"},                          {"object-fit", ""},
        {"vertical-align", "align-items on the row"},     {"list-style", ""},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcasecmp(prop, t[i][0]))
            return t[i][1];
    if (!strcasecmp(prop, "transform") && strstr(v, "matrix"))
        return "scale(), translate(), rotate() and skew()";
    return NULL;
}

/* ---- selectors ---- */

static uint8_t state_of(const char *name) {
    if (!strcmp(name, "hover")) return ST_HOVER;
    if (!strcmp(name, "active") || !strcmp(name, "pressed")) return ST_ACTIVE;
    if (!strcmp(name, "open") || !strcmp(name, "checked")) return ST_OPEN;
    if (!strcmp(name, "focus") || !strcmp(name, "focus-visible") || !strcmp(name, "focus-within")) return ST_FOCUS;
    return 0;
}

/* an+b, odd, even, 3 */
static bool parse_nth(const char *v, int *a, int *b) {
    while (*v == ' ')
        v++;
    if (!strncasecmp(v, "odd", 3)) return *a = 2, *b = 1, true;
    if (!strncasecmp(v, "even", 4)) return *a = 2, *b = 0, true;
    char *e;
    long x = strtol(v, &e, 10);
    const char *n = strchr(v, 'n');
    if (!n) {
        if (e == v)
            return false;
        *a = 0, *b = (int)x;
        return true;
    }
    if (n == v || (n == v + 1 && (*v == '+' || *v == '-')))
        *a = *v == '-' ? -1 : 1;
    else
        *a = (int)x;
    const char *r = n + 1;
    while (*r == ' ')
        r++;
    int sign = 1;
    if (*r == '+' || *r == '-') {
        sign = *r == '-' ? -1 : 1;
        r++;
    }
    *b = *r ? sign * atoi(r) : 0;
    return true;
}

/* :not(x) for a simple x */
static bool parse_not(const char *v, struct compound *c) {
    while (*v == ' ')
        v++;
    c->has_not = true;
    int k = 0;
    if (*v == '#') {
        for (v++; (isalnum((unsigned char)*v) || *v == '-' || *v == '_') && k < 47;)
            c->not_id[k++] = *v++;
    } else if (*v == '.') {
        for (v++; (isalnum((unsigned char)*v) || *v == '-' || *v == '_') && k < 31;)
            c->not_cls[k++] = *v++;
    } else if (*v == '[') {
        for (v++; *v && *v != ']' && *v != '=' && k < 23;)
            c->not_attr[k++] = *v++;
    } else if (*v == ':') {
        char name[24];
        for (v++; (isalnum((unsigned char)*v) || *v == '-') && k < 23;)
            name[k++] = (char)tolower((unsigned char)*v++);
        name[k] = 0;
        c->not_states = state_of(name);
        return c->not_states != 0;
    } else if (isalpha((unsigned char)*v)) {
        for (; (isalnum((unsigned char)*v) || *v == '-') && k < 23;)
            c->not_tag[k++] = (char)tolower((unsigned char)*v++);
    }
    return k > 0;
}

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
            while (*s && *s != '=' && *s != ']' && !strchr("^$*~|", *s) && k < 23)
                c->attr[k++] = *s++;
            if (strchr("^$*~|", *s) && *s && s[1] == '=')
                c->attr_op = *s == '|' ? '^' : *s, s++;
            if (*s == '=') {
                if (!c->attr_op)
                    c->attr_op = '=';
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
            char name[24];
            int k = 0;
            while ((isalnum((unsigned char)*s) || *s == '-') && k < 23)
                name[k++] = (char)tolower((unsigned char)*s++);
            name[k] = 0;
            char arg[96] = "";
            if (*s == '(') { /* its argument, nested brackets and all */
                int depth = 0, a = 0;
                for (s++; *s && !(*s == ')' && depth == 0); s++) {
                    if (*s == '(') depth++;
                    if (*s == ')') depth--;
                    if (a < 95)
                        arg[a++] = *s;
                }
                arg[a] = 0;
                if (*s == ')')
                    s++;
            }
            uint8_t st = state_of(name);
            if (st)
                c->states |= st;
            else if (!strcmp(name, "root"))
                strcpy(c->tag, "facet");
            else if (!strcmp(name, "first-child") || !strcmp(name, "first-of-type"))
                c->nth = true, c->nth_a = 0, c->nth_b = 1;
            else if (!strcmp(name, "last-child") || !strcmp(name, "last-of-type"))
                c->nth = c->nth_last = true, c->nth_a = 0, c->nth_b = 1;
            else if (!strcmp(name, "only-child"))
                c->only = true;
            else if (!strcmp(name, "empty"))
                c->empty = true;
            else if (!strcmp(name, "nth-child") || !strcmp(name, "nth-last-child") || !strcmp(name, "nth-of-type")) {
                if (!parse_nth(arg, &c->nth_a, &c->nth_b))
                    return false;
                c->nth = true;
                c->nth_last = !strcmp(name, "nth-last-child");
            } else if (!strcmp(name, "not")) {
                if (!parse_not(arg, c))
                    return false;
            } else if (!strcmp(name, "before"))
                c->pseudo = 'b';
            else if (!strcmp(name, "after"))
                c->pseudo = 'a';
            else
                return false; /* a pseudo-class this doesn't know: refused, never quietly matched */
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
            comb = *s; /* '>' child, '+' the very next sibling, '~' any later sibling */
            s = skip_ws(s + 1);
        }
    }
    if (*s || sel->n == 0)
        return false;
    for (int i = 0; i < sel->n; i++) {
        struct compound *c = &sel->parts[i];
        sel->spec += (c->id[0] ? 100 : 0) +
                     10 * (c->ncls + (c->attr[0] ? 1 : 0) + __builtin_popcount(c->states) + c->nth + c->only + c->empty) +
                     (c->tag[0] ? 1 : 0) + (c->pseudo ? 1 : 0) +
                     (c->has_not ? (c->not_id[0] ? 100 : c->not_tag[0] ? 1 : 10) : 0);
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

/* Its place among its parent's elements: 1-based, from the start or from the end */
static int place_of(const fm_node *n, bool from_end, int *count) {
    int i = 0, at = 0;
    for (const fm_node *k = n->parent ? n->parent->first : n; k; k = k->next) {
        if (k->kind != FM_ELEM || k->pseudo || (n->parent == NULL && k != n))
            continue;
        i++;
        if (k == n)
            at = i;
    }
    *count = i;
    return from_end ? i - at + 1 : at;
}

static bool nth_fits(int pos, int a, int b) {
    if (a == 0)
        return pos == b;
    int d = pos - b;
    return d % a == 0 && d / a >= 0;
}

static bool attr_fits(const char *v, char op, const char *want) {
    if (!v)
        return false;
    size_t lv = strlen(v), lw = strlen(want);
    switch (op) {
    case 0: return true;
    case '=': return !strcmp(v, want);
    case '^': return !strncmp(v, want, lw);
    case '$': return lv >= lw && !strcmp(v + lv - lw, want);
    case '*': return strstr(v, want) != NULL;
    case '~':
        for (const char *p = v; (p = strstr(p, want)); p++)
            if ((p == v || p[-1] == ' ') && (p[lw] == 0 || p[lw] == ' '))
                return true;
        return false;
    }
    return false;
}

static bool match_compound_el(const fm_node *n, const struct compound *c);

static bool match_compound(const fm_node *n, const struct compound *c) {
    if (n->kind != FM_ELEM)
        return false;
    /* ::before and ::after are their own boxes; the rest of the compound is about the element they belong to */
    if (c->pseudo || n->pseudo)
        return c->pseudo == n->pseudo && n->parent && match_compound_el(n->parent, c);
    return match_compound_el(n, c);
}

static bool match_compound_el(const fm_node *n, const struct compound *c) {
    if (c->tag[0] && !same_tag(c->tag, n->tag))
        return false;
    if (c->id[0] && strcmp(c->id, n->id))
        return false;
    for (int i = 0; i < c->ncls; i++)
        if (!has_class(n, c->cls[i]))
            return false;
    if (c->attr[0] && !attr_fits(fm_attr_get(n, c->attr), c->attr_op, c->attr_val))
        return false;
    if (c->nth || c->only) {
        int count, pos = place_of(n, c->nth_last, &count);
        if (c->only && count != 1)
            return false;
        if (c->nth && !nth_fits(pos, c->nth_a, c->nth_b))
            return false;
    }
    if (c->empty)
        for (const fm_node *k = n->first; k; k = k->next)
            if (k->kind == FM_ELEM || (k->text && k->text[strspn(k->text, " \t\n\r")]))
                return false;
    if (c->has_not) {
        bool hit = (c->not_tag[0] && same_tag(c->not_tag, n->tag)) || (c->not_id[0] && !strcmp(c->not_id, n->id)) ||
                   (c->not_cls[0] && has_class(n, c->not_cls)) || (c->not_attr[0] && fm_attr_get(n, c->not_attr)) ||
                   ((c->not_states & ST_HOVER) && n->hover) || ((c->not_states & ST_ACTIVE) && n->active) ||
                   ((c->not_states & ST_OPEN) && n->open) || ((c->not_states & ST_FOCUS) && n->focus);
        if (hit)
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

static bool match_up(const fm_node *n, const struct selector *s, int i);

static bool match_from(const fm_node *n, const struct selector *s, int i) {
    if (!match_compound(n, &s->parts[i]))
        return false;
    return match_up(n, s, i);
}

/* n matched part i: does what is around it match the parts before? */
static bool match_up(const fm_node *n, const struct selector *s, int i) {
    if (i == 0)
        return true;
    if (s->parts[i].comb == '>')
        return n->parent && match_from(n->parent, s, i - 1);
    if (s->parts[i].comb == '+' || s->parts[i].comb == '~') { /* siblings before it */
        const fm_node *prev = NULL;
        for (const fm_node *k = n->parent ? n->parent->first : NULL; k && k != n; k = k->next)
            if (k->kind == FM_ELEM && !k->pseudo) {
                if (s->parts[i].comb == '~' && match_from(k, s, i - 1))
                    return true;
                prev = k;
            }
        return s->parts[i].comb == '+' && prev && match_from(prev, s, i - 1);
    }
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
                const char *at = skip_ws(selectors);
                if (!strncasecmp(at, "@keyframes", 10) && pass == 1 && !vars_only) {
                    read_keyframes(d, at, open + 1, end);
                } else if (scheme && scheme == fm_scheme && pass == 0 && nlater < 8) {
                    /* it wins wherever it sits: read after the rest */
                    later[nlater][0] = open + 1;
                    later[nlater++][1] = end;
                } else if (!scheme && !strncasecmp(at, "@media", 6) && media_holds(at + 6) && pass == 0 && nlater < 8) {
                    later[nlater][0] = open + 1; /* a screen-size block that holds: read after the rest */
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
    free_keyframes(d);
    if (d->media_w)
        fm_media_w = d->media_w, fm_media_h = d->media_h;
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
    d->has_pseudo = false;
    for (int i = 0; i < d->nrules; i++)
        d->has_pseudo |= d->rules[i].sel.parts[d->rules[i].sel.n - 1].pseudo != 0;
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
    st->anim_count = 1, st->anim_dir = 'n', st->anim_ease = 'e', st->anim_fill = 'n';
    st->f_bright = st->f_contrast = st->f_sat = st->f_opacity = 1;
    st->align = FM_A_STRETCH;
    st->border = RGBA(255, 255, 255, 60);
    st->glow = RGBA(255, 255, 255, 120);
    if (parent) { /* inherited properties */
        st->color = parent->color;
        memcpy(st->font, parent->font, sizeof st->font);
        st->font_size = parent->font_size;
        st->text_align = parent->text_align;
        st->text_rot = parent->text_rot;
        st->crisp_text = parent->crisp_text;
        memcpy(st->cursor, parent->cursor, sizeof st->cursor);
        st->sb_thumb = parent->sb_thumb, st->sb_track = parent->sb_track, st->sb_width = parent->sb_width;
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

/* ---- ::before and ::after ---- */

/* What content: says for n's ::before ('b') or ::after ('a'): its text (malloc'd), or NULL for none */
static char *pseudo_content(fm_doc *d, fm_node *n, char which) {
    const char *found = NULL;
    for (int i = 0; i < d->nrules; i++) {
        struct fm_rule *r = &d->rules[i];
        const struct compound *last = &r->sel.parts[r->sel.n - 1];
        if (last->pseudo != which || !match_compound_el(n, last) || !match_up(n, &r->sel, r->sel.n - 1))
            continue;
        for (int k = 0; k < r->ndecls; k++)
            if (!strcmp(r->decls[k].prop, "content"))
                found = r->decls[k].value;
    }
    if (!found)
        return NULL;
    const char *v = skip_ws(found);
    if (!strncasecmp(v, "none", 4) || !strncasecmp(v, "normal", 6))
        return NULL;
    sbuf b;
    sb_init(&b);
    while (*v) { /* "text", 'text', attr(name), in any order */
        v = skip_ws(v);
        if (*v == '"' || *v == '\'') {
            char q = *v++;
            while (*v && *v != q) {
                if (*v == '\\' && v[1]) {
                    v++;
                    if (*v == 'A' || *v == 'a') { /* \A: a new line */
                        sb_add(&b, "\n");
                        v++;
                        continue;
                    }
                }
                sb_addn(&b, v++, 1);
            }
            if (*v)
                v++;
        } else if (!strncasecmp(v, "attr(", 5)) {
            char name[32];
            int k = 0;
            for (v += 5; *v && *v != ')' && k < 31; v++)
                if (*v != ' ')
                    name[k++] = *v;
            name[k] = 0;
            const char *a = fm_attr_get(n, name);
            if (a)
                sb_add(&b, a);
            if (*v)
                v++;
        } else {
            while (*v && !isspace((unsigned char)*v))
                v++;
        }
    }
    return b.p ? b.p : strdup("");
}

static void ensure_pseudo(fm_doc *d, fm_node *n, char which) {
    fm_node *k = NULL;
    for (fm_node *c = n->first; c; c = c->next)
        if (c->kind == FM_ELEM && c->pseudo == which)
            k = c;
    char *text = pseudo_content(d, n, which);
    if (!text) {
        if (k)
            fm_attr_set(k, "hidden", "true");
        return;
    }
    if (!k) {
        k = fm_node_new(d, which == 'b' ? "before" : "after");
        k->generated = true;
        k->pseudo = which;
        k->app = k->win = -1;
        k->parent = n;
        if (which == 'b') {
            k->next = n->first;
            n->first = k;
            if (!n->last)
                n->last = k;
        } else {
            fm_append(n, k);
        }
    }
    fm_attr_set(k, "hidden", "false");
    fm_node *t = k->first;
    if (!t) {
        t = calloc(1, sizeof *t);
        t->kind = FM_TEXT;
        t->generated = true;
        t->app = t->win = -1;
        fm_append(k, t);
    }
    if (!t->text || strcmp(t->text, text)) {
        free(t->text);
        t->text = text;
    } else {
        free(text);
    }
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
        if (d->nkf)
            animate(d, n);
        if (n->st.opacity < 0) n->st.opacity = 0;
        if (n->st.opacity > 1) n->st.opacity = 1;
        if (n->st.scale <= 0.05f) n->st.scale = 1;
        if (n->st.font_size < 6) n->st.font_size = 6;
        if (n->st.font_size > 220) n->st.font_size = 220;
        if (d->has_pseudo && !n->pseudo && !is_meta(n)) {
            ensure_pseudo(d, n, 'b');
            ensure_pseudo(d, n, 'a');
        }
        style_node(d, n->first, &n->st);
    }
}

void fm_style_tree(fm_doc *d) {
    /* @media: the size it is shown at; a new one reads the sheet again (if it has size rules) */
    int w = d->screen.w > 0 ? d->screen.w : 1600, h = d->screen.h > 0 ? d->screen.h : 900;
    if (w != d->media_w || h != d->media_h) {
        bool had = d->media_w != 0;
        d->media_w = w, d->media_h = h;
        if (had && d->css && strcasestr(d->css, "@media"))
            fm_css_load(d);
    }

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
