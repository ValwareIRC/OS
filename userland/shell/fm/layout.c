#define _GNU_SOURCE
#include "fmi.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Layout: measure (bottom-up preferred sizes), then arrange (top-down).
 * Containers lay their children out as flex rows/columns, grids, stacks or
 * orbits. Direct children of <desktop> are surfaces: they dock to a screen
 * edge (reserving room so windows don't cover them) or are placed in the
 * area that is left. Popovers and sheets open over everything, next to the
 * element that opens them. */

/* ---- live content ---- */

int fm_app_index(const char *id) {
    if (!id || !*id)
        return -1;
    int n = fm_host_app_count();
    for (int i = 0; i < n; i++)
        if (!strcasecmp(fm_host_app_id(i), id))
            return i;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(fm_host_app_label(i), id))
            return i;
    return -1;
}

static const char *const hour_words[] = {"twelve", "one", "two", "three", "four", "five", "six",
                                          "seven",  "eight", "nine", "ten", "eleven", "twelve"};

static const char *const minute_words[] = {"", "five", "ten", "quarter", "twenty", "twenty-five", "half"};

static void time_words(const struct tm *tm, char *out, size_t len) {
    int m = (tm->tm_min + 2) / 5 * 5, h = tm->tm_hour % 12;
    if (m == 60)
        m = 0, h = (h + 1) % 12;
    int next = (h + 1) % 12;
    const char *part = tm->tm_hour < 5 ? "in the night" : tm->tm_hour < 12 ? "in the morning"
                       : tm->tm_hour < 17 ? "in the afternoon" : tm->tm_hour < 21 ? "in the evening" : "at night";
    if (m == 0)
        snprintf(out, len, "%s o'clock %s", hour_words[h], part);
    else if (m == 15)
        snprintf(out, len, "quarter past %s", hour_words[h]);
    else if (m == 30)
        snprintf(out, len, "half past %s", hour_words[h]);
    else if (m == 45)
        snprintf(out, len, "quarter to %s", hour_words[next]);
    else if (m < 30)
        snprintf(out, len, "%s past %s", minute_words[m / 5], hour_words[h]);
    else
        snprintf(out, len, "%s to %s", minute_words[(60 - m) / 5], hour_words[next]);
}

const char *fmi_own_text(fm_doc *d, fm_node *n, char *buf, size_t len) {
    const char *bind = fm_attr_get(n, "bind");
    if (bind) {
        char key[96];
        if (!strncasecmp(bind, "window.", 7) && d->cur_win >= 0)
            snprintf(key, sizeof key, "%s:%d", bind, d->cur_win);
        else
            snprintf(key, sizeof key, "%s", bind);
        if (fm_host_bind(key, buf, len))
            return buf;
        return "";
    }
    if (tag_is(n, "clock")) {
        const char *fmt = fm_attr_get(n, "format");
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        if (!fmt || !strcasecmp(fmt, "time"))
            fmt = "%H:%M";
        else if (!strcasecmp(fmt, "time12"))
            fmt = "%I:%M %p";
        else if (!strcasecmp(fmt, "seconds"))
            fmt = "%H:%M:%S";
        else if (!strcasecmp(fmt, "date"))
            fmt = "%A, %d %B";
        else if (!strcasecmp(fmt, "day"))
            fmt = "%A";
        else if (!strcasecmp(fmt, "short-date"))
            fmt = "%d %b";
        /* names a model may reach for */
        else if (!strcasecmp(fmt, "digital") || !strcasecmp(fmt, "24h") || !strcasecmp(fmt, "clock") ||
                 !strcasecmp(fmt, "analog") || !strcasecmp(fmt, "analogue") || !strcasecmp(fmt, "hours"))
            fmt = "%H:%M";
        else if (!strcasecmp(fmt, "12h") || !strcasecmp(fmt, "ampm"))
            fmt = "%I:%M %p";
        else if (!strcasecmp(fmt, "datetime") || !strcasecmp(fmt, "full"))
            fmt = "%A %H:%M";
        else if (strcasecmp(fmt, "words") && !strchr(fmt, '%') && !strstr(fmt, "HH") && !strstr(fmt, "hh") &&
                 !strstr(fmt, "MM") && !strstr(fmt, "mm"))
            fmt = "%H:%M"; /* a word nobody defined: a plain time beats gibberish */
        if (!strcasecmp(fmt, "words")) {
            time_words(&tm, buf, len);
            return buf;
        }
        if (!strchr(fmt, '%')) { /* "HH:MM" style patterns */
            char f2[64] = "";
            size_t k = 0;
            for (const char *p = fmt; *p && k < sizeof f2 - 4; p++) {
                if (!strncmp(p, "HH", 2)) f2[k++] = '%', f2[k++] = 'H', p++;
                else if (!strncmp(p, "hh", 2)) f2[k++] = '%', f2[k++] = 'I', p++;
                else if (!strncmp(p, "MM", 2) || !strncmp(p, "mm", 2)) f2[k++] = '%', f2[k++] = 'M', p++;
                else if (!strncmp(p, "SS", 2) || !strncmp(p, "ss", 2)) f2[k++] = '%', f2[k++] = 'S', p++;
                else if (!strncmp(p, "dddd", 4)) f2[k++] = '%', f2[k++] = 'A', p += 3;
                else if (!strncmp(p, "ddd", 3)) f2[k++] = '%', f2[k++] = 'a', p += 2;
                else if (!strncmp(p, "dd", 2) || !strncmp(p, "DD", 2)) f2[k++] = '%', f2[k++] = 'd', p++;
                else if (!strncmp(p, "MMMM", 4)) f2[k++] = '%', f2[k++] = 'B', p += 3;
                else if (*p == 'a' || *p == 'A') f2[k++] = '%', f2[k++] = 'p';
                else f2[k++] = *p;
            }
            f2[k] = 0;
            strftime(buf, len, f2, &tm);
        } else {
            strftime(buf, len, fmt, &tm);
        }
        return buf;
    }
    if (tag_is(n, "search") || tag_is(n, "input")) {
        const char *v = fm_attr_get(n, "value");
        snprintf(buf, len, "%s", v ? v : "");
        return buf;
    }
    const char *t = fm_attr_get(n, "text");
    if (!t)
        t = fm_attr_get(n, "label");
    if (t && (tag_is(n, "text") || tag_is(n, "label") || tag_is(n, "button") || tag_is(n, "title")))
        return t;
    return NULL;
}

void fmi_text_for_draw(const fm_style *st, const char *src, char *out, size_t len) {
    snprintf(out, len, "%s", src ? src : "");
    if (st->uppercase)
        for (char *p = out; *p; p++)
            if (*p >= 'a' && *p <= 'z')
                *p = (char)(*p - 32);
    fmi_text_case(st, out); /* lower and capitalize */
}

font *fmi_font(const fm_node *n, const fm_style *st) {
    float s = n && n->vscale > 0 ? n->vscale : 1;
    return fmi_text_font(st, s);
}

float fmi_icon_size(const fm_node *n) {
    if (n->st.icon_size > 0)
        return n->st.icon_size;
    if (n->parent && n->parent->st.icon_size > 0)
        return n->parent->st.icon_size;
    return n->st.font_size * 1.5f;
}

