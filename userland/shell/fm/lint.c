#define _GNU_SOURCE
#include "lint.h"
#include "fmi.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The design lint. A model writing a desktop blind makes the same few
 * mistakes again and again: pale text on a pale bar, a search field that
 * grows to fill a whole column, a start menu taller than the screen, a
 * label cut to "Contr...", an app nothing opens. Each is easy to see once
 * the design is laid out and drawn, and easy to fix once someone says
 * exactly where it is. This file does the looking and the saying.
 *
 * It draws the design offscreen in each state that matters (everything
 * closed with a window open, then each popover open), watches every run of
 * text as it is drawn (fm_text_probe), and walks the laid-out tree. Each
 * finding names the element with a selector the model can use straight
 * away, says what is wrong in numbers, and suggests the change. */

bool fm_lint_offscreen;

enum kind {
    K_UNREACH,
    K_OFFSCREEN,
    K_BIGPOP,
    K_OVERLAP,
    K_CLIP,
    K_CONTRAST,
    K_TRUNC,
    K_DEGEN,
    K_STRETCH,
    K_TARGET,
    K_SMALLTEXT,
    K_EMPTY,
    K_ORPHAN,
    K_WINDOW,
};

#define MAX_FIND 96

struct finding {
    int kind, weight, count;
    char key[128];
    char msg[320];
};

static struct {
    struct finding f[MAX_FIND];
    int n;
    const fm_lint_opts *o;
    fm_doc *d;
    bool reach[64];
    bool reach_all;           /* a search field opens anything by name */
    char opened[32][48];      /* popover ids something visible toggles */
    int nopened;
} L;

static void add(int kind, int weight, const char *key, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void add(int kind, int weight, const char *key, const char *fmt, ...) {
    for (int i = 0; i < L.n; i++)
        if (L.f[i].kind == kind && !strcmp(L.f[i].key, key)) {
            L.f[i].count++;
            return;
        }
    if (L.n == MAX_FIND)
        return;
    struct finding *f = &L.f[L.n++];
    f->kind = kind;
    f->weight = weight;
    f->count = 1;
    snprintf(f->key, sizeof f->key, "%s", key);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, sizeof f->msg, fmt, ap);
    va_end(ap);
}

/* ---- naming things the way a stylesheet would ---- */

static bool real_id(const fm_node *n) { return n && n->kind == FM_ELEM && n->id[0] && n->id[0] != '_'; }

static void seg(const fm_node *n, char *out, size_t len) {
    if (real_id(n)) {
        snprintf(out, len, "#%s", n->id);
        return;
    }
    const char *cls = n->generated ? NULL : fm_attr_get(n, "class");
    while (cls && *cls == ' ')
        cls++;
    if (cls && *cls) {
        int k = (int)strcspn(cls, " ");
        snprintf(out, len, "%s.%.*s", n->tag, k, cls);
    } else {
        snprintf(out, len, "%s", n->tag);
    }
}

/* "#startmenu #right search": the nearest ancestor with an id, then the
 * last two steps down to the element. Text is named by its element. */
static void path_of(const fm_node *n, char *out, size_t len) {
    if (n && n->kind == FM_TEXT)
        n = n->parent;
    const fm_node *chain[16];
    int k = 0;
    for (const fm_node *p = n; p && k < 16; p = p->parent) {
        if (p->kind != FM_ELEM || tag_is(p, "desktop") || tag_is(p, "template") || tag_is(p, "facet"))
            break;
        chain[k++] = p;
        if (real_id(p))
            break;
    }
    out[0] = 0;
    if (!k) {
        snprintf(out, len, "%s", n ? n->tag : "?");
        return;
    }
    size_t o = 0;
    char s[80];
    seg(chain[k - 1], s, sizeof s);
    o += (size_t)snprintf(out + o, len - o, "%s", s);
    int from = k - 2 > 1 ? 1 : k - 2;
    for (int i = from; i >= 0 && o < len; i--) {
        seg(chain[i], s, sizeof s);
        o += (size_t)snprintf(out + o, len - o, " %s", s);
    }
}

