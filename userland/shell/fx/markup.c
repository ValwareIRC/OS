/* A Facet 2 app's view as a Facet document for the desktop's engine
 * (fxwin.c draws it; tests/fx_shot.c pictures it on the build machine).
 *
 * Its look starts from the person's design, never from a stock one:
 * - every element has a considered default (buttons, inputs, lists);
 * - the built-in classes (card, glass, hero, primary, pill, title, grid3,
 *   lift...) are computed from this design: its material (glass, frost,
 *   solid or neon), corners, shadows, glow, colours and fonts. The same
 *   class="card lift" is frosted in one design and neon-edged in another;
 * - the app's own rules come last, with token(...) as this design's values
 *   and Facet 2's short property names (gradient, depth, glow, scale, move,
 *   text, upper, columns...) said in the engine's words.
 * Inputs, fields, checkboxes and widgets (ring, progress, chart) become boxes
 * (fxin="N") the shell draws into (fxwin.c, widgets.c). */
#define _GNU_SOURCE
#include "markup.h"
#include "../../lib/json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D (*dz)
static const design *dz;

typedef struct {
    fx_out **acts;
    int nacts;
} acts_t;

static void hex(char *out, size_t n, color c) { snprintf(out, n, "#%02x%02x%02x", C_R(c), C_G(c), C_B(c)); }
static void rgba(char *out, size_t n, color c, float a) {
    snprintf(out, n, "rgba(%d, %d, %d, %.2f)", C_R(c), C_G(c), C_B(c), (double)a);
}
static color readable(color bg) {
    float l = (0.299f * C_R(bg) + 0.587f * C_G(bg) + 0.114f * C_B(bg)) / 255.0f;
    return l > 0.6f ? RGBA(20, 18, 28, 255) : RGBA(255, 255, 255, 255);
}
static bool dark_surface(void) {
    color s = D.surface;
    return (0.299f * C_R(s) + 0.587f * C_G(s) + 0.114f * C_B(s)) / 255.0f < 0.5f;
}
static color danger_c(void) { return color_mix(RGBA(214, 64, 64, 255), D.accent, 0.15f); }
static color success_c(void) { return color_mix(RGBA(60, 170, 100, 255), D.accent, 0.15f); }

/* token(name) in this design */
void fx_token_value(const design *d, const char *name, char *out, size_t n) {
    const design *keep = dz;
    dz = d;
    color c;
    if (!strcmp(name, "accent")) c = D.accent;
    else if (!strcmp(name, "accent2")) c = D.accent2;
    else if (!strcmp(name, "background")) c = D.bg;
    else if (!strcmp(name, "surface")) c = D.surface;
    else if (!strcmp(name, "surface2")) c = color_mix(D.surface, D.surface_text, 0.07f);
    else if (!strcmp(name, "text")) c = D.surface_text;
    else if (!strcmp(name, "muted")) c = D.muted;
    else if (!strcmp(name, "danger")) c = danger_c();
    else if (!strcmp(name, "success")) c = success_c();
    else if (!strcmp(name, "border")) c = color_mix(D.surface, D.surface_text, 0.2f);
    else if (!strcmp(name, "font-body")) { snprintf(out, n, "%s", D.font_body); dz = keep; return; }
    else if (!strcmp(name, "font-heading")) { snprintf(out, n, "%s", D.font_heading); dz = keep; return; }
    else if (!strcmp(name, "font-mono")) { snprintf(out, n, "JetBrainsMono"); dz = keep; return; }
    else c = D.surface_text;
    hex(out, n, c);
    dz = keep;
}

/* the shadow for depth 0..3 in this design (none where the design has none; neon gives light instead) */
static void depth_shadow(int level, char *out, size_t n) {
    if (level <= 0 || (!D.shadows && D.material != MAT_NEON)) {
        snprintf(out, n, "box-shadow: none;");
        return;
    }
    if (D.material == MAT_NEON) {
        char g[48];
        rgba(g, sizeof g, D.accent, 0.25f + 0.15f * (float)level);
        snprintf(out, n, "glow: %s %d;", g, 6 + 6 * level);
        return;
    }
    static const int y[] = {0, 2, 6, 14}, blur[] = {0, 8, 18, 34};
    static const float a[] = {0, 0.18f, 0.26f, 0.34f};
    /* on a light design a shadow is a faint tint of its own ink, not grey smoke */
    bool light = (0.299f * C_R(D.surface) + 0.587f * C_G(D.surface) + 0.114f * C_B(D.surface)) / 255.0f >= 0.5f;
    color sc = light ? color_mix(D.surface_text, D.accent, 0.2f) : RGBA(0, 0, 0, 255);
    snprintf(out, n, "box-shadow: 0 %d %d rgba(%d, %d, %d, %.2f);", y[level], blur[level], C_R(sc), C_G(sc), C_B(sc),
             (double)(a[level] * (light ? 0.45f : 1)));
}