/* place= and dock= come from the attribute, else from the stylesheet. */
const char *fmi_place(const fm_node *n, const char *which) {
    const char *a = fm_attr_get(n, which);
    if (a)
        return a;
    const char *v = !strcmp(which, "dock") ? n->st.dock : n->st.place;
    return v[0] ? v : NULL;
}

static fm_node *new_gen(fm_doc *d, const char *tag) {
    fm_node *n = fm_node_new(d, tag);
    n->generated = true;
    return n;
}

static fm_node *new_text(const char *s) {
    fm_node *t = calloc(1, sizeof *t);
    t->kind = FM_TEXT;
    t->text = strdup(s);
    t->generated = true;
    t->app = t->win = -1;
    return t;
}

static void remove_generated(fm_node *n) {
    for (fm_node *c = n->first, *next; c; c = next) {
        next = c->next;
        if (c->generated)
            fm_remove(c);
    }
}

static void prepend(fm_node *parent, fm_node *child) {
    child->parent = parent;
    child->next = parent->first;
    parent->first = child;
    if (!parent->last)
        parent->last = child;
}

static fm_node *app_item(fm_doc *d, int app, int win, const char *labels) {
    fm_node *it = new_gen(d, "item");
    it->app = app;
    it->win = win;
    fm_attr_set(it, "app", fm_host_app_id(app));
    fm_attr_set(it, "class", win >= 0 ? "window" : "app");
    fm_node *ic = new_gen(d, "icon");
    ic->app = app;
    fm_attr_set(ic, "app", fm_host_app_id(app));
    fm_append(it, ic);
    if (!labels || strcasecmp(labels, "none")) {
        bool tip = labels && !strcasecmp(labels, "hover");
        fm_node *lb = new_gen(d, tip ? "tip" : "label");
        fm_append(lb, new_text(win >= 0 ? fm_host_window_title(win) : fm_host_app_label(app)));
        fm_append(it, lb);
    }
    return it;
}

/* A list the design didn't orient follows its container: in a column (a
 * start menu's side, a sidebar) it runs down, one item per line, instead of
 * squeezing every item into one cramped row. Runs after styling. */
static void orient_lists(fm_node *n) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        if ((tag_is(n, "applist") || tag_is(n, "windowlist")) && !n->st.dir_set && n->st.mode == FM_FLEX &&
            n->parent && n->parent->st.mode == FM_FLEX && n->parent->st.dir == FM_COLUMN) {
            n->st.dir = FM_COLUMN;
            n->st.align = FM_A_STRETCH;
        }
        orient_lists(n->first);
    }
}

/* Apps the design reaches nowhere (written before they were installed, or
 * just left out) still have to be findable: they join one list of its own,
 * a menu's if it has one, else the longest. Recomputed each frame (cheap). */
static fm_node *home_list;  /* an <applist> to add them to */
static fm_node *home_group; /* or, in a design of buttons, the group with the most app buttons */
static int strays[32], nstrays;

static bool opens_app(const fm_node *c) {
    const char *w = c->kind == FM_ELEM && !c->generated ? fm_attr_get(c, "what") : NULL;
    return w && !strncasecmp(w, "open:", 5) && fm_app_index(w + 5) >= 0;
}

static void reach_rec(fm_node *n, bool *reach, int *best, int *group_best, bool in_menu) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || is_meta(n) || n->generated)
            continue;
        int buttons = 0;
        for (fm_node *c = n->first; c; c = c->next)
            buttons += opens_app(c);
        if (buttons >= 2 && buttons + (in_menu || is_popover(n) ? 1000 : 0) > *group_best)
            *group_best = buttons + (in_menu || is_popover(n) ? 1000 : 0), home_group = n;
        const char *what = fm_attr_get(n, "what");
        if (what && !strncasecmp(what, "open:", 5)) {
            int a = fm_app_index(what + 5);
            if (a >= 0 && a < 64)
                reach[a] = true;
        }
        /* A power or redesign button of the design's own reaches those too:
         * they don't also need to turn up in an app list (where a Power Off
         * among the apps looks out of place). */
        if (what && (!strncasecmp(what, "power:", 6) || !strcasecmp(what, "power"))) {
            int a = fm_app_index("power");
            if (a >= 0 && a < 64)
                reach[a] = true;
        }
        if (what && !strcasecmp(what, "redesign")) {
            int a = fm_app_index("redesign");
            if (a >= 0 && a < 64)
                reach[a] = true;
        }
        if (tag_is(n, "applist")) {
            const char *list = fm_attr_get(n, "apps");
            int count = 0;
            if (!list || !*list || !strcasecmp(list, "all")) {
                for (int i = 0; i < 64; i++)
                    reach[i] = true;
            } else {
                char buf[256];
                snprintf(buf, sizeof buf, "%s", list);
                for (char *save, *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
                    int a = fm_app_index(t);
                    if (a >= 0 && a < 64)
                        reach[a] = true, count++;
                }
                int score = count + (in_menu ? 1000 : 0);
                if (score > *best)
                    *best = score, home_list = n;
            }
        }
        reach_rec(n->first, reach, best, group_best, in_menu || is_popover(n));
    }
}

static void find_strays(fm_doc *d) {
    bool reach[64] = {0};
    int best = -1, group_best = -1;
    home_list = home_group = NULL;
    nstrays = 0;
    reach_rec(d->desktop, reach, &best, &group_best, false);
    if (home_list)
        home_group = NULL; /* a list is the natural home */
    if (!home_list && !home_group)
        return;
    for (int i = 0; i < fm_host_app_count() && i < 64 && nstrays < 32; i++)
        if (!reach[i])
            strays[nstrays++] = i;
}

/* The strays as buttons like the group's own: the same class and, if the
 * design writes its names a certain way ("#notes"), the same way. */
static void sync_group_buttons(fm_doc *d, fm_node *g) {
    int k = 0;
    bool same = true;
    fm_node *model = NULL;
    for (fm_node *c = g->first; c; c = c->next) {
        if (c->generated && tag_is(c, "button")) {
            if (k >= nstrays || c->app != strays[k])
                same = false;
            k++;
        } else if (opens_app(c)) {
            model = c;
        }
    }
    if (same && k == nstrays)
        return;
    for (fm_node *c = g->first, *next; c; c = next) {
        next = c->next;
        if (c->generated && tag_is(c, "button"))
            fm_remove(c);
    }
    char *mt = model ? fm_text_of(model) : NULL;
    const char *t = mt ? mt + strspn(mt, " \t\n") : "";
    bool hash = *t == '#', lower = *t && islower((unsigned char)t[hash]);
    free(mt);
    for (int i = 0; i < nstrays; i++) {
        fm_node *b = new_gen(d, model ? model->tag : "button");
        b->app = strays[i];
        const char *id = fm_host_app_id(strays[i]);
        char what[64], label[64];
        snprintf(what, sizeof what, "open:%s", id);
        fm_attr_set(b, "what", what);
        fm_attr_set(b, "icon", id);
        const char *cls = model ? fm_attr_get(model, "class") : NULL;
        if (cls)
            fm_attr_set(b, "class", cls);
        snprintf(label, sizeof label, "%s%s", hash ? "#" : "", fm_host_app_label(strays[i]));
        if (lower)
            for (char *p = label; *p; p++)
                *p = (char)tolower((unsigned char)*p);
        fm_node *ic = new_gen(d, "icon");
        fm_attr_set(ic, "name", id);
        ic->app = strays[i];
        fm_append(b, ic);
        fm_append(b, new_text(label));
        if (model) { /* right after the group's own app buttons, not after whatever follows them */
            b->parent = g;
            b->next = model->next;
            model->next = b;
            if (g->last == model)
                g->last = b;
            model = b;
        } else {
            fm_append(g, b);
        }
    }
}