static void hexc(char *buf, size_t n, color c) { snprintf(buf, n, "#%02x%02x%02x", C_R(c), C_G(c), C_B(c)); }

/* ---- contrast, watched as the text is drawn ---- */

static color over(color fg, color bg) {
    unsigned a = C_A(fg), ia = 255 - a;
    return RGBA((C_R(fg) * a + C_R(bg) * ia) / 255, (C_G(fg) * a + C_G(bg) * ia) / 255,
                (C_B(fg) * a + C_B(bg) * ia) / 255, 255);
}

static void sample(canvas *c, rect r, color *lo, color *hi, color *avg) {
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    float llo = 2, lhi = -1;
    *lo = *hi = *avg = RGBA(0, 0, 0, 255);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 10; i++) {
            int x = r.x + (r.w * (2 * i + 1)) / 20, y = r.y + (r.h * (2 * j + 1)) / 8;
            if (x < 0 || y < 0 || x >= c->w || y >= c->h)
                continue;
            uint32_t p = c->px[(size_t)y * (size_t)c->w + (size_t)x] | 0xff000000u;
            float l = color_luma(p);
            if (l < llo) llo = l, *lo = p;
            if (l > lhi) lhi = l, *hi = p;
            sr += C_R(p), sg += C_G(p), sb += C_B(p), n++;
        }
    if (n)
        *avg = RGBA(sr / n, sg / n, sb / n, 255);
}

static float worst(color fg, color lo, color hi) {
    float a = color_contrast(over(fg, lo), lo), b = color_contrast(over(fg, hi), hi);
    return a < b ? a : b;
}

static void probe(canvas *c, fm_node *t, rect r, color col, float px) {
    if (!L.o || !C_A(col) || r.w <= 0 || r.h <= 0 || !t->parent)
        return;
    color lo, hi, avg;
    sample(c, r, &lo, &hi, &avg);
    float need = px >= 24 ? 3.0f : L.o->min_contrast;
    if (t->app == -2 && need > 3) /* a placeholder may be quieter, not invisible */
        need = 3;
    float ratio = worst(col, lo, hi);
    if (ratio + 0.05f >= need)
        return;
    /* The nearest colour that would read: the same hue pushed away from
     * what is behind it. */
    bool dark = color_luma(avg) < 0.45f;
    color solid = col | 0xff000000u, fix = dark ? RGBA(255, 255, 255, 255) : RGBA(0, 0, 0, 255);
    bool found = false;
    for (int k = 1; k <= 10 && !found; k++) {
        color m = color_mix(solid, fix, k * 0.1f) | 0xff000000u;
        if (worst(m, lo, hi) >= need)
            fix = m, found = true;
    }
    char path[160], key[160], text[40], want[8], behind[8];
    path_of(t, path, sizeof path);
    snprintf(key, sizeof key, "%s", path);
    snprintf(text, sizeof text, "%.24s%s", t->text ? t->text : "", t->text && strlen(t->text) > 24 ? "..." : "");
    for (char *p = text; *p; p++)
        if (*p == '\n' || *p == '"')
            *p = ' ';
    hexc(want, sizeof want, fix);
    hexc(behind, sizeof behind, avg);
    bool busy = color_contrast(lo, hi) > 2.5f;
    /* Already as light (or dark) as text gets: only the ground can move. */
    bool maxed = !found || (C_A(col) > 240 && (solid & 0xffffffu) == (fix & 0xffffffu));
    if (maxed)
        add(K_CONTRAST, ratio < 2 ? 6 : 4, key,
            "%s: \"%s\" is %.1f:1 against the %s%s%s behind it (needs %.1f:1); make what is behind it %s", path,
            text, ratio, busy ? "busy " : "", behind, busy ? " wallpaper" : "", need,
            busy ? "a solid or blurred background" : dark ? "darker" : "lighter");
    else
        add(K_CONTRAST, ratio < 2 ? 6 : 4, key,
            "%s: \"%s\" is %.1f:1 against the %s%s%s behind it (needs %.1f:1); use color:%s%s", path, text, ratio,
            busy ? "busy " : "", behind, busy ? " wallpaper" : "", need, want,
            busy ? " and give it a solid or blurred background" : " or change the background");
}

