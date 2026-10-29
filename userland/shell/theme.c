#define _GNU_SOURCE
#include "theme.h"
#include "app.h"
#include "fm/fm.h"
#include "wallpaper.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Light and dark for the whole space, and the colours the built-in apps
 * take from the design. See theme.h for the idea. */

#define THEME_FILE STATE_DIR "/theme"
#define FADE_MS 420.0f

enum { PREF_AUTO, PREF_LIGHT, PREF_DARK };

static struct {
    bool loaded;
    int pref;           /* what they asked for */
    bool dark;          /* what is showing */
    bool natural_dark;  /* what the design was drawn in */
    bool derived;       /* showing a variant the shell made itself */
    bool pending;       /* a switch waits for the end of this frame */
    fm_doc *doc;        /* the document the look was worked out for */
    const char *css;    /* ... and its stylesheet (a restyle replaces it) */
    size_t css_len;
    canvas *before;     /* the last frame of the old look, fading out */
    uint64_t fade_at;
    bool snap;          /* theme_crossfade: keep this frame and fade it out over the next */
} TH;

/* ---- colour ---- */

static float lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float gam(float c) {
    c = c < 0 ? 0 : c > 1 ? 1 : c;
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1 / 2.4f) - 0.055f;
}

/* OKLab: lightness that matches what the eye sees, so turning it around
 * keeps colours feeling like themselves. */
static void to_oklab(color c, float *L, float *a, float *b) {
    float r = lin(C_R(c) / 255.0f), g = lin(C_G(c) / 255.0f), bl = lin(C_B(c) / 255.0f);
    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * bl);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * bl);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * bl);
    *L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    *a = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    *b = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static bool from_oklab(float L, float a, float b, uint32_t alpha, color *out) {
    float l = L + 0.3963377774f * a + 0.2158037573f * b;
    float m = L - 0.1055613458f * a - 0.0638541728f * b;
    float s = L - 0.0894841775f * a - 1.2914855480f * b;
    l = l * l * l, m = m * m * m, s = s * s * s;
    float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float bl = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    bool in = r > -0.002f && r < 1.002f && g > -0.002f && g < 1.002f && bl > -0.002f && bl < 1.002f;
    *out = RGBA((int)lroundf(gam(r) * 255), (int)lroundf(gam(g) * 255), (int)lroundf(gam(bl) * 255), alpha);
    return in;
}

/* Piecewise-linear through (x, y) points, x increasing. */
static float curve(float x, const float (*p)[2], int n) {
    if (x <= p[0][0])
        return p[0][1];
    for (int i = 1; i < n; i++)
        if (x <= p[i][0]) {
            float t = (x - p[i - 1][0]) / (p[i][0] - p[i - 1][0]);
            return p[i - 1][1] + t * (p[i][1] - p[i - 1][1]);
        }
    return p[n - 1][1];
}

/* The other scheme, made by the shell: lightness turned around along a
 * curve chosen so surfaces land deep (or bright) but never pure black or
 * white, and text lands where it reads. Strong colours (a brand blue, an
 * accent) move less, so the design still looks like itself. */
static color derive(color c, bool to_dark) {
    static const float dark_curve[][2] = {{0, 0.95f}, {0.4f, 0.74f}, {0.6f, 0.44f}, {0.85f, 0.25f}, {1, 0.18f}};
    static const float light_curve[][2] = {{0, 0.985f}, {0.3f, 0.935f}, {0.5f, 0.64f}, {0.7f, 0.36f}, {1, 0.2f}};
    float L, a, b;
    to_oklab(c, &L, &a, &b);
    float C = sqrtf(a * a + b * b);
    float flipped = to_dark ? curve(L, dark_curve, 5) : curve(L, light_curve, 5);
    float keep = fminf(C / 0.15f, 1) * 0.55f;
    float nl = flipped + (L - flipped) * keep;
    /* deep colours lose a little colour, so dark surfaces don't turn muddy */
    float k = to_dark && nl < 0.35f ? 0.75f : 1;
    color out = c;
    for (int i = 0; i < 6; i++, k *= 0.8f) /* bring it back inside sRGB */
        if (from_oklab(nl, a * k, b * k, C_A(c), &out))
            break;
    return out;
}