static bool any_power_button(const fm_node *n);

static void sync_applist(fm_doc *d, fm_node *n) {
    int want[32], nw = 0;
    const char *list = fm_attr_get(n, "apps");
    if (list && *list && strcasecmp(list, "all")) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", list);
        for (char *save, *t = strtok_r(buf, ", ", &save); t && nw < 32; t = strtok_r(NULL, ", ", &save)) {
            int a = fm_app_index(t);
            if (a >= 0)
                want[nw++] = a;
        }
    } else {
        /* every app, but Power Off only when the design has no power
         * button of its own: it is a command, not an app */
        int power = any_power_button(d->desktop ? d->desktop->first : NULL) ? fm_app_index("power") : -1;
        for (int i = 0; i < fm_host_app_count() && nw < 32; i++)
            if (i != power)
                want[nw++] = i;
    }
    if (n == home_list)
        for (int i = 0; i < nstrays && nw < 32; i++)
            want[nw++] = strays[i];
    int k = 0;
    bool same = true;
    for (fm_node *c = n->first; c; c = c->next)
        if (c->generated) {
            if (k >= nw || c->app != want[k])
                same = false;
            k++;
        }
    if (same && k == nw)
        return;
    remove_generated(n);
    const char *labels = fm_attr_get(n, "labels");
    for (int i = 0; i < nw; i++)
        fm_append(n, app_item(d, want[i], -1, labels));
}

static void sync_windowlist(fm_doc *d, fm_node *n) {
    int nw = fm_host_window_count();
    int k = 0;
    bool same = true;
    for (fm_node *c = n->first; c; c = c->next)
        if (c->generated) {
            if (k >= nw || c->win != k || c->app != fm_host_window_app(k))
                same = false;
            k++;
        }
    if (!same || k != nw) {
        remove_generated(n);
        const char *labels = fm_attr_get(n, "labels");
        for (int i = 0; i < nw; i++)
            fm_append(n, app_item(d, fm_host_window_app(i), i, labels));
    }
    for (fm_node *c = n->first; c; c = c->next)
        if (c->generated && c->win >= 0)
            c->open = fm_host_window_focused(c->win);
}

/* Keeps a generated text child holding the element's own text. */
static void sync_own_text(fm_doc *d, fm_node *n) {
    char buf[256];
    const char *t = fmi_own_text(d, n, buf, sizeof buf);
    if (!t)
        return;
    bool placeholder = false;
    if ((tag_is(n, "search") || tag_is(n, "input")) && !*t) {
        const char *ph = fm_attr_get(n, "placeholder");
        t = ph ? ph : "";
        placeholder = true;
    }
    fm_node *tn = NULL;
    for (fm_node *c = n->first; c; c = c->next)
        if (c->generated && c->kind == FM_TEXT)
            tn = c;
    if (!tn) {
        tn = new_text(t);
        fm_append(n, tn);
    } else if (strcmp(tn->text, t)) {
        free(tn->text);
        tn->text = strdup(t);
    }
    tn->app = placeholder ? -2 : -1;
}

static void sync_icon_attr(fm_doc *d, fm_node *n) {
    const char *ic = fm_attr_get(n, "icon");
    if (!ic || tag_is(n, "icon") || n->generated)
        return;
    fm_node *g = n->first && n->first->generated && tag_is(n->first, "icon") ? n->first : NULL;
    if (!g) {
        g = new_gen(d, "icon");
        prepend(n, g);
    }
    const char *cur = fm_attr_get(g, "name");
    if (!cur || strcmp(cur, ic)) {
        fm_attr_set(g, "name", ic);
        g->app = fm_app_index(ic);
    }
}

static void sync_rec(fm_doc *d, fm_node *n) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || is_meta(n))
            continue;
        if (n == home_group)
            sync_group_buttons(d, n);
        if (tag_is(n, "applist"))
            sync_applist(d, n);
        else if (tag_is(n, "windowlist"))
            sync_windowlist(d, n);
        sync_icon_attr(d, n);
        sync_own_text(d, n);
        if (tag_is(n, "icon") && !n->generated) {
            const char *a = fm_attr_get(n, "app");
            if (!a)
                a = fm_attr_get(n, "name");
            n->app = fm_app_index(a);
        }
        sync_rec(d, n->first);
    }
}

/* ---- menus that aren't there ----
 * A button that opens a menu (what="toggle:#id") whose menu is gone gets a
 * plain one: a rework that removed the old menu and whose new one never
 * arrived (cut off, or markup that didn't parse) left people with a Menu
 * button that did nothing, and no way to their apps. The menu holds a
 * search, every app, and Power Off when the design has no power button of
 * its own; it is saved like any other change, for them or the companion to
 * reshape. */

static bool any_power_button(const fm_node *n) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        const char *w = fm_attr_get(n, "what");
        if (w && !strncasecmp(w, "power", 5) && !n->generated)
            return true;
        if (any_power_button(n->first))
            return true;
    }
    return false;
}

static fm_node *dangling_toggle(fm_doc *d, fm_node *n, char *id, size_t idn) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || n->generated)
            continue;
        const char *w = fm_attr_get(n, "what");
        if (w && !strncasecmp(w, "toggle:#", 8)) {
            snprintf(id, idn, "%.*s", (int)strcspn(w + 8, " ;"), w + 8);
            char sel[80];
            snprintf(sel, sizeof sel, "#%s", id);
            if (id[0] && !fm_find(d, sel))
                return n;
        }
        fm_node *r = dangling_toggle(d, n->first, id, idn);
        if (r)
            return r;
    }
    return NULL;
}

static void repair_menus(fm_doc *d) {
    char id[64];
    fm_node *b = dangling_toggle(d, d->desktop->first, id, sizeof id);
    if (!b)
        return;
    char markup[600];
    snprintf(markup, sizeof markup,
             "<popover id=\"%s\" placement=\"auto\" align=\"start\"><search/><applist apps=\"all\" labels=\"right\"/>%s"
             "</popover>",
             id, any_power_button(d->desktop->first) ? "" : "<divider/><button what=\"power:off\" icon=\"power\">Power Off</button>");
    fm_parse_fragment(d, d->desktop, markup);
    d->changed = true;
    fm_report(d, 0, false, "#%s didn't exist, so its button opened nothing: a plain menu was put back", id);
}

void fmi_sync_live(fm_doc *d) {
    if (d->desktop) {
        repair_menus(d);
        find_strays(d);
        sync_rec(d, d->desktop);
    }
}

/* ---- measuring ---- */

static float rl(fm_len l, float avail, float autov) {
    switch (l.unit) {
    case FM_PX: return l.v;
    case FM_PCT: return avail * l.v / 100.0f;
    case FM_FULL: return avail;
    default: return autov;
    }
}

static float clamp_len(float v, fm_len mn, fm_len mx, float avail) {
    if (mx.unit != FM_AUTO && v > rl(mx, avail, v))
        v = rl(mx, avail, v);
    if (mn.unit != FM_AUTO && v < rl(mn, avail, v))
        v = rl(mn, avail, v);
    return v < 0 ? 0 : v;
}