/* ---- drawing a state ---- */

static void settle(fm_doc *d, rect screen) {
    d->entrance_at = -1; /* no arrival animation: show it as it rests */
    for (int i = 0; i < 6; i++) {
        fm_layout(d, screen);
        fm_animate(d, 200);
    }
    fm_layout(d, screen);
}

static rect sample_window(fm_doc *d, const fm_lint_opts *o) {
    rect wk = d->work.w > 300 && d->work.h > 240 ? d->work : o->screen;
    int ww = wk.w - 120 < 780 ? wk.w - 120 : 780, wh = wk.h - 100 < 500 ? wk.h - 100 : 500;
    if (ww < 200) ww = wk.w;
    if (wh < 160) wh = wk.h;
    return (rect){wk.x + (wk.w - ww) / 2, wk.y + (wk.h - wh) / 2, ww, wh};
}

void fm_lint_paint(fm_doc *d, const fm_lint_opts *o, fm_node *open, bool window) {
    bool was = fm_lint_offscreen;
    fm_lint_offscreen = true;
    canvas *c = o->work;
    canvas_unclip(c);
    if (o->backdrop)
        canvas_blit(c, o->backdrop, 0, 0);
    else
        fill_rect(c, o->screen, fm_host_wallpaper_tone() | 0xff000000u);
    fm_close_popovers(d, NULL);
    if (open)
        fm_set_open(d, open, true);
    d->focus = NULL; /* no caret blinking in a picture */
    settle(d, o->screen);
    fm_render_layer(d, c, 0);
    if (window) {
        rect body;
        fm_render_window_frame(d, c, 0, sample_window(d, o), true, &body);
        canvas_clip(c, body);
        fm_host_draw_view("files", body);
        canvas_unclip(c);
    }
    fm_render_layer(d, c, 1);
    fm_render_layer(d, c, 2);
    fm_lint_offscreen = was;
}

/* ---- the tree, as drawn ---- */

static bool drawn_elem(const fm_node *k) {
    return k->kind == FM_ELEM && !is_popover(k) && !is_meta(k) && !k->st.hidden && k->laid_out && !tag_is(k, "tip");
}

static bool drawn_text(const fm_node *k) { return k->kind == FM_TEXT && in_flow(k) && k->laid_out; }

static bool is_leaf_tag(const fm_node *n) {
    static const char *const leaves[] = {"icon", "orb", "avatar", "clock", "canvas", "view", "spacer", "divider", "slot",
                                         "search", "input", "text", "label", "title", NULL};
    for (int i = 0; leaves[i]; i++)
        if (tag_is(n, leaves[i]))
            return true;
    return false;
}

static bool is_control(const fm_node *n) {
    return tag_is(n, "search") || tag_is(n, "input") || tag_is(n, "button") || tag_is(n, "item") ||
           tag_is(n, "text") || tag_is(n, "label") || tag_is(n, "title") || tag_is(n, "clock") || tag_is(n, "icon");
}

static const char *edge_name(int side) {
    static const char *const names[] = {"left", "top", "right", "bottom"};
    return names[side];
}

/* How far `in` reaches outside `out`, and on which side. */
static int overhang(rect in, rect out, int *side) {
    int o[4] = {out.x - in.x, out.y - in.y, in.x + in.w - (out.x + out.w), in.y + in.h - (out.y + out.h)};
    int best = 0;
    *side = 0;
    for (int i = 0; i < 4; i++)
        if (o[i] > best)
            best = o[i], *side = i;
    return best;
}

