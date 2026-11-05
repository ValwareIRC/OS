#define _GNU_SOURCE
#include "fmi.h"
#include "../textedit.h"
#include "../../lib/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Input, actions, and the companion's view of and control over the UI. */

/* ---- hit testing ---- */

static fm_node *hit_in(fm_node *n, int x, int y) {
    if (n->kind != FM_ELEM || n->st.hidden || !n->laid_out || is_meta(n))
        return NULL;
    /* children first, last drawn on top */
    fm_node *kids[256];
    int nk = 0;
    for (fm_node *k = n->first; k && nk < 256; k = k->next)
        if (k->kind == FM_ELEM && !is_popover(k) && !tag_is(k, "tip"))
            kids[nk++] = k;
    if (n->st.overflow == FM_OV_VISIBLE || pt_in(n->vis, x, y))
        for (int i = nk - 1; i >= 0; i--) {
            fm_node *h = hit_in(kids[i], x, y);
            if (h)
                return h;
        }
    if (!pt_in(n->vis, x, y))
        return NULL;
    if (tag_is(n, "desktop"))
        return NULL;
    /* plain layout boxes and text with nothing to click let clicks through */
    bool solid = n->st.bg.kind != FM_BG_NONE || n->st.border_w > 0 || n->st.blur > 0 || fmi_action_of(n) || tag_is(n, "model") ||
                 is_popover(n) || tag_is(n, "icon") || tag_is(n, "orb") || tag_is(n, "avatar") || tag_is(n, "view") ||
                 tag_is(n, "search") || tag_is(n, "input") || tag_is(n, "item") || tag_is(n, "button") || tag_is(n, "canvas") ||
                 fm_attr_get(n, "drag");
    if (!solid)
        return NULL;
    return n;
}

/* Small controls are easy to miss: one smaller than 24x24 can also be clicked a little around it (up to 24x24,
 * centred), when the click lands on nothing that does something itself. So a design can keep 14px title-bar
 * buttons and still be comfortable to use. The nearest one wins. */
#define SLOP 24
static bool clickable(const fm_node *n) {
    return fmi_action_of(n) || tag_is(n, "button") || tag_is(n, "item") || tag_is(n, "input") || tag_is(n, "search");
}

static void slop_in(fm_node *n, int x, int y, fm_node **best, float *best_d) {
    if (n->kind != FM_ELEM || n->st.hidden || !n->laid_out || is_meta(n) || is_popover(n))
        return;
    rect v = n->vis;
    if (clickable(n) && (v.w < SLOP || v.h < SLOP) && v.w > 0 && v.h > 0) {
        int gw = v.w < SLOP ? (SLOP - v.w + 1) / 2 : 0, gh = v.h < SLOP ? (SLOP - v.h + 1) / 2 : 0;
        rect g = {v.x - gw, v.y - gh, v.w + 2 * gw, v.h + 2 * gh};
        if (pt_in(g, x, y)) {
            float dx = x - (v.x + v.w / 2.0f), dy = y - (v.y + v.h / 2.0f), d2 = dx * dx + dy * dy;
            if (d2 < *best_d)
                *best = n, *best_d = d2;
        }
    }
    for (fm_node *k = n->first; k; k = k->next)
        if (k->kind == FM_ELEM && !tag_is(k, "tip"))
            slop_in(k, x, y, best, best_d);
}

static fm_node *with_slop(fm_node *top, fm_node *h, int x, int y) {
    if (h && clickable(h))
        return h;
    fm_node *best = NULL;
    float d = 1e9f;
    slop_in(top, x, y, &best, &d);
    return best ? best : h;
}

static fm_node *hit_popovers(fm_node *n, int x, int y) {
    fm_node *found = NULL;
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || is_meta(n))
            continue;
        if (is_popover(n) && n->open && n->laid_out) {
            fm_node *h = hit_in(n, x, y);
            if (h || pt_in(n->vis, x, y))
                h = with_slop(n, h, x, y);
            if (h)
                found = h; /* later ones are on top */
        }
        fm_node *h = hit_popovers(n->first, x, y);
        if (h)
            found = h;
    }
    return found;
}