static color filter(color c, bool to_dark) {
    /* Styles are worked out every frame, so remember recent answers. */
    static struct {
        color in, out;
        bool dark, used;
    } cache[512];
    unsigned h = (c * 2654435761u) >> 23;
    if (cache[h].used && cache[h].in == c && cache[h].dark == to_dark)
        return cache[h].out;
    color out = derive(c, to_dark);
    cache[h].in = c, cache[h].out = out, cache[h].dark = to_dark, cache[h].used = true;
    return out;
}
static color filter_dark(color c) { return filter(c, true); }
static color filter_light(color c) { return filter(c, false); }

color theme_fit(color fg, color bg, float min) {
    if (color_contrast(fg, bg) >= min)
        return fg;
    /* toward whichever end can get there: away from the background */
    color w = RGBA(255, 255, 255, C_A(fg)), k = RGBA(0, 0, 0, C_A(fg));
    color toward = color_contrast(w, bg) >= color_contrast(k, bg) ? w : k;
    for (int i = 1; i <= 20; i++) {
        color c = color_mix(fg, toward, i / 20.0f);
        if (color_contrast(c, bg) >= min)
            return c;
    }
    return toward;
}

color theme_under(rect r) {
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 8; i++) {
            int x = r.x + r.w * (2 * i + 1) / 16, y = r.y + r.h * (2 * j + 1) / 8;
            if (x < 0 || y < 0 || x >= A.W || y >= A.H)
                continue;
            uint32_t p = A.frame->px[(size_t)y * A.W + x];
            sr += C_R(p), sg += C_G(p), sb += C_B(p), n++;
        }
    return n ? RGBA(sr / n, sg / n, sb / n, 255) : A.d.surface;
}

color theme_ink(rect r, color want, float min) { return theme_fit(want, theme_under(r), min); }

/* ---- the scheme ---- */

static void load_pref(void) {
    TH.loaded = true;
    FILE *f = fopen(THEME_FILE, "r");
    char w[16] = "";
    if (f) {
        if (!fgets(w, sizeof w, f))
            w[0] = 0;
        fclose(f);
    }
    TH.pref = !strncmp(w, "dark", 4) ? PREF_DARK : !strncmp(w, "light", 5) ? PREF_LIGHT : PREF_AUTO;
    LOG("theme: %s\n", TH.pref == PREF_DARK ? "dark" : TH.pref == PREF_LIGHT ? "light" : "as designed");
}

static void save_pref(void) {
    FILE *f = fopen(THEME_FILE, "w");
    if (f) {
        fputs(TH.pref == PREF_DARK ? "dark\n" : TH.pref == PREF_LIGHT ? "light\n" : "auto\n", f);
        fclose(f);
    }
}

/* Does the stylesheet have colours of its own for this scheme? */
static bool has_block(const char *css, bool dark) {
    if (!css)
        return false;
    for (const char *p = strchr(css, '@'); p; p = strchr(p + 1, '@')) {
        if (!strncasecmp(p, dark ? "@dark" : "@light", dark ? 5 : 6))
            return true;
        if (!strncasecmp(p, "@media", 6)) {
            const char *brace = strchr(p, '{'), *m = strcasestr(p, "prefers-color-scheme");
            if (m && brace && m < brace) {
                const char *word = strcasestr(m, dark ? "dark" : "light");
                if (word && word < brace)
                    return true;
            }
        }
    }
    return false;
}

/* Works the look out again for the current document and choice: which
 * scheme, whose colours, and the design record the apps draw from. */