static void mark_action(const char *a) {
    if (!a)
        return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", a);
    for (char *save, *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        while (*t == ' ')
            t++;
        int k = -1;
        if (!strncasecmp(t, "open:", 5) || !strncasecmp(t, "launch:", 7))
            k = fm_app_index(strchr(t, ':') + 1);
        else if (!strncasecmp(t, "redesign", 8))
            k = fm_app_index("redesign");
        else if (!strncasecmp(t, "power", 5))
            k = fm_app_index("power");
        else if (!strncasecmp(t, "toggle:#", 8) || !strncasecmp(t, "show:#", 6)) {
            const char *id = strchr(t, '#') + 1;
            if (L.nopened < 32)
                snprintf(L.opened[L.nopened++], sizeof L.opened[0], "%.*s", (int)strcspn(id, " ;"), id);
        }
        if (k >= 0 && k < 64)
            L.reach[k] = true;
    }
}

static void mark_reach(fm_node *n) {
    if (tag_is(n, "search"))
        L.reach_all = true;
    if (tag_is(n, "orb")) {
        int k = fm_app_index("companion");
        if (k >= 0 && k < 64)
            L.reach[k] = true;
    }
    if (tag_is(n, "view")) {
        const char *a = fm_attr_get(n, "app");
        int k = fm_app_index(a ? a : "companion");
        if (k >= 0 && k < 64)
            L.reach[k] = true;
    }
    mark_action(fmi_action_of(n));
}

static void check_text(fm_node *t) {
    fm_node *p = t->parent;
    if (!p || tag_is(p, "canvas"))
        return;
    for (fm_node *a = p; a; a = a->parent)
        if (tag_is(a, "windowlist")) /* window titles are cut to fit by design */
            return;
    const fm_style *st = &p->st;
    char buf[512];
    fmi_text_for_draw(st, t->text, buf, sizeof buf);
    if (!buf[strspn(buf, " \t\r\n")])
        return;
    char path[160], snip[40];
    path_of(t, path, sizeof path);
    snprintf(snip, sizeof snip, "%.22s%s", buf, strlen(buf) > 22 ? "..." : "");
    for (char *q = snip; *q; q++)
        if (*q == '\n' || *q == '"')
            *q = ' ';
    float s = t->vscale > 0 ? t->vscale : 1;
    if (st->font_size * s < 10.99f)
        add(K_SMALLTEXT, 2, path, "%s: text is %.0fpx; nothing should be under 12px (%s)", path, st->font_size * s,
            "raise font-size");
    font *f = fm_host_font(st->font, st->font_size * s);
    int tw = text_width(f, buf), lh = (int)(font_line_height(f) * st->line_height);
    rect r = t->vis;
    if (tw > r.w + 2 && r.w > 0) {
        bool in_item = p->generated || (p->parent && p->parent->generated);
        if (r.h >= lh * 2 - 2) {
            int need = draw_text_wrapped(NULL, f, (rect){0, 0, r.w, 10000}, buf, 0, st->line_height);
            if (need > r.h + 4)
                add(K_TRUNC, 3, path, "%s: \"%s\" needs %dpx of height but gets %dpx, so lines are cut; give it more "
                    "room or fewer words", path, snip, need, r.h);
        } else {
            add(K_TRUNC, 3, path, "%s: \"%s\" is cut short (needs %dpx, gets %dpx); %s", path, snip, tw, r.w,
                in_item ? "widen the items (width or min-width), use labels=\"hover\", or shorter <apps> labels"
                        : "give it room (width, min-width, less padding) or use a shorter label");
        }
    }
}