static fm_node *hit_layer(fm_doc *d, int layer, int x, int y) {
    if (!d->desktop)
        return NULL;
    if (layer == 2)
        return hit_popovers(d->desktop->first, x, y);
    fm_node *found = NULL;
    float best_z = -1e9f;
    for (fm_node *k = d->desktop->first; k; k = k->next) {
        if (k->kind != FM_ELEM || is_meta(k) || is_popover(k) || fm_layer_of(k) != layer)
            continue;
        fm_node *h = hit_in(k, x, y);
        if (h || pt_in(k->vis, x, y))
            h = with_slop(k, h, x, y);
        if (h && k->st.z >= best_z) {
            found = h;
            best_z = k->st.z;
        }
    }
    return found;
}

bool fm_hit_layer(fm_doc *d, int layer, int x, int y) { return hit_layer(d, layer, x, y) != NULL; }

/* ---- popovers ---- */

static bool is_ancestor(const fm_node *a, const fm_node *n) {
    for (; n; n = n->parent)
        if (n == a)
            return true;
    return false;
}

static void close_rec(fm_doc *d, fm_node *n, fm_node *except) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || is_meta(n))
            continue;
        if (is_popover(n) && n->open && !(except && is_ancestor(n, except))) {
            n->open = false;
            fm_node *t = fmi_toggler_of(d, n);
            if (t && !is_popover(t))
                t->open = false;
        }
        close_rec(d, n->first, except);
    }
}

void fm_close_popovers(fm_doc *d, fm_node *except) {
    if (d->desktop)
        close_rec(d, d->desktop->first, except);
}

static bool any_open(fm_node *n) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        if (is_popover(n) && n->open)
            return true;
        if (any_open(n->first))
            return true;
    }
    return false;
}

void fm_set_open(fm_doc *d, fm_node *n, bool open) {
    if (!n)
        return;
    if (open && is_popover(n))
        fm_close_popovers(d, n);
    n->open = open;
    if (is_popover(n)) {
        fm_node *t = fmi_toggler_of(d, n);
        if (t && !is_popover(t))
            t->open = open;
        if (open) { /* focus the first field inside, however deep (a Start menu's search sits in a column) */
            fm_node *stack[256];
            int sp = 0;
            stack[sp++] = n;
            while (sp) {
                fm_node *k = stack[--sp];
                if (k != n && (tag_is(k, "search") || tag_is(k, "input"))) {
                    d->focus = k;
                    break;
                }
                fm_node *kids[64];
                int nk = 0;
                for (fm_node *c = k->first; c && nk < 64; c = c->next)
                    kids[nk++] = c;
                while (nk && sp < 256) /* pushed in reverse so the first child comes out first */
                    stack[sp++] = kids[--nk];
            }
        } else if (d->focus && is_ancestor(n, d->focus)) {
            d->focus = NULL;
        }
    }
}

fm_node *fm_hotkey(fm_doc *d, const char *key) {
    fm_node *stack[512];
    int sp = 0;
    if (d->desktop)
        stack[sp++] = d->desktop;
    while (sp) {
        fm_node *n = stack[--sp];
        for (; n; n = n->next) {
            if (n->kind != FM_ELEM || is_meta(n))
                continue;
            const char *h = fm_attr_get(n, "hotkey");
            if (h && !strcasecmp(h, key))
                return n;
            if (n->first && sp < 512)
                stack[sp++] = n->first;
        }
    }
    return NULL;
}

void fm_flash(fm_node *n) {
    if (n)
        n->flash = 1;
}

/* ---- actions ---- */