static void edges(const fm_style *st, float *l, float *t, float *r, float *b) {
    *l = st->pad[3] + st->border_w;
    *t = st->pad[0] + st->border_w;
    *r = st->pad[1] + st->border_w;
    *b = st->pad[2] + st->border_w;
}

static void measure_text(fm_node *n, float aw) {
    const fm_style *st = n->parent ? &n->parent->st : &n->st;
    font *f = fmi_text_font(st, 1);
    char buf[512];
    fmi_text_for_draw(st, n->text, buf, sizeof buf);
    float w = (float)text_width(f, buf);
    float lh = (float)font_line_height(f) * st->line_height;
    if (aw > 24 && w > aw && !st->nowrap) {
        n->mw = aw;
        n->mh = (float)fmi_measure_block(f, (int)aw, buf, st);
    } else {
        n->mw = w;
        n->mh = lh;
    }
    /* Text inherits how its parent's text looks, not its box. */
    n->st = *st;
    fm_len aut = {FM_AUTO, 0};
    n->st.width = n->st.height = n->st.min_w = n->st.min_h = n->st.max_w = n->st.max_h = aut;
    n->st.left = n->st.top = n->st.right = n->st.bottom = aut;
    memset(n->st.margin, 0, sizeof n->st.margin);
    memset(n->st.pad, 0, sizeof n->st.pad);
    n->st.grow = 0;
    n->st.absolute = false;
    n->st.self_align_set = false;
}

void fmi_measure(fm_doc *d, fm_node *n, float aw, float ah) {
    if (n->kind == FM_TEXT) {
        measure_text(n, aw);
        return;
    }
    fm_style *st = &n->st;
    float pl, pt, pr, pb;
    edges(st, &pl, &pt, &pr, &pb);
    float fw = st->width.unit == FM_AUTO ? -1 : rl(st->width, aw, 0);
    float fh = st->height.unit == FM_AUTO ? -1 : rl(st->height, ah, 0);
    float caw = (fw >= 0 ? fw : aw - st->margin[1] - st->margin[3]) - pl - pr;
    float cah = (fh >= 0 ? fh : ah - st->margin[0] - st->margin[2]) - pt - pb;
    if (caw < 0) caw = 0;
    if (cah < 0) cah = 0;
    float cw = 0, ch = 0;

    if (tag_is(n, "icon")) {
        cw = ch = fmi_icon_size(n);
    } else if (tag_is(n, "orb")) {
        cw = ch = st->icon_size > 0 ? st->icon_size : 56;
    } else if (tag_is(n, "view")) {
        cw = 420, ch = 320;
    } else if (is_tray(n)) {
        cw = ch = fmi_icon_size(n);
        cw += fm_host_tray_extra(n);
    } else if (tag_is(n, "divider")) {
        cw = ch = 1;
    } else if (tag_is(n, "canvas")) {
        cw = 240, ch = 160;
    }

    fm_node *kids[256];
    int nk = 0;
    for (fm_node *c = n->first; c && nk < 256; c = c->next)
        if (in_flow(c) && !tag_is(c, "tip"))
            kids[nk++] = c;

    if (nk) {
        for (int i = 0; i < nk; i++)
            fmi_measure(d, kids[i], caw, cah);
        float gap = st->gap;
        if (st->mode == FM_GRID) {
            int cols = st->columns > 0 ? st->columns : 3;
            float cellw = 0, total_h = 0, row_h = 0;
            for (int i = 0; i < nk; i++) {
                float w = kids[i]->mw + kids[i]->st.margin[1] + kids[i]->st.margin[3];
                if (w > cellw) cellw = w;
            }
            for (int i = 0; i < nk; i++) {
                float h = kids[i]->mh + kids[i]->st.margin[0] + kids[i]->st.margin[2];
                if (h > row_h) row_h = h;
                if (i % cols == cols - 1 || i == nk - 1) {
                    total_h += row_h + (total_h > 0 ? gap : 0);
                    row_h = 0;
                }
            }
            int c = nk < cols ? nk : cols;
            cw = fmaxf(cw, c * cellw + gap * (c - 1));
            ch = fmaxf(ch, total_h);
        } else if (st->mode == FM_STACK) {
            for (int i = 0; i < nk; i++) {
                cw = fmaxf(cw, kids[i]->mw + kids[i]->st.margin[1] + kids[i]->st.margin[3]);
                ch = fmaxf(ch, kids[i]->mh + kids[i]->st.margin[0] + kids[i]->st.margin[2]);
            }
        } else if (st->mode == FM_ORBIT) {
            float R = st->orbit > 0 ? st->orbit : 120, mx = 0;
            for (int i = 0; i < nk; i++)
                mx = fmaxf(mx, fmaxf(kids[i]->mw, kids[i]->mh));
            float rmin = nk > 1 ? mx * 1.15f * (float)nk / (2 * (float)M_PI) : 0; /* room for every item */
            if (R < rmin)
                R = rmin;
            cw = fmaxf(cw, 2 * R + mx);
            ch = fmaxf(ch, 2 * R + mx);
        } else {
            bool row = st->dir == FM_ROW;
            float main = 0, cross = 0, line_main = 0, line_cross = 0, lim = row ? caw : cah;
            int in_line = 0;
            for (int i = 0; i < nk; i++) {
                fm_node *k = kids[i];
                float m = row ? k->mw + k->st.margin[1] + k->st.margin[3] : k->mh + k->st.margin[0] + k->st.margin[2];
                float c = row ? k->mh + k->st.margin[0] + k->st.margin[2] : k->mw + k->st.margin[1] + k->st.margin[3];
                if (st->wrap && in_line && line_main + gap + m > lim) {
                    main = fmaxf(main, line_main);
                    cross += line_cross + gap;
                    line_main = line_cross = 0;
                    in_line = 0;
                }
                line_main += (in_line ? gap : 0) + m;
                line_cross = fmaxf(line_cross, c);
                in_line++;
            }
            main = fmaxf(main, line_main);
            cross += line_cross;
            if (row)
                cw = fmaxf(cw, main), ch = fmaxf(ch, cross);
            else
                cw = fmaxf(cw, cross), ch = fmaxf(ch, main);
        }
    }
    float w = fw >= 0 ? fw : cw + pl + pr;
    float h = fh >= 0 ? fh : ch + pt + pb;
    n->mw = clamp_len(w, st->min_w, st->max_w, aw);
    n->mh = clamp_len(h, st->min_h, st->max_h, ah);
}

/* ---- arranging ---- */

/* The desktop's placed surfaces so far, while arranging it (not popovers). */
static struct {
    bool active;
    int n;
    rect r[32];
} placing;

static void arrange_abs(fm_doc *d, fm_node *k, rect box);

static float cross_size(fm_node *k, enum fm_align parent_align, float line_cross, bool row, float avail_cross) {
    fm_len fixed = row ? k->st.height : k->st.width;
    float margins = row ? k->st.margin[0] + k->st.margin[2] : k->st.margin[1] + k->st.margin[3];
    if (fixed.unit != FM_AUTO)
        return rl(fixed, avail_cross, 0);
    enum fm_align a = k->st.self_align_set ? k->st.self_align : parent_align;
    if (a == FM_A_STRETCH && k->kind == FM_ELEM)
        return clamp_len(line_cross - margins, row ? k->st.min_h : k->st.min_w, row ? k->st.max_h : k->st.max_w,
                         avail_cross);
    return row ? k->mh : k->mw;
}