static void check_siblings(fm_node *n) {
    if (n->st.mode == FM_STACK)
        return;
    fm_node *k[64];
    int nk = 0;
    for (fm_node *c = n->first; c && nk < 64; c = c->next)
        if (drawn_elem(c) && !c->st.absolute && c->vis.w > 2 && c->vis.h > 2)
            k[nk++] = c;
    for (int i = 0; i < nk; i++)
        for (int j = i + 1; j < nk; j++) {
            rect a = k[i]->vis, b = k[j]->vis;
            int iw = (a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w) - (a.x > b.x ? a.x : b.x);
            int ih = (a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h) - (a.y > b.y ? a.y : b.y);
            if (iw <= 4 || ih <= 4)
                continue;
            char pa[160], pb[160], key[160];
            path_of(k[i], pa, sizeof pa);
            path_of(k[j], pb, sizeof pb);
            path_of(n, key, sizeof key);
            if (n->st.mode == FM_ORBIT)
                add(K_OVERLAP, 4, key, "%s: the items around the circle sit on top of each other; raise orbit or "
                    "make the items smaller", key);
            else
                add(K_OVERLAP, 4, key, "%s and %s overlap by %dx%dpx inside %s; check margins and fixed sizes", pa,
                    pb, iw, ih, key);
            return;
        }
}

static void check_elem(fm_node *n, fm_node *parent, bool *off) {
    rect v = n->vis, S = L.o->screen;
    char path[160];
    path_of(n, path, sizeof path);
    int side;
    if (!*off && v.w > 0 && v.h > 0) {
        int out = overhang(v, S, &side);
        if (out > 2) {
            add(K_OFFSCREEN, 5, path, "%s sticks %dpx off the %s edge of the screen; move it in (place, margin, "
                "or a smaller size)", path, out, edge_name(side));
            *off = true;
        }
    }
    bool thin = v.w <= 2 || v.h <= 2;
    bool has_kids = false;
    for (fm_node *c = n->first; c; c = c->next)
        if (drawn_elem(c) || drawn_text(c))
            has_kids = true;
    /* a hairline the design asked for (height:2) is a rule, not a collapse */
    bool asked = (v.h <= 2 && n->st.height.unit == FM_PX) || (v.w <= 2 && n->st.width.unit == FM_PX);
    if (thin && !tag_is(n, "spacer") && !tag_is(n, "divider") && (has_kids || (n->st.bg.kind != FM_BG_NONE && !asked)))
        add(K_DEGEN, 3, path, "%s is squeezed to %dx%dpx and can't show anything; give it a size or let it grow",
            path, v.w, v.h);
    if (parent && parent->kind == FM_ELEM && !tag_is(parent, "desktop") && !n->st.absolute &&
        parent->st.overflow != FM_OV_SCROLL && parent->vis.w > 0 && !thin) {
        int out = overhang(v, parent->vis, &side);
        if (out > 3) {
            char pp[160];
            path_of(parent, pp, sizeof pp);
            bool fixed = parent->st.height.unit == FM_PX || parent->st.width.unit == FM_PX;
            if (parent->st.overflow == FM_OV_CLIP)
                add(K_CLIP, 4, path, "%s is cut off at the %s by %s (%dpx don't fit); %s", path, edge_name(side), pp,
                    out, fixed ? "remove or raise the fixed size, or shrink what's inside" : "make room for it");
            else
                add(K_CLIP, 4, path, "%s spills %dpx out of the %s of %s; %s", path, out, edge_name(side), pp,
                    fixed ? "remove or raise its fixed size, or shrink what's inside" : "make room for it");
        }
    }
    if (parent && parent->st.mode == FM_FLEX && parent->st.dir == FM_COLUMN && n->st.grow > 0 && is_control(n) &&
        n->box.h > n->mh * 2 + 24)
        add(K_STRETCH, 3, path, "%s grows to %dpx tall in a column; set grow:0 on it", path, n->box.h);
    if (parent && parent->st.mode == FM_FLEX && parent->st.dir == FM_ROW && n->st.grow > 0 &&
        (tag_is(n, "button") || tag_is(n, "icon")) && n->box.w > n->mw * 3 + 80)
        add(K_STRETCH, 2, path, "%s stretches to %dpx wide in a row; set grow:0 on it", path, n->box.w);
    const char *act = fmi_action_of(n);
    if ((act || tag_is(n, "search") || tag_is(n, "input")) && !thin && (v.w < 24 || v.h < 24)) {
        char key[180];
        snprintf(key, sizeof key, "%s", path);
        add(K_TARGET, 2, key, "%s is a %dx%dpx click target; make it at least 24x24 (padding or size)", path, v.w,
            v.h);
    }
    if (!has_kids && !is_leaf_tag(n) && !act) {
        bool surface = parent && (tag_is(parent, "desktop") || is_popover(n));
        if (tag_is(n, "applist"))
            add(K_EMPTY, 4, path, "%s shows no apps; check the ids in apps=\"...\"", path);
        else if (tag_is(n, "windowlist") || tag_is(n, "item"))
            ; /* fills up when windows open */
        else if (surface || (n->st.bg.kind != FM_BG_NONE && v.w >= 120 && v.h >= 80))
            add(K_EMPTY, 2, path, "%s is an empty %dx%dpx panel; give it content or remove it", path, v.w, v.h);
    }
}