static void run_one(fm_doc *d, fm_node *src, const char *a) {
    while (*a == ' ')
        a++;
    if (!*a)
        return;
    /* verb:#target or verb #target */
    static const char *const verbs[] = {"toggle", "open", "show", "close", "hide", "focus"};
    for (size_t i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        size_t l = strlen(verbs[i]);
        if (!strncasecmp(a, verbs[i], l) && (a[l] == ':' || a[l] == ' ')) {
            const char *t = a + l + 1;
            while (*t == ' ')
                t++;
            if (*t != '#' && *t != '.')
                break; /* "open:files" is an app: the host handles it */
            fm_node *n = fm_find(d, t);
            if (!n)
                return;
            if (i == 5) {
                d->focus = n;
                return;
            }
            bool want = i == 0 ? !(is_popover(n) ? n->open : n->st.hidden == false && !fm_attr_get(n, "hidden")) : i <= 2;
            if (is_popover(n)) {
                fm_set_open(d, n, i == 0 ? !n->open : want);
            } else {
                bool hidden_now = n->st.hidden;
                bool show = i == 0 ? hidden_now : want;
                fm_attr_set(n, "hidden", show ? "false" : "true");
            }
            return;
        }
    }
    if (!strncasecmp(a, "close-popovers", 14) || !strncasecmp(a, "dismiss", 7)) {
        fm_close_popovers(d, NULL);
        return;
    }
    /* a bare app id means "open it" */
    char buf[160];
    if (!strchr(a, ':') && fm_app_index(a) >= 0) {
        snprintf(buf, sizeof buf, "open:%s", a);
        a = buf;
    }
    fm_host_action(d, src, a);
}

void fm_activate(fm_doc *d, fm_node *n) {
    if (!n)
        return;
    if (tag_is(n, "search") || tag_is(n, "input")) {
        d->focus = n;
        return;
    }
    const char *act = fmi_action_of(n);
    if (!act)
        return;
    fmi_run_actions(d, n, act);
}

void fmi_run_actions(fm_doc *d, fm_node *n, const char *act) {
    char copy[400];
    snprintf(copy, sizeof copy, "%s", act);
    bool keeps = false;
    for (char *save, *t = strtok_r(copy, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        while (*t == ' ')
            t++;
        if (!strncasecmp(t, "toggle", 6) || !strncasecmp(t, "show", 4) || !strncasecmp(t, "open:#", 6) ||
            !strncasecmp(t, "open #", 6) || !strncasecmp(t, "focus", 5))
            keeps = true;
        run_one(d, n, t);
    }
    /* choosing something in a menu closes the menu */
    if (!keeps && !fm_attr_get(n, "keep-open")) {
        for (fm_node *p = n->parent; p; p = p->parent)
            if (is_popover(p)) {
                fm_close_popovers(d, NULL);
                break;
            }
    }
}

static fm_node *actionable(fm_node *n) {
    for (; n; n = n->parent) {
        if (n->kind != FM_ELEM)
            continue;
        if (fmi_action_of(n) || tag_is(n, "search") || tag_is(n, "input") || fm_attr_get(n, "drag"))
            return n;
        if (is_popover(n) || tag_is(n, "desktop"))
            return NULL;
    }
    return NULL;
}

static void clear_hover(fm_node *n) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        n->hover = false;
        clear_hover(n->first);
    }
}

static void type_into(fm_doc *d, fm_node *f, int key, int mod) {
    const char *v = fm_attr_get(f, "value");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", v ? v : "");
    if (key == K_ENTER) {
        if (!buf[0])
            return;
        const char *act = fmi_action_of(f);
        char a[1100];
        if (act)
            snprintf(a, sizeof a, "%s:%s", act, buf);
        else
            snprintf(a, sizeof a, "search:%s", buf);
        fm_attr_set(f, "value", "");
        fm_host_action(d, f, a);
        fm_close_popovers(d, NULL);
        d->focus = NULL;
        return;
    }
    const char *type = fm_attr_get(f, "type");
    bool changed = false;
    te_key(fmi_editor(f), buf, sizeof buf, key, (mod & KM_CTRL) != 0, (mod & KM_SHIFT) != 0, type && !strcasecmp(type, "password"),
           fmi_now_ms(), &changed);
    if (changed)
        fm_attr_set(f, "value", buf);
}

const char *fm_cursor(fm_doc *d, int x, int y) {
    if (!d || !d->desktop)
        return "";
    fm_node *hit = hit_layer(d, 2, x, y);
    if (!hit)
        hit = hit_layer(d, 1, x, y);
    if (!hit)
        hit = hit_layer(d, 0, x, y);
    for (fm_node *p = hit; p; p = p->parent)
        if (tag_is(p, "model")) {
            bool click;
            fmi_model_hover_title(p, &click);
            if (click)
                return "pointer";
            break;
        }
    const char *c = hit ? hit->st.cursor : d->desktop->st.cursor;
    if (!c[0] && hit && fmi_action_of(hit))
        return d->desktop->st.cursor[0] ? "pointer" : ""; /* a design with pointers of its own: a hand on links */
    return c[0] ? c : d->desktop->st.cursor;
}