static void arrange_line(fm_doc *d, fm_node *n, fm_node **kids, int nk, rect c, float line_pos, float line_cross) {
    const fm_style *st = &n->st;
    bool row = st->dir == FM_ROW;
    float main_avail = row ? c.w : c.h, gap = st->gap;
    float sz[256], used = 0, grow = 0;
    for (int i = 0; i < nk; i++) {
        fm_node *k = kids[i];
        fm_len fixed = row ? k->st.width : k->st.height;
        sz[i] = fixed.unit != FM_AUTO ? rl(fixed, main_avail, 0) : (row ? k->mw : k->mh);
        used += sz[i] + (row ? k->st.margin[1] + k->st.margin[3] : k->st.margin[0] + k->st.margin[2]);
        if (k->kind == FM_ELEM)
            grow += k->st.grow;
    }
    used += gap * (nk - 1);
    float free_space = main_avail - used;
    if (free_space > 0 && grow > 0) {
        for (int i = 0; i < nk; i++)
            if (kids[i]->kind == FM_ELEM && kids[i]->st.grow > 0) {
                float add = free_space * kids[i]->st.grow / grow;
                fm_len mx = row ? kids[i]->st.max_w : kids[i]->st.max_h;
                if (mx.unit != FM_AUTO && sz[i] + add > rl(mx, main_avail, 0))
                    add = rl(mx, main_avail, 0) - sz[i];
                sz[i] += add;
            }
        free_space = 0;
    } else if (free_space < 0) {
        /* shrink what isn't a fixed size, in proportion */
        float shrinkable = 0;
        for (int i = 0; i < nk; i++) {
            fm_len fixed = row ? kids[i]->st.width : kids[i]->st.height;
            if (fixed.unit != FM_PX)
                shrinkable += sz[i];
        }
        if (shrinkable > 0) {
            float f = fmaxf(0, 1 + free_space / shrinkable);
            for (int i = 0; i < nk; i++) {
                fm_len fixed = row ? kids[i]->st.width : kids[i]->st.height;
                if (fixed.unit != FM_PX)
                    sz[i] *= f;
            }
            free_space = 0;
        }
    }
    float pos = 0, between = gap;
    if (free_space > 0) {
        switch (st->justify) {
        case FM_J_CENTER: pos = free_space / 2; break;
        case FM_J_END: pos = free_space; break;
        case FM_J_BETWEEN: between = nk > 1 ? gap + free_space / (nk - 1) : gap; break;
        case FM_J_AROUND: pos = free_space / nk / 2; between = gap + free_space / nk; break;
        case FM_J_EVENLY: pos = free_space / (nk + 1); between = gap + free_space / (nk + 1); break;
        default: break;
        }
    }
    float avail_cross = row ? c.h : c.w;
    for (int i = 0; i < nk; i++) {
        fm_node *k = kids[i];
        float m0 = row ? k->st.margin[3] : k->st.margin[0], m1 = row ? k->st.margin[1] : k->st.margin[2];
        float c0 = row ? k->st.margin[0] : k->st.margin[3], c1 = row ? k->st.margin[2] : k->st.margin[1];
        pos += m0;
        float cs = cross_size(k, st->align, line_cross, row, avail_cross);
        enum fm_align a = k->kind == FM_ELEM && k->st.self_align_set ? k->st.self_align : st->align;
        float cp;
        if (a == FM_A_CENTER || (k->kind == FM_TEXT && a == FM_A_STRETCH && row))
            cp = (line_cross - c0 - c1 - cs) / 2 + c0;
        else if (a == FM_A_END)
            cp = line_cross - c1 - cs;
        else
            cp = c0;
        rect r;
        if (row)
            r = (rect){(int)lroundf(c.x + pos), (int)lroundf(c.y + line_pos + cp), (int)lroundf(sz[i]), (int)lroundf(cs)};
        else
            r = (rect){(int)lroundf(c.x + line_pos + cp), (int)lroundf(c.y + pos), (int)lroundf(cs), (int)lroundf(sz[i])};
        fmi_arrange(d, k, r);
        pos += sz[i] + m1 + between;
    }
}

static void arrange_flex(fm_doc *d, fm_node *n, fm_node **kids, int nk, rect c) {
    const fm_style *st = &n->st;
    bool row = st->dir == FM_ROW;
    float lim = row ? c.w : c.h, gap = st->gap;
    if (!st->wrap) {
        arrange_line(d, n, kids, nk, c, 0, row ? c.h : c.w);
        return;
    }
    int start = 0;
    float line_pos = 0;
    while (start < nk) {
        float line_main = 0, line_cross = 0;
        int end = start;
        while (end < nk) {
            fm_node *k = kids[end];
            float m = row ? k->mw + k->st.margin[1] + k->st.margin[3] : k->mh + k->st.margin[0] + k->st.margin[2];
            if (end > start && line_main + gap + m > lim)
                break;
            line_main += (end > start ? gap : 0) + m;
            float cr = row ? k->mh + k->st.margin[0] + k->st.margin[2] : k->mw + k->st.margin[1] + k->st.margin[3];
            line_cross = fmaxf(line_cross, cr);
            end++;
        }
        arrange_line(d, n, kids + start, end - start, c, line_pos, line_cross);
        line_pos += line_cross + gap;
        start = end;
    }
}

static void arrange_grid(fm_doc *d, fm_node *n, fm_node **kids, int nk, rect c) {
    const fm_style *st = &n->st;
    int cols = st->columns > 0 ? st->columns : 3;
    float gap = st->gap;
    float cellw = (c.w - gap * (cols - 1)) / cols;
    float y = 0;
    for (int r0 = 0; r0 < nk; r0 += cols) {
        float row_h = 0;
        for (int i = r0; i < nk && i < r0 + cols; i++) {
            fmi_measure(d, kids[i], cellw, c.h);
            row_h = fmaxf(row_h, kids[i]->mh + kids[i]->st.margin[0] + kids[i]->st.margin[2]);
        }
        for (int i = r0; i < nk && i < r0 + cols; i++) {
            fm_node *k = kids[i];
            float x = (i - r0) * (cellw + gap);
            float w = cross_size(k, st->align, cellw, false, cellw);
            float h = cross_size(k, st->align, row_h, true, row_h);
            float ox = st->align == FM_A_CENTER ? (cellw - w) / 2 : st->align == FM_A_END ? cellw - w : 0;
            float oy = st->align == FM_A_CENTER ? (row_h - h) / 2 : st->align == FM_A_END ? row_h - h : 0;
            fmi_arrange(d, k, (rect){(int)(c.x + x + ox + k->st.margin[3]), (int)(c.y + y + oy + k->st.margin[0]),
                                     (int)(w), (int)(h)});
        }
        y += row_h + gap;
    }
}