static void apply(void) {
    fm_doc *d = A.ui;
    if (!d)
        return;
    /* the design as drawn, to learn its natural scheme */
    fm_scheme = 0;
    fm_color_filter = NULL;
    fm_css_load(d);
    color probe = A.d.surface;
    const char *v = fm_var(d, "--surface");
    if (!v || !fm_parse_color_raw(v, &probe)) {
        v = fm_var(d, "--bg");
        if (v)
            fm_parse_color_raw(v, &probe);
    }
    TH.natural_dark = color_luma(probe | 0xff000000u) < 0.4f;
    TH.dark = TH.pref == PREF_AUTO ? TH.natural_dark : TH.pref == PREF_DARK;
    TH.derived = TH.dark != TH.natural_dark && !has_block(d->css, TH.dark);
    fm_scheme = TH.dark ? 2 : 1;
    fm_color_filter = TH.derived ? (TH.dark ? filter_dark : filter_light) : NULL;
    fm_css_load(d);
    TH.doc = d;
    TH.css = d->css;
    TH.css_len = d->css ? strlen(d->css) : 0;

    /* The apps draw from the design record: give it this scheme's colours. */
    design nd;
    if (design_from_facet(&nd, d, &A.prof, NULL, 0)) {
        A.d.bg = nd.bg, A.d.bg2 = nd.bg2, A.d.surface = nd.surface, A.d.surface_opacity = nd.surface_opacity;
        A.d.surface_text = nd.surface_text, A.d.text = nd.text, A.d.muted = nd.muted;
        A.d.accent = nd.accent, A.d.accent2 = nd.accent2;
        bool has_wall = false;
        for (fm_node *n = d->root ? d->root->first : NULL; n; n = n->next)
            if (n->kind == FM_ELEM && !strcasecmp(n->tag, "wallpaper") && fm_attr_get(n, "colors"))
                has_wall = true;
        if (has_wall)
            memcpy(A.d.wp, nd.wp, sizeof nd.wp);
    }
    if (A.wall)
        wallpaper_paint(A.wall, &A.d);
    LOG("theme: %s%s (drawn %s)\n", TH.dark ? "dark" : "light", TH.derived ? ", derived" : "",
        TH.natural_dark ? "dark" : "light");
}

static void request(int pref) {
    if (!TH.loaded)
        load_pref();
    TH.pref = pref;
    save_pref();
    TH.pending = true;
}

void theme_set_dark(bool dark) { request(dark ? PREF_DARK : PREF_LIGHT); }
bool theme_is_dark(void) {
    if (TH.doc)
        return TH.dark;
    if (!TH.loaded)
        load_pref();
    return TH.pref == PREF_AUTO ? color_luma(A.d.surface) < 0.4f : TH.pref == PREF_DARK;
}
void theme_toggle(void) { theme_set_dark(!theme_is_dark()); }
void theme_follow_design(void) { request(PREF_AUTO); }

bool theme_action(const char *what) {
    const char *w = !strncasecmp(what, "theme:", 6) ? what + 6 : what;
    if (!strcasecmp(w, "toggle"))
        theme_toggle();
    else if (!strcasecmp(w, "dark"))
        theme_set_dark(true);
    else if (!strcasecmp(w, "light"))
        theme_set_dark(false);
    else if (!strcasecmp(w, "auto"))
        theme_follow_design();
    else
        return false;
    return true;
}

void theme_crossfade(void) { TH.snap = true; }

void theme_frame_begin(void) {
    if (!TH.loaded)
        load_pref();
    /* Ctrl+Shift+D anywhere. The key is taken, so the terminal never sees
     * it as Ctrl+D (end of input). */
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.ctrl && A.in.shift && (A.in.keys[i] == 'D' || A.in.keys[i] == 'd')) {
            theme_toggle();
            memmove(&A.in.keys[i], &A.in.keys[i + 1], sizeof A.in.keys[0] * (size_t)(A.in.nkeys - i - 1));
            A.in.nkeys--;
            i--;
        }
    fm_doc *d = A.ui;
    if (!d)
        return;
    /* A new design, or the companion restyled this one: work it out again
     * (no fade: the change itself is what they are looking at). */
    if (!TH.pending && (d != TH.doc || d->css != TH.css || (d->css ? strlen(d->css) : 0) != TH.css_len))
        apply();
}