bool fm_input(fm_doc *d, int layer_mask) {
    const input_state *in = d->in;
    if (!in || !d->desktop)
        return false;
    bool used_key = false;
    for (int i = 0; i < in->nkeys; i++) {
        int k = in->keys[i];
        if (k == K_ESC) {
            fm_close_popovers(d, NULL);
            d->focus = NULL;
            used_key = true;
        } else if (k == K_META) {
            fm_node *h = fm_hotkey(d, "meta");
            if (h) {
                if (is_popover(h))
                    fm_set_open(d, h, !h->open);
                else
                    fm_activate(d, h);
                used_key = true;
            }
        } else if (d->focus) {
            type_into(d, d->focus, k, in->keymod[i]);
            used_key = true;
        }
    }
    (void)used_key;

    fm_node *hit = hit_layer(d, 2, in->mx, in->my);
    if (!hit && (layer_mask & 2))
        hit = hit_layer(d, 1, in->mx, in->my);
    if (!hit && (layer_mask & 1))
        hit = hit_layer(d, 0, in->mx, in->my);
    clear_hover(d->desktop);
    for (fm_node *p = hit; p; p = p->parent)
        p->hover = true;
    d->hovered = hit;

    if (in->pressed) {
        fm_node *t = actionable(hit);
        if (d->pressed)
            d->pressed->active = false;
        d->pressed = t;
        if (t)
            t->active = true;
        bool in_menu = false; /* (a right-click menu's pick goes to the field it was opened on: it keeps it) */
        for (const fm_node *p = hit; p && !in_menu; p = p->parent)
            in_menu = tag_is(p, "menu") && !strcmp(p->id, "_menu");
        if (!in_menu && (!t || !(tag_is(t, "search") || tag_is(t, "input"))))
            if (!(hit && d->focus && is_ancestor(d->focus, hit)))
                d->focus = NULL;
        if (t && (tag_is(t, "search") || tag_is(t, "input"))) {
            if (d->focus != t)
                te_reset(fmi_editor(t), fm_attr_get(t, "value") ? fm_attr_get(t, "value") : "");
            d->focus = t;
        }
        /* clicking outside an open popover closes it, unless it's on the
         * button that toggles it (that click toggles it instead) */
        if (any_open(d->desktop->first)) {
            fm_node *pop = NULL;
            for (fm_node *p = hit; p; p = p->parent)
                if (is_popover(p))
                    pop = p;
            if (!pop) {
                const char *a = t ? fmi_action_of(t) : NULL;
                if (!a || !strchr(a, '#'))
                    fm_close_popovers(d, NULL);
            }
        }
    }
    if (d->focus && (in->pressed || in->down)) { /* the caret where they click, a selection where they drag */
        font *ef;
        rect box;
        int tx, ty;
        const char *v = fm_attr_get(d->focus, "value"), *type = fm_attr_get(d->focus, "type");
        if (fmi_input_geom(d->focus, &ef, &box, &tx, &ty))
            te_mouse(fmi_editor(d->focus), v ? v : "", ef, d->focus->vis, tx, in,
                     type && !strcasecmp(type, "password"), fmi_now_ms());
    }
    for (fm_node *p = hit; p; p = p->parent)
        if (tag_is(p, "model")) {
            if (fmi_model_pointer(d, p, in))
                ((input_state *)in)->wheel = 0; /* the model turned with it: nothing under it scrolls */
            break;
        }
    if (in->clicked && hit && tag_is(hit, "canvas") && !fmi_action_of(hit))
        fmi_canvas_click(d, hit, in->mx, in->my);
    if (in->clicked && d->pressed) {
        fm_node *p = d->pressed;
        if (pt_in(p->vis, in->mx, in->my) && !fm_attr_get(p, "drag"))
            fm_activate(d, p);
    }
    /* Let go: only a press on the desktop is the desktop's to forget. A
     * window frame's button (minimise, close...) is pressed in its own pass,
     * which runs after this one and needs the press to turn the release
     * into a click. */
    if (!in->down && d->pressed && is_ancestor(d->desktop, d->pressed)) {
        d->pressed->active = false;
        d->pressed = NULL;
    }
    if (in->wheel && hit) {
        for (fm_node *p = hit; p; p = p->parent)
            if (p->st.overflow == FM_OV_SCROLL) {
                p->scroll -= in->wheel * 48;
                break;
            }
    }
    return hit != NULL;
}