static void arrange_stack(fm_doc *d, fm_node *n, fm_node **kids, int nk, rect c) {
    const fm_style *st = &n->st;
    for (int i = 0; i < nk; i++) {
        fm_node *k = kids[i];
        /* Layers stretch by default, but an orb or a ring of apps is a
         * shape with its own size: it is centred in the stack instead. */
        bool shape = tag_is(k, "orb") || k->st.mode == FM_ORBIT || tag_is(k, "clock") || tag_is(k, "icon");
        enum fm_align al = shape && st->align == FM_A_STRETCH ? FM_A_CENTER : st->align;
        enum fm_justify ju = shape && st->justify == FM_J_START ? FM_J_CENTER : st->justify;
        float w = cross_size(k, al, (float)c.w, false, (float)c.w);
        float h = k->st.height.unit != FM_AUTO ? rl(k->st.height, (float)c.h, 0)
                  : (al == FM_A_STRETCH && k->kind == FM_ELEM) ? (float)c.h - k->st.margin[0] - k->st.margin[2]
                                                               : k->mh;
        float x = al == FM_A_CENTER ? (c.w - w) / 2 : al == FM_A_END ? c.w - w - k->st.margin[1] : k->st.margin[3];
        float y = ju == FM_J_CENTER ? (c.h - h) / 2 : ju == FM_J_END ? c.h - h - k->st.margin[2] : k->st.margin[0];
        fmi_arrange(d, k, (rect){(int)(c.x + x), (int)(c.y + y), (int)w, (int)h});
    }
}

static void arrange_orbit(fm_doc *d, fm_node *n, fm_node **kids, int nk, rect c) {
    const fm_style *st = &n->st;
    float R = st->orbit > 0 ? st->orbit : fminf(c.w, c.h) / 2 - 40;
    const char *sa = fm_attr_get(n, "start-angle"), *sp = fm_attr_get(n, "spread");
    float start = sa ? strtof(sa, NULL) : -90, spread = sp ? strtof(sp, NULL) : 360;
    /* The circle is always big enough that its items don't sit on top of
     * each other, whatever size the container was given. */
    float mx = 0;
    for (int i = 0; i < nk; i++)
        mx = fmaxf(mx, fmaxf(kids[i]->mw, kids[i]->mh));
    float arc = fmaxf(spread, 30) * (float)M_PI / 180;
    float rmin = nk > 1 ? (mx * 1.15f) * (float)(spread >= 359 ? nk : nk - 1) / arc : 0;
    if (R < rmin)
        R = rmin;
    float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f;
    int slots = spread >= 359 ? nk : (nk > 1 ? nk - 1 : 1);
    for (int i = 0; i < nk; i++) {
        float a = (start + spread * i / (float)slots) * (float)M_PI / 180;
        fm_node *k = kids[i];
        float w = k->mw, h = k->mh;
        fmi_arrange(d, k, (rect){(int)(cx + cosf(a) * R - w / 2), (int)(cy + sinf(a) * R - h / 2), (int)w, (int)h});
    }
}

static void arrange_abs(fm_doc *d, fm_node *k, rect box) {
    const fm_style *s = &k->st;
    fmi_measure(d, k, (float)box.w, (float)box.h);
    float w = s->width.unit != FM_AUTO ? rl(s->width, (float)box.w, 0)
              : (s->left.unit != FM_AUTO && s->right.unit != FM_AUTO)
                  ? box.w - rl(s->left, (float)box.w, 0) - rl(s->right, (float)box.w, 0)
                  : k->mw;
    float h = s->height.unit != FM_AUTO ? rl(s->height, (float)box.h, 0)
              : (s->top.unit != FM_AUTO && s->bottom.unit != FM_AUTO)
                  ? box.h - rl(s->top, (float)box.h, 0) - rl(s->bottom, (float)box.h, 0)
                  : k->mh;
    float x = s->left.unit != FM_AUTO ? box.x + rl(s->left, (float)box.w, 0)
              : s->right.unit != FM_AUTO ? box.x + box.w - rl(s->right, (float)box.w, 0) - w
                                         : box.x + (box.w - w) / 2;
    float y = s->top.unit != FM_AUTO ? box.y + rl(s->top, (float)box.h, 0)
              : s->bottom.unit != FM_AUTO ? box.y + box.h - rl(s->bottom, (float)box.h, 0) - h
                                          : box.y + (box.h - h) / 2;
    x += w * s->tx_pct / 100, y += h * s->ty_pct / 100; /* left:50% + translateX(-50%) centres */
    /* An explicit position on the desktop still doesn't get to sit on top
     * of a surface placed before it. */
    if (placing.active) {
        for (int tries = 0; tries < 8; tries++) {
            int hit = -1;
            for (int i = 0; i < placing.n && hit < 0; i++) {
                rect o = placing.r[i];
                if (x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h)
                    hit = i;
            }
            if (hit < 0)
                break;
            y = placing.r[hit].y + placing.r[hit].h + 12;
        }
        if (placing.n < 32)
            placing.r[placing.n++] = (rect){(int)x, (int)y, (int)w, (int)h};
    }
    fmi_arrange(d, k, (rect){(int)x, (int)y, (int)w, (int)h});
}

static void shift_tree(fm_node *n, int dy) {
    for (fm_node *c = n->first; c; c = c->next) {
        c->box.y += dy;
        shift_tree(c, dy);
    }
}

void fmi_arrange(fm_doc *d, fm_node *n, rect box) {
    n->box = box;
    n->laid_out = true;
    if (n->kind == FM_TEXT)
        return;
    float pl, pt, pr, pb;
    edges(&n->st, &pl, &pt, &pr, &pb);
    rect c = {box.x + (int)pl, box.y + (int)pt, box.w - (int)(pl + pr), box.h - (int)(pt + pb)};
    if (c.w < 0) c.w = 0;
    if (c.h < 0) c.h = 0;
    fm_node *kids[256];
    int nk = 0;
    for (fm_node *k = n->first; k; k = k->next) {
        k->laid_out = false;
        if (k->kind == FM_ELEM && !k->st.hidden && k->st.absolute && !is_popover(k) && !is_meta(k))
            arrange_abs(d, k, box);
        else if (in_flow(k) && !tag_is(k, "tip") && nk < 256)
            kids[nk++] = k;
    }
    for (int i = 0; i < nk; i++)
        fmi_measure(d, kids[i], (float)c.w, (float)c.h);
    switch (n->st.mode) {
    case FM_GRID: arrange_grid(d, n, kids, nk, c); break;
    case FM_STACK: arrange_stack(d, n, kids, nk, c); break;
    case FM_ORBIT: arrange_orbit(d, n, kids, nk, c); break;
    default: arrange_flex(d, n, kids, nk, c); break;
    }
    /* tooltips sit above their element */
    for (fm_node *k = n->first; k; k = k->next)
        if (tag_is(k, "tip")) {
            fmi_measure(d, k, 400, 100);
            fmi_arrange(d, k, (rect){box.x + (box.w - (int)k->mw) / 2, box.y - (int)k->mh - 8, (int)k->mw, (int)k->mh});
        }
    /* scrolling */
    int bottom = c.y;
    for (int i = 0; i < nk; i++)
        if (kids[i]->box.y + kids[i]->box.h > bottom)
            bottom = kids[i]->box.y + kids[i]->box.h;
    n->content_h = bottom - c.y;
    if (n->st.overflow == FM_OV_SCROLL) {
        int max = n->content_h - c.h;
        if (n->scroll > max) n->scroll = max;
        if (n->scroll < 0) n->scroll = 0;
        if (n->scroll)
            shift_tree(n, -n->scroll);
    } else {
        n->scroll = 0;
    }
}

/* ---- the desktop ---- */