/* ---- the app's rules, in the engine's words ---- */

static void value_tokens(sbuf *b, const char *v, size_t n) {
    const char *end = v + n;
    for (const char *p = v; p < end;) {
        if (!strncmp(p, "token(", 6)) {
            const char *e = memchr(p, ')', (size_t)(end - p));
            if (e) {
                char name[48], val[64];
                snprintf(name, sizeof name, "%.*s", (int)(e - p - 6) < 47 ? (int)(e - p - 6) : 47, p + 6);
                fx_token_value(dz, name, val, sizeof val);
                sb_add(b, val);
                p = e + 1;
                continue;
            }
        }
        sb_addn(b, p, 1);
        p++;
    }
}

static void declaration(sbuf *b, const char *prop, const char *val) {
    sbuf v;
    sb_init(&v);
    value_tokens(&v, val, strlen(val));
    const char *s = v.p ? v.p : "";
    char out[256];
    if (!strcmp(prop, "size")) sb_printf(b, "font-size: %s;", s);
    else if (!strcmp(prop, "weight")) sb_printf(b, "font-weight: %s;", s);
    else if (!strcmp(prop, "font")) sb_printf(b, "font-family: %s;", s);
    else if (!strcmp(prop, "text")) sb_printf(b, "text-align: %s;", s);
    else if (!strcmp(prop, "line")) sb_printf(b, "line-height: %s;", s);
    else if (!strcmp(prop, "upper")) sb_printf(b, "text-transform: %s;", strstr(s, "false") ? "none" : "uppercase");
    else if (!strcmp(prop, "columns")) sb_printf(b, "display: grid; columns: %s;", s);
    else if (!strcmp(prop, "scale")) sb_printf(b, "transform: scale(%s);", s);
    else if (!strcmp(prop, "outline")) sb_printf(b, "border: 1 %s;", s);
    else if (!strcmp(prop, "move")) {
        float x = 0, y = 0;
        sscanf(s, "%f %f", &x, &y);
        sb_printf(b, "transform: translate(%g, %g);", (double)x, (double)y);
    } else if (!strcmp(prop, "depth")) {
        depth_shadow(atoi(s), out, sizeof out);
        sb_add(b, out);
    } else if (!strcmp(prop, "gradient")) { /* two colours and an optional angle */
        char a[64] = "", c2[64] = "";
        int ang = 135;
        sscanf(s, "%63s %63s %d", a, c2, &ang);
        sb_printf(b, "background: linear(%s, %s, %d);", a, c2[0] ? c2 : a, ang);
    } else sb_printf(b, "%s: %s;", prop, s);
    free(v.p);
}

static void convert_style(sbuf *b, const char *style) {
    const char *p = style;
    while (*p) {
        /* selector: #id -> .fx-id-id */
        const char *s = p;
        while (*p && *p != '{')
            p++;
        for (const char *q = s; q < p; q++) {
            if (*q == '#')
                sb_add(b, ".fx-id-");
            else
                sb_addn(b, q, 1);
        }
        if (!*p)
            return;
        sb_add(b, "{ ");
        p++;
        /* declarations */
        while (*p && *p != '}') {
            while (*p && (isspace((unsigned char)*p) || *p == ';'))
                p++;
            if (!*p || *p == '}')
                break;
            const char *ps = p;
            while (*p && *p != ':' && *p != ';' && *p != '}')
                p++;
            if (*p != ':')
                continue;
            char prop[48];
            int pl = (int)(p - ps);
            while (pl > 0 && isspace((unsigned char)ps[pl - 1]))
                pl--;
            snprintf(prop, sizeof prop, "%.*s", pl < 47 ? pl : 47, ps);
            p++;
            while (*p == ' ' || *p == '\t')
                p++;
            const char *vs = p;
            while (*p && *p != ';' && *p != '}' && *p != '\n')
                p++;
            int vl = (int)(p - vs);
            while (vl > 0 && isspace((unsigned char)vs[vl - 1]))
                vl--;
            char val[200];
            snprintf(val, sizeof val, "%.*s", vl < 199 ? vl : 199, vs);
            declaration(b, prop, val);
            sb_add(b, " ");
        }
        if (*p == '}')
            p++;
        sb_add(b, "}\n");
    }
}