/* ---- window frames ---- */

bool fm_window_input(fm_doc *d, int win, rect r, bool focused) {
    const input_state *in = d->in;
    fm_node *root = fmi_frame_layout(d, win, r, focused, NULL);
    if (!root || !in) {
        d->cur_win = -1;
        return false;
    }
    fm_node *hit = hit_in(root, in->mx, in->my);
    if (pt_in(root->vis, in->mx, in->my))
        hit = with_slop(root, hit, in->mx, in->my); /* 14px title-bar buttons, clickable around them */
    d->hover_win = pt_in(root->vis, in->mx, in->my) ? win : -1;
    clear_hover(root);
    for (fm_node *p = hit; p && p != root->parent; p = p->parent)
        p->hover = true;
    if (in->pressed && hit) {
        fm_node *t = actionable(hit);
        if (t && fm_attr_get(t, "drag") && !fmi_action_of(t))
            fm_host_action(d, t, "window:drag");
        else if (t) {
            d->pressed = t;
            t->active = true;
        }
    }
    if (in->clicked && d->pressed && is_ancestor(root, d->pressed)) {
        if (pt_in(d->pressed->vis, in->mx, in->my))
            fm_activate(d, d->pressed);
        d->pressed->active = false;
        d->pressed = NULL;
    }
    if (!in->down && d->pressed && is_ancestor(root, d->pressed)) { /* let go off the button */
        d->pressed->active = false;
        d->pressed = NULL;
    }
    d->cur_win = -1;
    return pt_in(root->vis, in->mx, in->my);
}

/* ---- the companion's view ---- */