void theme_frame_end(void) {
    if (TH.snap && A.frame && !A.prof.reduce_motion) {
        TH.snap = false;
        if (!TH.before || TH.before->w != A.frame->w || TH.before->h != A.frame->h) {
            canvas_free(TH.before);
            TH.before = canvas_new(A.frame->w, A.frame->h);
        }
        memcpy(TH.before->px, A.frame->px, sizeof(uint32_t) * (size_t)A.frame->w * (size_t)A.frame->h);
        TH.fade_at = A.now;
        return;
    }
    TH.snap = false;
    if (TH.pending) {
        TH.pending = false;
        bool motion = !A.prof.reduce_motion && A.frame;
        if (motion) {
            if (!TH.before || TH.before->w != A.frame->w || TH.before->h != A.frame->h) {
                canvas_free(TH.before);
                TH.before = canvas_new(A.frame->w, A.frame->h);
            }
            memcpy(TH.before->px, A.frame->px, sizeof(uint32_t) * (size_t)A.frame->w * (size_t)A.frame->h);
            TH.fade_at = A.now;
        }
        bool was = TH.dark;
        apply();
        if (was == TH.dark)
            TH.fade_at = 0;
        return;
    }
    /* the old look fades out over the new one */
    if (TH.fade_at && TH.before) {
        float t = (float)(A.now - TH.fade_at) / FADE_MS;
        if (t >= 1) {
            TH.fade_at = 0;
            return;
        }
        float e = 1 - t * t * (3 - 2 * t); /* smoothstep, reversed */
        uint32_t a = (uint32_t)(e * 256), ia = 256 - a;
        uint32_t *dst = A.frame->px, *src = TH.before->px;
        size_t n = (size_t)A.frame->w * (size_t)A.frame->h;
        for (size_t i = 0; i < n; i++) {
            uint32_t p = dst[i], q = src[i];
            uint32_t rb = (((q & 0xff00ff) * a + (p & 0xff00ff) * ia) >> 8) & 0xff00ff;
            uint32_t g = (((q & 0x00ff00) * a + (p & 0x00ff00) * ia) >> 8) & 0x00ff00;
            dst[i] = 0xff000000u | rb | g;
        }
    }
}

/* ---- the terminal ---- */

/* Classic hues: the 16 colours programs expect, tuned for a dark and a
 * light ground. They are fitted to the real background afterwards. */
static const uint32_t ansi_dark[16] = {0x1c1f26, 0xff6b6b, 0x8bd17c, 0xf4c96b, 0x6cb6ff, 0xd291ff, 0x5fd7d7, 0xd8dbe2,
                                       0x5c6370, 0xff8c8c, 0xa8e69a, 0xffdc8a, 0x91c9ff, 0xe2b0ff, 0x86ecec, 0xffffff};
static const uint32_t ansi_light[16] = {0x1d1f24, 0xc8323a, 0x2e8a3a, 0x9a6b00, 0x1f63c6, 0x8a3fc4, 0x14838a, 0x6b7080,
                                        0x4b5060, 0xd9444c, 0x3a9e48, 0xb37f00, 0x2b78e4, 0x9d52d6, 0x1c99a1, 0x30343c};

static color hexrgb(uint32_t v) { return RGBA((v >> 16) & 255, (v >> 8) & 255, v & 255, 255); }

/* A value from --term-<name>, else the `terminal { prop }` rule. */
static const char *term_value(const fm_doc *d, const char *var, const char *prop) {
    const char *v = d ? fm_var(d, var) : NULL;
    return v ? v : d && prop ? fm_rule_value(d, "terminal", prop) : NULL;
}

static bool term_color(const fm_doc *d, const char *var, const char *prop, color *out) {
    const char *v = term_value(d, var, prop);
    return v && fm_parse_color_raw(v, out);
}