/* ---- the built-in look, from this design ---- */

static void base_css(sbuf *b) {
    char ink[16], muted[16], acc[16], acc2[16], on_acc[16], field[16], fieldh[16], line[40], soft[40], softh[40],
        quiet[40], dang[40], dangi[16], succ[40], succi[16], cardbg[48], cardborder[64], glassbg[48], sunken[40],
        badge[40], d1[64], d2[64], d3[64], glow[40], track[40];
    color ink_c = D.surface_text;
    bool dark = dark_surface();
    hex(ink, sizeof ink, ink_c);
    hex(muted, sizeof muted, D.muted);
    hex(acc, sizeof acc, D.accent);
    hex(acc2, sizeof acc2, D.accent2);
    hex(on_acc, sizeof on_acc, readable(D.accent));
    hex(field, sizeof field, color_mix(D.surface, ink_c, 0.07f));
    hex(fieldh, sizeof fieldh, color_mix(D.surface, ink_c, 0.13f));
    rgba(line, sizeof line, ink_c, 0.14f);
    rgba(soft, sizeof soft, D.accent, 0.18f);
    rgba(softh, sizeof softh, D.accent, 0.30f);
    rgba(quiet, sizeof quiet, ink_c, 0.08f);
    rgba(dang, sizeof dang, danger_c(), 0.18f);
    hex(dangi, sizeof dangi, color_mix(danger_c(), ink_c, dark ? 0.35f : 0.1f));
    rgba(succ, sizeof succ, success_c(), 0.18f);
    hex(succi, sizeof succi, color_mix(success_c(), ink_c, dark ? 0.35f : 0.1f));
    rgba(sunken, sizeof sunken, dark ? RGBA(0, 0, 0, 255) : ink_c, dark ? 0.18f : 0.05f);
    rgba(badge, sizeof badge, D.accent, 0.22f);
    rgba(track, sizeof track, ink_c, 0.12f);
    rgba(glow, sizeof glow, D.accent, 0.35f + 0.4f * D.glow);
    depth_shadow(1, d1, sizeof d1);
    depth_shadow(2, d2, sizeof d2);
    depth_shadow(3, d3, sizeof d3);
    float R = D.radius < 4 ? 4 : D.radius, r = R * 0.55f < 4 ? 4 : R * 0.55f;
    float blur = D.blur;
    /* a card is the design's own material */
    switch (D.material) {
    case MAT_GLASS:
        rgba(cardbg, sizeof cardbg, D.surface, 0.55f);
        snprintf(cardborder, sizeof cardborder, "border: 1 %s; blur: %.0f;", line, (double)(blur > 6 ? blur : 10));
        break;
    case MAT_FROST:
        rgba(cardbg, sizeof cardbg, color_mix(D.surface, ink_c, 0.04f), 0.85f);
        snprintf(cardborder, sizeof cardborder, "border: 1 %s; blur: %.0f;", line, (double)(blur > 4 ? blur : 8));
        break;
    case MAT_NEON: {
        char edge[40];
        rgba(edge, sizeof edge, D.accent, 0.55f);
        rgba(cardbg, sizeof cardbg, color_mix(D.bg, D.surface, 0.5f), 0.8f);
        snprintf(cardborder, sizeof cardborder, "border: 1 %s;", edge);
        break;
    }
    default:
        hex(cardbg, sizeof cardbg, color_mix(D.surface, ink_c, dark ? 0.06f : 0.035f));
        snprintf(cardborder, sizeof cardborder, "border: 1 %s;", line);
        break;
    }
    rgba(glassbg, sizeof glassbg, D.surface, 0.42f);
    float hpx = D.heading_px > 20 ? D.heading_px : 28, bpx = D.body_px > 11 ? D.body_px : 15;
    sb_printf(b,
              ".fx-root { direction: column; gap: 12; padding: 18; color: %s; font-family: %s; font-size: %.0f; "
              "overflow: scroll; }\n"
              "row { direction: row; gap: 10; align: center; }\n"
              "column { direction: column; gap: 8; }\n"
              "text { color: %s; }\n"
              /* controls */
              "button { padding: 8 16; radius: %.0f; background: %s; color: %s; transition: 140; }\n"
              "button:hover { background: %s; }\n"
              ".fx-input { height: 40; min-width: 140; }\n"
              ".fx-field { min-height: 180; grow: 1; }\n"
              ".fx-check { width: 22; height: 22; }\n"
              ".fx-scroll { overflow: scroll; grow: 1; gap: 6; }\n"
              ".fx-disabled { opacity: 0.4; }\n"
              /* widgets the shell draws into */
              ".fx-w-ring { width: 104; height: 104; }\n"
              ".fx-w-progress { height: 10; min-width: 80; grow: 1; }\n"
              ".fx-w-chart { height: 160; min-width: 220; grow: 1; }\n"
              ".fx-w-avatar { width: 40; height: 40; radius: 9999; background: linear(%s, %s, 135); color: %s; "
              "align: center; justify: center; font-weight: 700; }\n"
              ".badge { padding: 2 9; radius: 9999; background: %s; color: %s; font-size: %.0f; }\n",
              ink, D.font_body, (double)bpx, ink, (double)r, field, ink, fieldh, acc, acc2, on_acc, badge, ink,
              (double)(bpx - 3));
    /* surfaces */
    sb_printf(b,
              ".card { background: %s; %s radius: %.0f; padding: 16; gap: 10; %s }\n"
              ".tile { background: %s; %s radius: %.0f; padding: 14; gap: 6; direction: column; min-height: 96; %s }\n"
              ".glass { background: %s; border: 1 %s; blur: %.0f; radius: %.0f; padding: 16; gap: 10; }\n"
              ".hero { background: linear(%s, %s, 135); color: %s; radius: %.0f; padding: 24; gap: 6; %s }\n"
              ".hero text { color: %s; }\n"
              ".sunken { background: %s; radius: %.0f; padding: 12; gap: 8; }\n"
              ".outline { background: none; border: 1 %s; radius: %.0f; padding: 14; }\n"
              ".raised { %s }\n .floating { %s }\n",
              cardbg, cardborder, (double)R, d1, cardbg, cardborder, (double)R, d1, glassbg, line,
              (double)(blur > 8 ? blur + 4 : 14), (double)R, acc, acc2, on_acc, (double)(R * 1.2f), d2, on_acc, sunken,
              (double)(R * 0.75f), line, (double)R, d2, d3);
    /* emphasis */
    sb_printf(b,
              ".primary { background: %s; color: %s; }\n .primary:hover { background: %s; }\n"
              ".soft { background: %s; color: %s; }\n .soft:hover { background: %s; }\n"
              ".quiet { background: none; }\n .quiet:hover { background: %s; }\n"
              ".danger { background: %s; color: %s; }\n .success { background: %s; color: %s; }\n"
              ".glow { glow: %s %.0f; }\n",
              acc, on_acc, acc2, soft, ink, softh, quiet, dang, dangi, succ, succi, glow, (double)(10 + 14 * D.glow));
    /* shape, type, layout, motion */
    sb_printf(b,
              ".pill { radius: 9999; padding: 6 16; }\n .circle { radius: 9999; }\n"
              ".title { font-family: %s; font-size: %.0f; font-weight: 700; }\n"
              ".heading { font-family: %s; font-size: %.0f; font-weight: 600; }\n"
              ".label { font-size: %.0f; color: %s; text-transform: uppercase; }\n"
              ".subtle { color: %s; }\n"
              ".big { font-family: %s; font-size: %.0f; font-weight: 700; }\n"
              ".mono { font-family: JetBrainsMono; }\n"
              ".left { text-align: start; justify: start; }\n .center { align: center; justify: center; text-align: center; }\n"
              ".spread { justify: space-between; }\n .wrap { flex-wrap: wrap; }\n .fill { grow: 1; }\n"
              ".stack { direction: column; }\n"
              ".grid2 { display: grid; columns: 2; gap: 14; align: stretch; }\n"
              ".grid3 { display: grid; columns: 3; gap: 14; align: stretch; }\n"
              ".grid4 { display: grid; columns: 4; gap: 14; align: stretch; }\n"
              ".tight { gap: 4; }\n .airy { gap: 18; padding: 20; }\n"
              ".lift { transition: %.0f; }\n .lift:hover { transform: translate(0, -3); %s }\n"
              ".track { background: %s; }\n",
              D.font_heading, (double)(hpx * 0.95f), D.font_heading, (double)(bpx + 5), (double)(bpx - 3), muted, muted,
              D.font_heading, (double)(hpx * 1.35f), (double)(120 + 200 * D.motion), d3, track);
}