static void walk(fm_node *n, fm_node *parent, bool off, bool reach) {
    if (n->kind == FM_TEXT) {
        if (drawn_text(n)) {
            check_text(n);
            if (parent && !off && parent->vis.w > 0 && n->vis.w > 2 && !tag_is(parent, "canvas")) {
                int side, out = overhang(n->vis, parent->vis, &side);
                if (out > 3 && parent->st.overflow != FM_OV_SCROLL) {
                    char pp[160];
                    path_of(parent, pp, sizeof pp);
                    add(K_CLIP, 3, pp, "%s: its text doesn't fit at the %s (%dpx over); give it more room or a "
                        "smaller font", pp, edge_name(side), out);
                }
            }
        }
        return;
    }
    if (!drawn_elem(n) && !(is_popover(n) && n->open))
        return;
    bool was_off = off;
    check_elem(n, parent, &off);
    rect S = L.o->screen;
    bool on_screen = n->vis.w > 0 && n->vis.h > 0 && n->vis.x < S.x + S.w && n->vis.y < S.y + S.h &&
                     n->vis.x + n->vis.w > S.x && n->vis.y + n->vis.h > S.y;
    if (reach && on_screen)
        mark_reach(n);
    check_siblings(n);
    for (fm_node *c = n->first; c; c = c->next)
        walk(c, n, off || was_off, reach);
}

static bool toggled(const char *id) {
    for (int i = 0; i < L.nopened; i++)
        if (!strcmp(L.opened[i], id))
            return true;
    return false;
}

static void check_surface_overlaps(fm_doc *d) {
    fm_node *s[64];
    int ns = 0;
    rect S = L.o->screen;
    for (fm_node *k = d->desktop->first; k && ns < 64; k = k->next) {
        if (!drawn_elem(k) || k->vis.w <= 2 || k->vis.h <= 2)
            continue;
        const char *pl = fmi_place(k, "place");
        bool fill = pl && (!strcasecmp(pl, "fill") || !strcasecmp(pl, "full"));
        if (fill || (long)k->vis.w * k->vis.h > (long)S.w * S.h * 7 / 10) /* a backdrop layer: meant to be under */
            continue;
        s[ns++] = k;
    }
    for (int i = 0; i < ns; i++)
        for (int j = i + 1; j < ns; j++) {
            rect a = s[i]->vis, b = s[j]->vis;
            int iw = (a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w) - (a.x > b.x ? a.x : b.x);
            int ih = (a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h) - (a.y > b.y ? a.y : b.y);
            if (iw <= 4 || ih <= 4)
                continue;
            long small = (long)a.w * a.h < (long)b.w * b.h ? (long)a.w * a.h : (long)b.w * b.h;
            if ((long)iw * ih * 10 < small && (long)iw * ih < 1500)
                continue;
            char pa[160], pb[160], key[330];
            path_of(s[i], pa, sizeof pa);
            path_of(s[j], pb, sizeof pb);
            snprintf(key, sizeof key, "%s+%s", pa, pb);
            add(K_OVERLAP, 4, key, "%s and %s overlap by %dx%dpx on the desktop; move one (place, dock, margin) or "
                "make it smaller", pa, pb, iw, ih);
        }
}