static void place_in(fm_doc *d, fm_node *k, rect area) {
    const fm_style *s = &k->st;
    fmi_measure(d, k, (float)area.w, (float)area.h);
    float w = s->width.unit != FM_AUTO ? rl(s->width, (float)area.w, 0) : k->mw;
    float h = s->height.unit != FM_AUTO ? rl(s->height, (float)area.h, 0) : k->mh;
    if (w > area.w) w = (float)area.w;
    if (h > area.h) h = (float)area.h;
    const char *place = fmi_place(k, "place");
    if (!place)
        place = "center";
    float x = area.x + (area.w - w) / 2, y = area.y + (area.h - h) / 2;
    /* Nothing placed sits flush against the edge of the screen: it reads
     * as cut off. (Bars that want the edge are docked, not placed.) */
    const float gap = 24;
    float ml = s->margin[3] > gap ? s->margin[3] : gap, mr = s->margin[1] > gap ? s->margin[1] : gap;
    float mt = s->margin[0] > gap ? s->margin[0] : gap, mb = s->margin[2] > gap ? s->margin[2] : gap;
    if (strstr(place, "left"))
        x = area.x + ml;
    else if (strstr(place, "right"))
        x = area.x + area.w - w - mr;
    if (strstr(place, "top"))
        y = area.y + mt;
    else if (strstr(place, "bottom"))
        y = area.y + area.h - h - mb;
    bool fill = !strcmp(place, "fill") || !strcmp(place, "full");
    if (fill) {
        x = area.x + s->margin[3], y = area.y + s->margin[0];
        w = area.w - s->margin[1] - s->margin[3], h = area.h - s->margin[0] - s->margin[2];
    }
    /* Two surfaces asked for the same spot (a greeting and a search both
     * "top"): the later one makes way, below (or above, at the bottom),
     * instead of drawing over the first. */
    if (placing.active && !fill) {
        bool up = strstr(place, "bottom") != NULL;
        for (int tries = 0; tries < 8; tries++) {
            int hit = -1;
            for (int i = 0; i < placing.n && hit < 0; i++) {
                rect o = placing.r[i];
                if (x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h)
                    hit = i;
            }
            if (hit < 0)
                break;
            rect o = placing.r[hit];
            y = up ? o.y - h - 12 : o.y + o.h + 12;
        }
        if (y < area.y)
            y = (float)area.y;
        if (y + h > area.y + area.h)
            y = area.y + area.h - h;
        if (placing.n < 32)
            placing.r[placing.n++] = (rect){(int)x, (int)y, (int)w, (int)h};
    }
    fmi_arrange(d, k, (rect){(int)x, (int)y, (int)w, (int)h});
}

static void arrange_desktop(fm_doc *d) {
    fm_node *desk = d->desktop;
    rect area = d->screen;
    desk->box = d->screen;
    desk->laid_out = true;
    /* edge-docked surfaces first, in document order */
    for (fm_node *k = desk->first; k; k = k->next) {
        if (k->kind != FM_ELEM || k->st.hidden || is_popover(k) || is_meta(k))
            continue;
        k->laid_out = false;
        const char *dock = fmi_place(k, "dock");
        if (!dock)
            continue;
        const fm_style *s = &k->st;
        bool horiz = !strcasecmp(dock, "top") || !strcasecmp(dock, "bottom");
        fmi_measure(d, k, (float)area.w, (float)area.h);
        float w, h;
        if (horiz) {
            w = s->width.unit != FM_AUTO ? rl(s->width, (float)area.w, 0) : k->mw;
            const char *wa = fm_attr_get(k, "width");
            if (wa && !strcasecmp(wa, "full"))
                w = (float)area.w - s->margin[1] - s->margin[3];
            h = s->height.unit != FM_AUTO ? rl(s->height, (float)area.h, 0) : k->mh;
        } else {
            h = s->height.unit != FM_AUTO ? rl(s->height, (float)area.h, 0) : k->mh;
            const char *ha = fm_attr_get(k, "height");
            if (ha && !strcasecmp(ha, "full"))
                h = (float)area.h - s->margin[0] - s->margin[2];
            w = s->width.unit != FM_AUTO ? rl(s->width, (float)area.w, 0) : k->mw;
        }
        if (w > area.w) w = (float)area.w;
        if (h > area.h) h = (float)area.h;
        const char *al = fm_attr_get(k, "align");
        float along_x = area.x + (area.w - w) / 2, along_y = area.y + (area.h - h) / 2;
        if (al && (!strcasecmp(al, "start") || !strcasecmp(al, "left") || !strcasecmp(al, "top")))
            along_x = area.x + s->margin[3], along_y = area.y + s->margin[0];
        else if (al && (!strcasecmp(al, "end") || !strcasecmp(al, "right") || !strcasecmp(al, "bottom")))
            along_x = area.x + area.w - w - s->margin[1], along_y = area.y + area.h - h - s->margin[2];
        rect r;
        float used;
        if (!strcasecmp(dock, "top")) {
            r = (rect){(int)along_x, (int)(area.y + s->margin[0]), (int)w, (int)h};
            used = h + s->margin[0] + s->margin[2];
        } else if (!strcasecmp(dock, "bottom")) {
            r = (rect){(int)along_x, (int)(area.y + area.h - h - s->margin[2]), (int)w, (int)h};
            used = h + s->margin[0] + s->margin[2];
        } else if (!strcasecmp(dock, "left")) {
            r = (rect){(int)(area.x + s->margin[3]), (int)along_y, (int)w, (int)h};
            used = w + s->margin[1] + s->margin[3];
        } else {
            r = (rect){(int)(area.x + area.w - w - s->margin[1]), (int)along_y, (int)w, (int)h};
            used = w + s->margin[1] + s->margin[3];
        }
        fmi_arrange(d, k, r);
        const char *reserve = fm_attr_get(k, "reserve");
        if (reserve && (!strcasecmp(reserve, "false") || !strcasecmp(reserve, "no")))
            continue;
        if (!strcasecmp(dock, "top")) area.y += (int)used, area.h -= (int)used;
        else if (!strcasecmp(dock, "bottom")) area.h -= (int)used;
        else if (!strcasecmp(dock, "left")) area.x += (int)used, area.w -= (int)used;
        else area.w -= (int)used;
    }
    d->work = area;
    placing.active = true;
    placing.n = 0;
    for (fm_node *k = desk->first; k; k = k->next) {
        if (k->kind != FM_ELEM || k->st.hidden || is_popover(k) || is_meta(k) || fmi_place(k, "dock"))
            continue;
        const char *over = fm_attr_get(k, "over");
        rect within = over && !strcasecmp(over, "screen") ? d->screen : area;
        if (k->st.absolute)
            arrange_abs(d, k, within);
        else
            place_in(d, k, within);
    }
    placing.active = false;
}

/* ---- popovers ---- */