/* ---- the view ---- */

static void esc(sbuf *b, const char *s) {
    for (; *s; s++)
        switch (*s) {
        case '<': sb_add(b, "&lt;"); break;
        case '>': sb_add(b, "&gt;"); break;
        case '&': sb_add(b, "&amp;"); break;
        case '"': sb_add(b, "&quot;"); break;
        default: sb_addn(b, s, 1);
        }
}

static bool has_handler(const fx_out *o, const char *ev) {
    for (int i = 0; i < o->nh; i++)
        if (!strcmp(o->h[i].event, ev))
            return true;
    return false;
}

bool fx_is_widget(const char *tag) {
    return !strcmp(tag, "ring") || !strcmp(tag, "progress") || !strcmp(tag, "chart");
}

static void emit_markup(acts_t *fw, sbuf *b, fx_out *o) {
    if (!o->tag) {
        esc(b, o->text ? o->text : "");
        return;
    }
    const char *t = o->tag;
    bool input = !strcmp(t, "input"), fieldm = !strcmp(t, "field"), check = !strcmp(t, "checkbox");
    bool widget = fx_is_widget(t), avatar = !strcmp(t, "avatar"), badge = !strcmp(t, "badge");
    bool boxed = input || fieldm || check || widget;
    const char *tag = !strcmp(t, "scroll") ? "column"
                      : boxed || avatar || badge || !strcmp(t, "popover") || !strcmp(t, "sheet") ||
                                !strcmp(t, "image") || !strcmp(t, "canvas")
                          ? (badge ? "text" : "panel")
                          : t;
    sb_printf(b, "<%s", tag);
    /* ids repeat in lists (one per item), so they go as a class (#x in its rules becomes .fx-id-x) */
    const char *id = fx_get(o, "id"), *cls = fx_get(o, "class");
    sb_add(b, " class=\"");
    if (cls)
        esc(b, cls);
    if (id) {
        sb_add(b, " fx-id-");
        esc(b, id);
    }
    if (input) sb_add(b, " fx-input");
    if (fieldm) sb_add(b, " fx-field");
    if (check) sb_add(b, " fx-check");
    if (widget) sb_printf(b, " fx-w-%s", t);
    if (avatar) sb_add(b, " fx-w-avatar");
    if (badge) sb_add(b, " badge");
    if (!strcmp(t, "scroll")) sb_add(b, " fx-scroll");
    if (fx_get(o, "disabled")) sb_add(b, " fx-disabled");
    sb_add(b, "\"");
    bool clickable = has_handler(o, "click") && !fx_get(o, "disabled");
    if (clickable || boxed) {
        fw->acts = realloc(fw->acts, sizeof *fw->acts * (size_t)(fw->nacts + 1));
        fw->acts[fw->nacts] = o;
        if (boxed)
            sb_printf(b, " fxin=\"%d\"", fw->nacts);
        if (clickable && !input && !fieldm && !check)
            sb_printf(b, " what=\"fx:%d\"", fw->nacts);
        fw->nacts++;
    }
    if (!strcmp(t, "icon") && fx_get(o, "name"))
        sb_printf(b, " name=\"%s\"", fx_get(o, "name"));
    sb_add(b, ">");
    if (avatar) { /* initials of its text */
        const char *s = fx_get(o, "text");
        char ini[8] = "";
        int k = 0;
        for (const char *q = s ? s : ""; *q && k < 2; q++)
            if (isalnum((unsigned char)*q) && (q == s || q[-1] == ' '))
                ini[k++] = (char)toupper((unsigned char)*q);
        ini[k] = 0;
        esc(b, ini);
    }
    for (int i = 0; i < o->nkids; i++)
        emit_markup(fw, b, o->kids[i]);
    sb_printf(b, "</%s>", tag);
}

char *fx_markup(fx_out *view, const fx_app *app, const design *d, fx_out ***acts, int *nacts) {
    dz = d;
    acts_t at = {NULL, 0};
    sbuf b;
    sb_init(&b);
    sb_add(&b, "<facet><desktop><column class=\"fx-root\" place=\"fill\" over=\"screen\" layer=\"below\">");
    for (int i = 0; i < view->nkids; i++)
        emit_markup(&at, &b, view->kids[i]);
    sb_add(&b, "</column></desktop><style>\n");
    base_css(&b);
    if (app->style)
        convert_style(&b, app->style);
    for (int i = 0; i < app->ncomps; i++)
        if (app->comps[i].style)
            convert_style(&b, app->comps[i].style);
    sb_add(&b, "</style></facet>");
    *acts = at.acts;
    *nacts = at.nacts;
    return b.p;
}