static void check_popover(fm_doc *d, fm_node *p) {
    rect S = L.o->screen;
    char path[160];
    path_of(p, path, sizeof path);
    if (!fm_attr_get(p, "hotkey") && !toggled(p->id))
        add(K_ORPHAN, 3, path, "%s has nothing visible that opens it; add a button with what=\"toggle:#%s\"",
            path, p->id);
    if (tag_is(p, "sheet"))
        return;
    fmi_measure(d, p, (float)S.w, (float)S.h);
    const fm_style *s = &p->st;
    float w = s->width.unit == FM_PX ? s->width.v : s->width.unit == FM_PCT ? S.w * s->width.v / 100
              : s->width.unit == FM_FULL ? (float)S.w : p->mw;
    float h = s->height.unit == FM_PX ? s->height.v : s->height.unit == FM_PCT ? S.h * s->height.v / 100
              : s->height.unit == FM_FULL ? (float)S.h : p->mh;
    if (h > S.h - 16 || w > S.w - 16)
        add(K_BIGPOP, 5, path, "%s wants %.0fx%.0fpx but the screen is %dx%d, so it gets squeezed; use fewer or "
            "smaller items, more grid columns, or overflow:scroll", path, w, h, S.w, S.h);
}

static void check_window(fm_doc *d) {
    fm_node *root = fmi_tpl_root(d);
    if (!root)
        return;
    bool slot = false, close = false;
    fm_node *stack[256];
    int sp = 0;
    stack[sp++] = root;
    while (sp) {
        fm_node *n = stack[--sp];
        if (n->kind != FM_ELEM)
            continue;
        if (tag_is(n, "slot"))
            slot = true;
        const char *a = fmi_action_of(n);
        if (a && strcasestr(a, "window:close"))
            close = true;
        for (fm_node *c = n->first; c && sp < 256; c = c->next)
            stack[sp++] = c;
    }
    if (!slot)
        add(K_WINDOW, 5, "slot", "the window template has no <slot/>, so apps have nowhere to draw");
    if (!close)
        add(K_WINDOW, 4, "close", "windows can't be closed: add a button with what=\"window:close\" to the template");
    walk(root, NULL, false, false);
    fm_node *sl = NULL;
    stack[0] = root;
    sp = 1;
    while (sp && !sl) {
        fm_node *n = stack[--sp];
        if (tag_is(n, "slot"))
            sl = n;
        for (fm_node *c = n->first; c && sp < 256; c = c->next)
            if (c->kind == FM_ELEM)
                stack[sp++] = c;
    }
    if (sl && sl->laid_out && (sl->vis.h < 120 || sl->vis.w < 200))
        add(K_WINDOW, 4, "slotsize", "the window's <slot/> is only %dx%dpx in a 780x500 window; let it grow:1 and "
            "keep the frame's own padding small", sl->vis.w, sl->vis.h);
}

static void popovers_of(fm_node *n, fm_node **out, int *k, int max) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || is_meta(n))
            continue;
        if (is_popover(n) && !tag_is(n, "tooltip") && !n->st.hidden && *k < max)
            out[(*k)++] = n;
        popovers_of(n->first, out, k, max);
    }
}