static void describe_rec(fm_doc *d, sbuf *b, fm_node *n, int depth) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || is_meta(n) || n->generated)
            continue;
        if (b->len > 7000) {
            sb_add(b, "...\n");
            return;
        }
        for (int i = 0; i < depth; i++)
            sb_add(b, "  ");
        sb_add(b, n->tag);
        if (n->id[0] != '_')
            sb_printf(b, "#%s", n->id);
        const char *cls = fm_attr_get(n, "class");
        if (cls)
            for (const char *p = cls; *p;) {
                while (*p == ' ')
                    p++;
                const char *e = p;
                while (*e && *e != ' ')
                    e++;
                if (e > p)
                    sb_printf(b, ".%.*s", (int)(e - p), p);
                p = e;
            }
        static const char *const keys[] = {"what", "action", "dock", "place", "icon", "app", "apps", "bind",
                                           "anchor", "placement", "hotkey", "format", "face", "labels", "layer"};
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            const char *v = fm_attr_get(n, keys[i]);
            if (v)
                sb_printf(b, " %s=\"%s\"", keys[i], v);
        }
        char *t = NULL;
        if ((tag_is(n, "image") || tag_is(n, "svg") || tag_is(n, "pixels")) && n->laid_out) {
            float iw, ih;
            if (!fmi_media_size(n, &iw, &ih))
                sb_add(b, tag_is(n, "svg") ? " [the drawing can't be read]"
                                           : " [can't be shown: no such picture here, or not a PNG, JPEG, GIF, BMP or SVG]");
        }
        for (fm_node *c = tag_is(n, "canvas") || tag_is(n, "svg") || tag_is(n, "pixels") ? NULL : n->first; c;
             c = c->next)
            if (c->kind == FM_TEXT && !c->generated && !is_blank_text(c)) {
                t = fm_text_of(n);
                break;
            }
        if (t) {
            sb_printf(b, " \"%.60s\"", t);
            free(t);
            /* what is under its text: the nearest solid background, else the wallpaper there */
            if (n->laid_out && !n->st.hidden && n->box.w > 0) {
                color under = 0;
                bool solid = false;
                for (fm_node *a = n; a && !solid; a = a->parent)
                    if (a->kind == FM_ELEM && a->st.bg.kind == FM_BG_SOLID && C_A(a->st.bg.a) > 200)
                        under = a->st.bg.a, solid = true;
                color dark = 0, light = 0;
                float worst;
                if (solid) {
                    worst = color_contrast(n->st.color | 0xff000000u, under | 0xff000000u);
                } else if (fm_host_wallpaper_range(n->box, &dark, &light)) {
                    float a1 = color_contrast(n->st.color | 0xff000000u, dark), a2 = color_contrast(n->st.color | 0xff000000u, light);
                    worst = a1 < a2 ? a1 : a2;
                } else {
                    worst = 99;
                }
                if (worst < 4.5f) {
                    sb_printf(b, " [text #%06x", (unsigned)(n->st.color & 0xffffff));
                    if (solid)
                        sb_printf(b, " on #%06x", (unsigned)(under & 0xffffff));
                    else
                        sb_printf(b, " over wallpaper #%06x..#%06x", (unsigned)(dark & 0xffffff),
                                  (unsigned)(light & 0xffffff));
                    sb_printf(b, ": %.1f:1, hard to read; 4.5 needed]", (double)worst);
                }
            }
        }
        if (is_popover(n))
            sb_add(b, n->open ? " [open]" : " [closed]");
        else if (n->st.hidden)
            sb_add(b, " [hidden]");
        /* Only once laid out: before that the lists are not filled yet. */
        if ((tag_is(n, "applist") || tag_is(n, "windowlist")) && n->laid_out) {
            sb_add(b, " (shows:");
            int k = 0;
            for (fm_node *c = n->first; c; c = c->next)
                if (c->generated && c->app >= 0)
                    sb_printf(b, "%s%s", k++ ? ", " : " ", fm_host_app_label(c->app));
            sb_add(b, k ? ")" : " nothing)");
        }
        sb_add(b, "\n");
        describe_rec(d, b, n->first, depth + 1);
    }
}

char *fm_describe(fm_doc *d) {
    sbuf b;
    sb_init(&b);
    if (d->desktop)
        describe_rec(d, &b, d->desktop, 0);
    if (!b.p)
        return strdup("(no desktop)");
    return b.p;
}

/* ---- the companion's commands: [[ui:verb target ...]] ---- */

static const char *take_target(const char *s, char *out, size_t len) {
    while (*s == ' ')
        s++;
    size_t k = 0;
    if (*s == '"' || *s == '\'') {
        char q = *s;
        out[k++] = *s++;
        while (*s && *s != q && k < len - 2)
            out[k++] = *s++;
        if (*s == q)
            out[k++] = *s++;
    } else {
        while (*s && *s != ' ' && k < len - 1)
            out[k++] = *s++;
    }
    out[k] = 0;
    while (*s == ' ')
        s++;
    return s;
}

static void attr_del(fm_node *n, const char *name) {
    for (int i = 0; i < n->nattrs; i++)
        if (!strcasecmp(n->attrs[i].name, name)) {
            free(n->attrs[i].name);
            free(n->attrs[i].value);
            n->attrs[i] = n->attrs[--n->nattrs];
            return;
        }
}

static void set_text(fm_node *n, const char *text) {
    /* fixed words replace a binding and any generated text */
    attr_del(n, "bind");
    attr_del(n, "text");
    for (fm_node *c = n->first, *next; c; c = next) {
        next = c->next;
        if (c->kind == FM_TEXT && c->generated)
            fm_remove(c);
    }
    for (fm_node *c = n->first, *next; c; c = next) {
        next = c->next;
        if (c->kind == FM_TEXT && !c->generated)
            fm_remove(c);
    }
    fm_node *t = calloc(1, sizeof *t);
    t->kind = FM_TEXT;
    t->text = strdup(text);
    t->app = t->win = -1;
    fm_append(n, t);
}