void theme_term_colors(struct term_colors *tc, color surface, color text) {
    fm_doc *d = A.ui;
    memset(tc, 0, sizeof *tc);
    color c;
    bool set_bg = term_color(d, "--term-bg", "background", &c);
    if (!set_bg)
        set_bg = term_color(d, "--term-background", "background-color", &c);
    if (set_bg) {
        tc->bg = c | 0xff000000u;
    } else {
        bool dk = color_luma(surface) < 0.45f;
        tc->bg = color_mix(surface | 0xff000000u, dk ? RGBA(0, 0, 0, 255) : RGBA(255, 255, 255, 255), 0.35f);
    }
    /* A colour the design wrote for one scheme follows a derived switch
     * only when it no longer fits: a black console stays black at night. */
    bool bg_dark = color_luma(tc->bg) < 0.45f;
    color (*map)(color) = set_bg && TH.derived && bg_dark != TH.dark ? fm_color_filter : NULL;
    if (map)
        tc->bg = map(tc->bg), bg_dark = !bg_dark;
    bool dark = bg_dark;

    bool set_fg = term_color(d, "--term-fg", "color", &c) || term_color(d, "--term-foreground", NULL, &c);
    tc->fg = set_fg ? (map ? map(c) : c) | 0xff000000u : text;
    tc->fg = theme_fit(tc->fg, tc->bg, set_fg ? 3.0f : 7.0f);

    /* the palette: given whole, or the classic hues fitted to this ground */
    const char *pal = term_value(d, "--term-palette", "palette");
    int got = 0;
    for (const char *p = pal; p && *p && got < 16;) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')
            p++;
        if (!*p)
            break;
        char one[48];
        int k = 0;
        if (!strncasecmp(p, "rgb", 3)) {
            while (p[k] && p[k] != ')' && k < 46)
                k++;
            k++;
        } else {
            while (p[k] && p[k] != ' ' && p[k] != ',' && k < 46)
                k++;
        }
        snprintf(one, sizeof one, "%.*s", k, p);
        p += k;
        if (fm_parse_color_raw(one, &c))
            tc->ansi[got++] = (map ? map(c) : c) | 0xff000000u;
    }
    for (int i = got; i < 16; i++) {
        color base = hexrgb(dark ? ansi_dark[i] : ansi_light[i]);
        if (i == 0)
            base = dark ? color_mix(tc->bg, RGBA(0, 0, 0, 255), 0.3f) : color_mix(tc->fg, RGBA(0, 0, 0, 255), 0.2f);
        else if (i == 7)
            base = dark ? color_mix(tc->fg, tc->bg, 0.15f) : color_mix(tc->bg, RGBA(0, 0, 0, 255), 0.18f);
        else if (i == 15)
            base = dark ? RGBA(255, 255, 255, 255) : color_mix(tc->fg, RGBA(0, 0, 0, 255), 0.4f);
        /* text colours must read; 0 (dark) and 7 (light) are grounds, not text */
        bool ground = (dark && i == 0) || (!dark && i == 7);
        tc->ansi[i] = ground ? base : theme_fit(base, tc->bg, i == 8 ? 3.0f : 4.5f);
    }

    bool set_cur = term_color(d, "--term-cursor", "caret-color", &c);
    tc->cursor = set_cur ? (map ? map(c) : c) | 0xff000000u : theme_fit(A.d.accent, tc->bg, 3.0f);
    if (term_color(d, "--term-selection", "selection", &c))
        tc->selection = (map ? map(c) : c) | 0xff000000u;
    else
        tc->selection = color_mix(tc->bg, A.d.accent, 0.35f);
    const char *shape = term_value(d, "--term-cursor-shape", "cursor");
    tc->cursor_shape = !shape ? 0 : strcasestr(shape, "bar") || strcasestr(shape, "beam") ? 1
                                  : strcasestr(shape, "under") ? 2 : 0;
    const char *fam = term_value(d, "--term-font", "font-family");
    if (fam) {
        while (*fam == ' ' || *fam == '"' || *fam == '\'')
            fam++;
        int k = 0;
        while (fam[k] && fam[k] != '"' && fam[k] != '\'' && fam[k] != ',' && k < 31)
            k++;
        snprintf(tc->font, sizeof tc->font, "%.*s", k, fam);
    }
}

/* ---- window slots ---- */

static bool slot_info(rect content, rect *frame, float rad[4], float *border, rect *slot) {
    rect inner;
    if (!A.ui || !fm_window_slot(A.ui, frame, rad, border, slot, &inner))
        return false;
    /* only the frame just drawn around this very content */
    return inner.x == content.x && inner.y == content.y && inner.w == content.w && inner.h == content.h;
}

rect theme_slot_box(rect content) {
    rect frame, slot;
    float rad[4], border;
    return slot_info(content, &frame, rad, &border, &slot) ? slot : content;
}

void theme_fill_slot(rect r, color c) {
    rect frame, slot, inner;
    float rad[4], border;
    /* r is a slot from theme_slot_box (or anything else, filled plainly) */
    if (!A.ui || !fm_window_slot(A.ui, &frame, rad, &border, &slot, &inner) || slot.x != r.x || slot.y != r.y ||
        slot.w != r.w || slot.h != r.h) {
        fill_rect(A.frame, r, c);
        return;
    }
    /* Round the bottom corners when the slot meets the frame's rounded ones. */
    int bottom = frame.y + frame.h - (int)border;
    float cr = fmaxf(rad[2], rad[3]) - border;
    float lim = fminf((float)r.w, (float)r.h) / 2;
    if (cr > lim)
        cr = lim;
    if (r.y + r.h >= bottom - 1 && cr > 0.5f) {
        fill_round(A.frame, r, cr, c);
        fill_rect(A.frame, (rect){r.x, r.y, r.w, r.h - (int)ceilf(cr)}, c);
    } else {
        fill_rect(A.frame, r, c);
    }
}