fm_node *fm_lint_main_popover(fm_doc *d) {
    if (!d->desktop)
        return NULL;
    fm_node *pops[32];
    int np = 0;
    fm_style_tree(d);
    popovers_of(d->desktop->first, pops, &np, 32);
    if (!np)
        return NULL;
    fm_node *hk = fm_hotkey(d, "meta");
    if (hk && is_popover(hk))
        return hk;
    if (hk) { /* a button on the key: the popover it toggles */
        const char *a = fmi_action_of(hk);
        const char *h = a ? strchr(a, '#') : NULL;
        if (h)
            for (int i = 0; i < np; i++)
                if (!strncmp(pops[i]->id, h + 1, strlen(pops[i]->id)) &&
                    !isalnum((unsigned char)h[1 + strlen(pops[i]->id)]))
                    return pops[i];
    }
    for (int i = 0; i < np; i++)
        if (tag_is(pops[i], "popover"))
            return pops[i];
    return pops[0];
}

static int by_weight(const void *a, const void *b) {
    const struct finding *x = a, *y = b;
    if (x->weight != y->weight)
        return y->weight - x->weight;
    return x->kind - y->kind;
}

void fm_lint_run(fm_doc *d, const fm_lint_opts *o, fm_lint *out) {
    memset(out, 0, sizeof *out);
    memset(&L, 0, sizeof L);
    L.o = o;
    L.d = d;
    if (!d->desktop) {
        snprintf(out->text, sizeof out->text, "- the document has no <desktop>\n");
        out->count = 1;
        out->score = 10;
        return;
    }
    void (*was)(canvas *, fm_node *, rect, color, float) = fm_text_probe;
    fm_text_probe = probe;

    /* everything closed, with a window open over the desktop */
    fm_lint_paint(d, o, NULL, true);
    for (fm_node *k = d->desktop->first; k; k = k->next)
        walk(k, d->desktop, false, true);
    check_surface_overlaps(d);
    check_window(d);

    /* each popover open in turn (those inside others after them) */
    fm_node *pops[32];
    int np = 0;
    popovers_of(d->desktop->first, pops, &np, 32);
    for (int i = 0; i < np; i++) {
        fm_lint_paint(d, o, pops[i], false);
        if (!pops[i]->laid_out)
            continue;
        bool reachable = fm_attr_get(pops[i], "hotkey") || toggled(pops[i]->id);
        walk(pops[i], NULL, false, reachable);
        check_popover(d, pops[i]);
    }
    fm_close_popovers(d, NULL);
    fm_text_probe = was;

    /* every app can be opened by a click (or by name, from a search) */
    if (!L.reach_all) {
        char miss[300] = "";
        int n = o->core_apps < fm_host_app_count() ? o->core_apps : fm_host_app_count();
        for (int i = 0; i < n && i < 64; i++)
            if (!L.reach[i])
                snprintf(miss + strlen(miss), sizeof miss - strlen(miss), "%s%s", miss[0] ? ", " : "",
                         fm_host_app_id(i));
        if (miss[0])
            add(K_UNREACH, 6, "apps", "nothing visible opens %s; add them to an applist, a button with "
                "what=\"open:id\", or a search field", miss);
    }

    qsort(L.f, (size_t)L.n, sizeof L.f[0], by_weight);
    size_t len = 0;
    int shown = 0;
    for (int i = 0; i < L.n; i++) {
        out->score += L.f[i].weight + (L.f[i].count > 1 ? 1 : 0);
        if (shown >= 24)
            continue;
        char more[32] = "";
        if (L.f[i].count > 1)
            snprintf(more, sizeof more, " (%d places)", L.f[i].count);
        int w = snprintf(out->text + len, sizeof out->text - len, "- %s%s\n", L.f[i].msg, more);
        if (w < 0 || (size_t)w >= sizeof out->text - len)
            break;
        len += (size_t)w;
        shown++;
    }
    if (shown < L.n && len + 64 < sizeof out->text)
        snprintf(out->text + len, sizeof out->text - len, "- and %d smaller things\n", L.n - shown);
    out->count = L.n;
    L.o = NULL;
}