const char *fmi_action_of(const fm_node *n) {
    static const char *const names[] = {"what", "action", "do", "on-click", "onclick", "click"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = fm_attr_get(n, names[i]);
        if (v && *v)
            return v;
    }
    if (tag_is(n, "orb"))
        return "open:companion";
    /* the tray opens its popovers; every clock opens a calendar unless told not to */
    if (is_tray(n)) {
        static char buf[40];
        snprintf(buf, sizeof buf, "tray:%s", n->tag);
        return buf;
    }
    if (tag_is(n, "clock")) {
        const char *cal = fm_attr_get(n, "calendar");
        if (!cal || (strcasecmp(cal, "false") && strcasecmp(cal, "off") && strcasecmp(cal, "none")))
            return "tray:calendar";
    }
    if (n->generated && tag_is(n, "item") && n->app >= 0) {
        static char buf[64];
        if (n->win >= 0)
            snprintf(buf, sizeof buf, "window:focus:%d", n->win);
        else
            snprintf(buf, sizeof buf, "open:%s", fm_host_app_id(n->app));
        return buf;
    }
    return NULL;
}

static fm_node *find_toggler(fm_node *n, const char *id) {
    for (; n; n = n->next) {
        if (n->kind == FM_ELEM && !is_meta(n)) {
            const char *a = fmi_action_of(n);
            if (a) {
                const char *h = strstr(a, id);
                if (h && h > a && h[-1] == '#' && !isalnum((unsigned char)h[strlen(id)]) && h[strlen(id)] != '-')
                    return n;
            }
            fm_node *r = find_toggler(n->first, id);
            if (r)
                return r;
        }
    }
    return NULL;
}

fm_node *fmi_toggler_of(fm_doc *d, const fm_node *pop) {
    const char *anchor = fm_attr_get(pop, "anchor");
    if (anchor) {
        fm_node *a = fm_find(d, anchor);
        if (a)
            return a;
    }
    fm_node *t = d->desktop ? find_toggler(d->desktop->first, pop->id) : NULL;
    if (t)
        return t;
    for (fm_node *p = pop->parent; p; p = p->parent)
        if (!is_popover(p) && p->laid_out)
            return p;
    return NULL;
}

static void place_popover(fm_doc *d, fm_node *p) {
    rect S = d->screen;
    const fm_style *s = &p->st;
    bool sheet = tag_is(p, "sheet");
    fmi_measure(d, p, (float)S.w, (float)S.h);
    float w = s->width.unit != FM_AUTO ? rl(s->width, (float)S.w, 0) : (sheet ? S.w : p->mw);
    float h = s->height.unit != FM_AUTO ? rl(s->height, (float)S.h, 0) : (sheet ? S.h : p->mh);
    if (w > S.w - 16 && !sheet) w = (float)S.w - 16;
    if (h > S.h - 16 && !sheet) h = (float)S.h - 16;
    const char *pl = fm_attr_get(p, "placement");
    fm_node *a = sheet ? NULL : fmi_toggler_of(d, p);
    float x, y;
    if (!a || (pl && !strcasecmp(pl, "center")) || (pl && !strcasecmp(pl, "screen"))) {
        const char *place = fmi_place(p, "place");
        if (place) {
            place_in(d, p, S);
            return;
        }
        x = S.x + (S.w - w) / 2;
        y = S.y + (S.h - h) / 2;
    } else {
        rect ar = a->box;
        float gap = 10;
        const char *where = pl ? pl : "auto";
        if (!strcasecmp(where, "auto")) {
            /* open away from the nearest screen edge */
            float cx = ar.x + ar.w / 2.0f - S.x, cy = ar.y + ar.h / 2.0f - S.y;
            float dt = cy, db = S.h - cy, dl = cx, dr = S.w - cx;
            float m = fminf(fminf(dt, db), fminf(dl, dr));
            where = m == db ? "above" : m == dt ? "below" : m == dl ? "right" : "left";
        }
        if (!strcasecmp(where, "above") || !strcasecmp(where, "top")) {
            x = ar.x, y = ar.y - h - gap;
        } else if (!strcasecmp(where, "below") || !strcasecmp(where, "bottom")) {
            x = ar.x, y = ar.y + ar.h + gap;
        } else if (!strcasecmp(where, "left")) {
            x = ar.x - w - gap, y = ar.y;
        } else {
            x = ar.x + ar.w + gap, y = ar.y;
        }
        const char *al = fm_attr_get(p, "align");
        bool vert = !strcasecmp(where, "above") || !strcasecmp(where, "below") || !strcasecmp(where, "top") ||
                    !strcasecmp(where, "bottom");
        if (al && !strcasecmp(al, "center")) {
            if (vert) x = ar.x + (ar.w - w) / 2;
            else y = ar.y + (ar.h - h) / 2;
        } else if (al && !strcasecmp(al, "end")) {
            if (vert) x = ar.x + ar.w - w;
            else y = ar.y + ar.h - h;
        }
        x += s->margin[3] - s->margin[1];
        y += s->margin[0] - s->margin[2];
    }
    if (!sheet) {
        if (x < S.x + 8) x = S.x + 8.0f;
        if (y < S.y + 8) y = S.y + 8.0f;
        if (x + w > S.x + S.w - 8) x = S.x + S.w - 8 - w;
        if (y + h > S.y + S.h - 8) y = S.y + S.h - 8 - h;
    }
    fmi_arrange(d, p, (rect){(int)x, (int)y, (int)w, (int)h});
}

static void popovers_rec(fm_doc *d, fm_node *n) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || is_meta(n))
            continue;
        if (is_popover(n) && (n->open || n->a_open > 0.01f))
            place_popover(d, n);
        popovers_rec(d, n->first);
    }
}

void fmi_place_popovers(fm_doc *d) {
    if (d->desktop)
        popovers_rec(d, d->desktop->first);
}

/* ---- transforms ---- */

void fmi_transform(fm_node *n, float s, float dx, float dy) {
    float sc = n->av.init ? n->av.scale : n->st.scale;
    float tx = n->av.init ? n->av.tx : n->st.tx, ty = n->av.init ? n->av.ty : n->st.ty;
    if (is_popover(n)) { /* opening: grow from 96% */
        float o = n->a_open;
        sc *= 0.96f + 0.04f * o;
        ty += (1 - o) * 6;
    }
    float cx = n->box.x + n->box.w / 2.0f, cy = n->box.y + n->box.h / 2.0f;
    float S = s * sc;
    float Dx = s * (cx * (1 - sc) + tx) + dx, Dy = s * (cy * (1 - sc) + ty) + dy;
    n->vscale = S;
    n->vis = (rect){(int)lroundf(n->box.x * S + Dx), (int)lroundf(n->box.y * S + Dy), (int)lroundf(n->box.w * S),
                    (int)lroundf(n->box.h * S)};
    for (fm_node *c = n->first; c; c = c->next) {
        if (c->kind == FM_ELEM && (is_popover(c) || is_meta(c)))
            continue;
        if (c->kind == FM_TEXT) {
            c->vscale = S;
            c->vis = (rect){(int)lroundf(c->box.x * S + Dx), (int)lroundf(c->box.y * S + Dy),
                            (int)lroundf(c->box.w * S), (int)lroundf(c->box.h * S)};
            continue;
        }
        fmi_transform(c, S, Dx, Dy);
    }
}

void fm_layout(fm_doc *d, rect screen) {
    d->screen = screen;
    if (!d->desktop)
        return;
    d->cur_win = -1;
    fmi_sync_live(d);
    fm_style_tree(d);
    orient_lists(d->desktop);
    arrange_desktop(d);
    fmi_place_popovers(d);
}

void fmi_sync_subtree(fm_doc *d, fm_node *n) {
    fm_node *saved = n->next;
    n->next = NULL;
    sync_rec(d, n);
    n->next = saved;
}