const char *fm_command(fm_doc *d, const char *cmd, fm_node **touched) {
    static char res[200];
    if (touched)
        *touched = NULL;
    while (*cmd == ' ')
        cmd++;
    char verb[16];
    size_t k = 0;
    while (*cmd && *cmd != ' ' && k < sizeof verb - 1)
        verb[k++] = (char)tolower((unsigned char)*cmd++);
    verb[k] = 0;
    if (!strcmp(verb, "style") || !strcmp(verb, "css")) {
        if (!strchr(cmd, '{')) {
            snprintf(res, sizeof res, "style needs a rule like: .x { color: #fff }");
            return res;
        }
        /* The whole sheet is read again; its old problems would come back too and fill the list, so the
         * list starts empty for the reading and only problems that weren't there before are new. */
        int before = d->nproblems;
        fm_problem *old = before ? malloc(sizeof *old * (size_t)before) : NULL;
        if (old)
            memcpy(old, d->problems, sizeof *old * (size_t)before);
        d->nproblems = 0;
        fm_css_patch(d, cmd);
        d->changed = true;
        fm_problem fresh[FM_MAX_PROBLEMS];
        int nfresh = 0;
        for (int i = 0; i < d->nproblems; i++) {
            bool known = false;
            for (int k = 0; k < before && !known; k++)
                known = !strcmp(old[k].msg, d->problems[i].msg);
            for (int k = 0; k < nfresh && !known; k++)
                known = !strcmp(fresh[k].msg, d->problems[i].msg);
            if (!known)
                fresh[nfresh++] = d->problems[i];
        }
        d->nproblems = 0;
        for (int k = 0; k < before && d->nproblems < FM_MAX_PROBLEMS; k++)
            d->problems[d->nproblems++] = old[k];
        for (int k = 0; k < nfresh; k++) { /* the newest matter most: room is made for them */
            if (d->nproblems == FM_MAX_PROBLEMS)
                memmove(d->problems, d->problems + 1, sizeof d->problems[0] * (FM_MAX_PROBLEMS - 1)), d->nproblems--;
            d->problems[d->nproblems++] = fresh[k];
        }
        free(old);
        char sel[128];
        snprintf(sel, sizeof sel, "%.*s", (int)(strchr(cmd, '{') - cmd), cmd);
        { char *s0 = sel; while (*s0 == 0x20) s0++; memmove(sel, s0, strlen(s0) + 1); }
        for (char *e = sel + strlen(sel); e > sel && e[-1] == ' ';)
            *--e = 0;
        fm_node *n = fm_find(d, sel);
        if (touched)
            *touched = n;
        char wins[700];
        int nwins = fm_css_who_wins(d, n, sel, wins, sizeof wins);
        if (nfresh || nwins) {
            static char many[2000];
            size_t l = (size_t)snprintf(many, sizeof many, "styled %s, but:", sel);
            for (int k = 0; k < nfresh && l < sizeof many - 1; k++)
                l += (size_t)snprintf(many + l, sizeof many - l, "%s %s", k ? ";" : "", fresh[k].msg);
            if (nwins && l < sizeof many - 1)
                snprintf(many + l, sizeof many - l,
                         "%s on %s these still win: %s (take them out with [[ui:unstyle SELECTOR { property }]], "
                         "or add !important)",
                         nfresh ? ";" : "", sel, wins);
            return many;
        }
        snprintf(res, sizeof res, "styled %s", sel);
        return res;
    }
    if (!strcmp(verb, "unstyle")) { /* [[ui:unstyle SELECTOR]] or [[ui:unstyle SELECTOR { height; width }]] */
        char sel[128];
        const char *brace = strchr(cmd, '{');
        snprintf(sel, sizeof sel, "%.*s", brace ? (int)(brace - cmd) : (int)strlen(cmd), cmd);
        for (char *e = sel + strlen(sel); e > sel && e[-1] == ' ';)
            *--e = 0;
        memmove(sel, sel + strspn(sel, " "), strlen(sel + strspn(sel, " ")) + 1);
        char props[300] = "";
        if (brace) {
            const char *end = strchr(brace, '}');
            snprintf(props, sizeof props, "%.*s", end ? (int)(end - brace - 1) : (int)strlen(brace + 1), brace + 1);
        }
        int k = fm_css_unstyle(d, sel, props[0] ? props : NULL);
        d->changed = k > 0;
        if (touched)
            *touched = fm_find(d, sel);
        if (!k)
            snprintf(res, sizeof res, "FAILED: no rule is written for %.100s (rules are taken out by their exact "
                     "selector)", sel);
        else
            snprintf(res, sizeof res, "took %s out of %d rule%s for %.100s", props[0] ? "those properties" : "them", k,
                     k == 1 ? "" : "s", sel);
        return res;
    }
    char target[128];
    const char *rest = take_target(cmd, target, sizeof target);
    fm_node *n = fm_find(d, target);
    if (!n) {
        snprintf(res, sizeof res, "FAILED: nothing matches %s", target);
        return res;
    }
    if (touched)
        *touched = n;
    if (!strcmp(verb, "press") || !strcmp(verb, "click") || !strcmp(verb, "activate")) {
        if (is_popover(n))
            fm_set_open(d, n, !n->open);
        else
            fm_activate(d, n);
        snprintf(res, sizeof res, "pressed %s", target);
    } else if (!strcmp(verb, "open") || !strcmp(verb, "show")) {
        if (is_popover(n))
            fm_set_open(d, n, true);
        else
            fm_attr_set(n, "hidden", "false");
        snprintf(res, sizeof res, "opened %s", target);
    } else if (!strcmp(verb, "close") || !strcmp(verb, "hide")) {
        if (is_popover(n))
            fm_set_open(d, n, false);
        else
            fm_attr_set(n, "hidden", "true");
        d->changed = !is_popover(n);
        snprintf(res, sizeof res, "closed %s", target);
    } else if (!strcmp(verb, "set")) {
        /* attr=value or attr="value with spaces" */
        char attr[32];
        size_t a = 0;
        while (*rest && *rest != '=' && *rest != ' ' && a < sizeof attr - 1)
            attr[a++] = *rest++;
        attr[a] = 0;
        while (*rest == ' ' || *rest == '=')
            rest++;
        char val[400];
        snprintf(val, sizeof val, "%s", rest);
        size_t vl = strlen(val);
        if (vl >= 2 && (val[0] == '"' || val[0] == '\'') && val[vl - 1] == val[0]) {
            memmove(val, val + 1, vl - 2);
            val[vl - 2] = 0;
        }
        if (!attr[0]) {
            snprintf(res, sizeof res, "set needs attr=value");
            return res;
        }
        if (!strcasecmp(attr, "text"))
            set_text(n, val);
        else
            fm_attr_set(n, attr, val);
        d->changed = true;
        snprintf(res, sizeof res, "set %s %s", target, attr);
    } else if (!strcmp(verb, "remove") || !strcmp(verb, "delete")) {
        if (n == d->desktop || n == d->root) {
            snprintf(res, sizeof res, "can't remove the desktop itself");
            return res;
        }
        if (touched)
            *touched = NULL;
        if (d->focus && is_ancestor(n, d->focus))
            d->focus = NULL;
        if (d->pressed && is_ancestor(n, d->pressed))
            d->pressed = NULL;
        if (d->hovered && is_ancestor(n, d->hovered))
            d->hovered = NULL;
        fm_remove(n);
        d->changed = true;
        snprintf(res, sizeof res, "removed %s", target);
    } else if (!strcmp(verb, "add") || !strcmp(verb, "append")) {
        if (!strchr(rest, '<')) {
            snprintf(res, sizeof res, "add needs markup, like <button what=\"open:files\">Files</button>");
            return res;
        }
        fm_node *first = fm_parse_fragment(d, n, rest);
        if (touched)
            *touched = first ? first : n;
        if (!first) { /* said plainly, so the companion knows to try again */
            snprintf(res, sizeof res, "FAILED: nothing was added to %s (the markup made no element; was it cut off?)",
                     target);
            return res;
        }
        d->changed = true;
        if (first->id[0])
            snprintf(res, sizeof res, "added <%s id=\"%s\"> to %s", first->tag, first->id, target);
        else
            snprintf(res, sizeof res, "added <%s> to %s", first->tag, target);
    } else {
        snprintf(res, sizeof res, "unknown ui command \"%s\"", verb);
    }
    return res;
}
