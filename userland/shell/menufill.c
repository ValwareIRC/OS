/* Right-click menus, made: the actions of what was clicked and those offered everywhere, put into the design's own
 * <menu for="..."> for that kind. What a menu is (its shape, layout, where it appears, how one is picked) is the
 * design's; this only places the actions, by the rules:
 *   - an element with do="ID" is that action (its name, unless it has words of its own), or goes where the
 *     thing clicked doesn't offer it;
 *   - <acts group="clipboard"/> holds a category's actions, <acts take="copy paste pictures"/> any mix of actions
 *     and categories, <acts/> the rest; @global, @purpose and @frequent are categories too (what is offered
 *     everywhere, what this thing offers, what they choose most);
 *   - each action appears once: in its priority category if that is in the menu (a category's prefer=, or the
 *     person's "priority" in menus.conf), else in the first place written that takes it, else with the rest;
 *   - nothing offered goes missing: what no place takes goes in an <acts class="rest">.
 * Categories: the system's (below), the person's and the companion's (menus.conf: menu_actions.c), and the
 * design's own (<category name="..." take="..." prefer="..."/> in its menu). Before a design has a menu of its
 * own, the actions sit in a ring around the pointer, what is offered everywhere in a small row under it.
 * No part of the desktop program: the build machine's picture tool makes menus with it too (tests/fm_shot.c). */
#define _GNU_SOURCE
#include "menufill.h"
#include "../lib/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const mf_category builtin[] = {
    {"clipboard", "cut copy paste copy-image copy-link copy-path copy-text copy-message", "", false},
    {"editing", "undo redo select-all", "", false},
    {"window", "minimize show maximize snap-left snap-right keep-on-top close", "", false},
    {"app", "open new-window close-windows app-info uninstall", "", false},
    {"desktop", "change-wallpaper new-note paste-here redesign settings", "", false},
    {"pictures", "open-picture copy-image save-image set-as-wallpaper", "", false},
    {"links", "open-link copy-link", "", false},
    {"files", "open open-with quick-look open-in-notes show-in-folder rename copy cut paste copy-path compress "
              "properties move-to-trash put-back erase-for-good empty-trash new-folder select-all",
     "", false},
    {"sorting", "sort-by-name sort-by-size sort-by-date sort-by-kind as-list as-icons", "", false},
    {"text", "search-for-it look-it-up", "", false},
    {"system", "take-screenshot open-terminal settings", "", true},
    {"ask", "ask ask-about-it", "", false},
};
#define NBUILTIN (sizeof builtin / sizeof builtin[0])

const char *mf_builtin_categories(void) {
    static char out[1600];
    if (!out[0]) {
        size_t o = 0;
        for (size_t i = 0; i < NBUILTIN && o + 200 < sizeof out; i++)
            o += (size_t)snprintf(out + o, sizeof out - o, "%s%s%s: %s", i ? "; " : "", builtin[i].name,
                                  builtin[i].global ? " (everywhere)" : "", builtin[i].take);
    }
    return out;
}

mf_action *mf_add(mf_spec *s, const char *id, const char *label, int flags) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->a[i].id, id)) /* (offered twice, by the thing and everywhere: once) */
            return &s->a[i];
    if (s->n == MF_MAX)
        return NULL;
    mf_action *a = &s->a[s->n++];
    memset(a, 0, sizeof *a);
    snprintf(a->id, sizeof a->id, "%s", id);
    snprintf(a->label, sizeof a->label, "%s", label);
    a->flags = flags;
    return a;
}

bool mf_has(const mf_spec *s, const char *id) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->a[i].id, id))
            return true;
    return false;
}

/* ---- categories ---- */

/* the design's own, read from the menu being made */
static mf_category dcats[MF_CATS];
static int ndcats;

static bool word_in(const char *list, const char *w) {
    size_t wl = strlen(w);
    for (const char *p = list; p && (p = strcasestr(p, w)); p += wl)
        if ((p == list || p[-1] == ' ' || p[-1] == ',') && (!p[wl] || p[wl] == ' ' || p[wl] == ','))
            return true;
    return false;
}

static const mf_category *cat_named(const mf_spec *s, const char *name) {
    for (int i = 0; i < ndcats; i++) /* (the design's first, then theirs, then the system's: the nearest wins) */
        if (!strcasecmp(dcats[i].name, name))
            return &dcats[i];
    for (int i = 0; i < s->ncats; i++)
        if (!strcasecmp(s->cats[i].name, name))
            return &s->cats[i];
    for (size_t i = 0; i < NBUILTIN; i++)
        if (!strcasecmp(builtin[i].name, name))
            return &builtin[i];
    return NULL;
}

static int most_used_rank(const mf_spec *s, int i) {
    if (s->a[i].uses <= 0)
        return 99;
    int rank = 0;
    for (int k = 0; k < s->n; k++)
        if (k != i && s->a[k].uses > s->a[i].uses)
            rank++;
    return rank;
}

/* Does `want` (ids, categories, @global @purpose @frequent @all) take action i? */
static bool takes(const mf_spec *s, int i, const char *want, int depth) {
    const mf_action *a = &s->a[i];
    char w[420];
    snprintf(w, sizeof w, "%s", want);
    for (char *save = NULL, *t = strtok_r(w, " ,", &save); t; t = strtok_r(NULL, " ,", &save)) {
        if (!strcasecmp(t, a->id) || !strcasecmp(t, "@all"))
            return true;
        if (!strcasecmp(t, "@global") && (a->flags & MF_GLOBAL))
            return true;
        if (!strcasecmp(t, "@purpose") && !(a->flags & MF_GLOBAL))
            return true;
        if (!strcasecmp(t, "@frequent") && most_used_rank(s, i) < 4)
            return true;
        const mf_category *c = depth < 4 ? cat_named(s, t) : NULL;
        if (c && takes(s, i, c->take, depth + 1))
            return true;
    }
    return false;
}

void mf_groups_of(const mf_spec *s, int i, char *out, size_t n) {
    size_t o = 0;
    out[0] = 0;
    const mf_category *all[NBUILTIN + 2 * MF_CATS];
    int na = 0;
    for (size_t k = 0; k < NBUILTIN; k++)
        all[na++] = &builtin[k];
    for (int k = 0; k < s->ncats; k++)
        all[na++] = &s->cats[k];
    for (int k = 0; k < ndcats; k++)
        all[na++] = &dcats[k];
    for (int k = 0; k < na; k++)
        if (takes(s, i, all[k]->take, 1) && !word_in(out, all[k]->name) && o + strlen(all[k]->name) + 2 < n)
            o += (size_t)snprintf(out + o, n - o, "%s%s", o ? " " : "", all[k]->name);
}

/* The one category where action i belongs first: the design's prefer=, else theirs (menus.conf), or NULL. */
static const char *priority_of(const mf_spec *s, int i) {
    for (int k = 0; k < ndcats; k++)
        if (word_in(dcats[k].prefer, s->a[i].id))
            return dcats[k].name;
    for (int k = 0; k < s->ncats; k++)
        if (word_in(s->cats[k].prefer, s->a[i].id))
            return s->cats[k].name;
    return NULL;
}

/* ---- building ---- */

static void xml_text(sbuf *b, const char *s) {
    for (; *s; s++) {
        if (*s == '<') sb_add(b, "&lt;");
        else if (*s == '>') sb_add(b, "&gt;");
        else if (*s == '&') sb_add(b, "&amp;");
        else if (*s == '"') sb_add(b, "&quot;");
        else sb_addn(b, s, 1);
    }
}

static void classes_of(const mf_action *a, char *cls, size_t n) {
    snprintf(cls, n, "%s%s%s%s%s", a->flags & MF_DANGER ? " danger" : "", a->flags & MF_OFF ? " off" : "",
             a->flags & MF_ON ? " on" : "", a->flags & MF_QUIET ? " quiet" : "", a->flags & MF_GLOBAL ? " global" : "");
}

/* What a placed action carries, for a design to use: do, group, what (choosing it), letter, title (what it does,
 * or why it can't be done now: a tooltip, and what a screen reader says), icon, uses. */
static bool want_tips;   /* the design's menu has tips="true": each action's line shows under the pointer */
static fm_doc *build_doc; /* (the document being built in) */

static void action_attrs(fm_node *n, const mf_spec *s, int i) {
    const mf_action *a = &s->a[i];
    char buf[220];
    fm_attr_set(n, "do", a->id);
    if (a->groups[0])
        fm_attr_set(n, "group", a->groups);
    char cls[64];
    classes_of(a, cls, sizeof cls);
    if (cls[0]) {
        const char *had = fm_attr_get(n, "class");
        snprintf(buf, sizeof buf, "%s%s%s", had && *had ? had : "", had && *had ? " " : "", cls + 1);
        fm_attr_set(n, "class", buf);
    }
    if (!(a->flags & MF_OFF)) {
        snprintf(buf, sizeof buf, "menu:%d", i);
        fm_attr_set(n, "what", buf);
    }
    if (a->letter) {
        snprintf(buf, sizeof buf, "%c", a->letter);
        fm_attr_set(n, "letter", buf);
    }
    const char *tip = (a->flags & MF_OFF) && a->why_off[0] ? a->why_off : a->desc;
    if (tip[0] && !fm_attr_get(n, "title"))
        fm_attr_set(n, "title", tip);
    if (a->icon[0] && !fm_attr_get(n, "icon"))
        fm_attr_set(n, "icon", a->icon);
    snprintf(buf, sizeof buf, "%d", a->uses);
    fm_attr_set(n, "uses", buf);
    if (want_tips && tip[0] && build_doc) {
        sbuf b;
        sb_init(&b);
        sb_add(&b, "<tip>");
        xml_text(&b, tip);
        sb_add(&b, "</tip>");
        fm_parse_fragment(build_doc, n, b.p);
        sb_free(&b);
    }
}

static void action_markup(sbuf *b, const mf_spec *s, int i) {
    const mf_action *a = &s->a[i];
    sb_add(b, "<button>");
    if (a->keys[0]) {
        sb_add(b, "<text>");
        xml_text(b, a->label);
        sb_add(b, "</text><spacer/><text class=\"keys\">");
        xml_text(b, a->keys);
        sb_add(b, "</text>");
    } else {
        xml_text(b, a->label);
    }
    sb_add(b, "</button>");
}

static bool has_words(const fm_node *n) {
    for (const fm_node *k = n->first; k; k = k->next)
        if (k->kind == FM_ELEM || (k->kind == FM_TEXT && k->text && k->text[strspn(k->text, " \t\r\n")]))
            return true;
    return false;
}

fm_node *mf_template(fm_doc *d, const char *kind) {
    fm_node *any = NULL;
    for (fm_node *c = d && d->desktop ? d->desktop->first : NULL; c; c = c->next) {
        if (c->kind != FM_ELEM || strcmp(c->tag, "menu") || c->generated || !strcmp(c->id, "_menu"))
            continue;
        const char *f = fm_attr_get(c, "for");
        if (!f)
            continue;
        if (word_in(f, kind))
            return c;
        if (!any && (word_in(f, "all") || word_in(f, "any")))
            any = c;
    }
    return any;
}

/* the design's categories in it (read, then taken out: they are words, not things to draw), and its places */
static int collect(fm_node *n, fm_node **acts, int k, int max, fm_node **dos, int *nd, int maxd) {
    for (fm_node *c = n->first, *next; c; c = next) {
        next = c->next;
        if (c->kind != FM_ELEM)
            continue;
        if (!strcmp(c->tag, "category")) {
            const char *nm = fm_attr_get(c, "name"), *tk = fm_attr_get(c, "take"), *pf = fm_attr_get(c, "prefer");
            if (nm && ndcats < MF_CATS) {
                mf_category *dc = &dcats[ndcats++];
                memset(dc, 0, sizeof *dc);
                snprintf(dc->name, sizeof dc->name, "%s", nm);
                snprintf(dc->take, sizeof dc->take, "%s", tk ? tk : "");
                snprintf(dc->prefer, sizeof dc->prefer, "%s", pf ? pf : "");
            }
            fm_remove(c);
            continue;
        }
        if (!strcmp(c->tag, "acts")) {
            if (k < max)
                acts[k++] = c;
        } else if (fm_attr_get(c, "do") && *nd < maxd) {
            dos[(*nd)++] = c;
            continue; /* (an action's own element: what is inside it is its own) */
        }
        k = collect(c, acts, k, max, dos, nd, maxd);
    }
    return k;
}

static const char *want_of(fm_node *acts) {
    const char *g = fm_attr_get(acts, "group"), *t = fm_attr_get(acts, "take");
    return g && *g ? g : t && *t ? t : NULL;
}

/* Is category `cat` named by this place (directly)? */
static bool names_cat(fm_node *acts, const char *cat) {
    const char *w = want_of(acts);
    return w && word_in(w, cat);
}

/* most chosen first, else as they came (a few at most: sorted by insertion) */
static void sort_by_use(int *order, int n, const mf_spec *s) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && s->a[order[j]].uses > s->a[order[j - 1]].uses; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
}

/* letters: each action the first letter of its name not already taken, else any of its letters */
static void give_letters(mf_spec *s) {
    bool taken[26] = {false};
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < s->n; i++) {
            if (s->a[i].letter)
                continue;
            for (const char *p = s->a[i].label; *p; p++) {
                if (!isalpha((unsigned char)*p))
                    continue;
                if (pass == 0 && p != s->a[i].label && p[-1] != ' ')
                    continue; /* (first, the start of a word) */
                int c = tolower((unsigned char)*p) - 'a';
                if (c >= 0 && c < 26 && !taken[c]) {
                    taken[c] = true;
                    s->a[i].letter = (char)('a' + c);
                    break;
                }
            }
        }
}

fm_node *mf_build(fm_doc *d, mf_spec *s) {
    if (!d || !d->desktop)
        return NULL;
    ndcats = 0;
    fm_node *tpl = mf_template(d, s->kind);
    char *markup = NULL;
    if (tpl) {
        markup = fm_serialize_node(d, tpl);
    } else { /* before the design has a menu of its own: a ring, what is offered everywhere in a row under it */
        sbuf b;
        sb_init(&b);
        sb_printf(&b, "<menu for=\"%s\" placement=\"around\" pick=\"flick type\"><acts take=\"@purpose\"/>"
                      "<acts class=\"global\" take=\"@global\"/></menu>",
                  s->kind);
        markup = b.p;
    }
    fm_node *m = markup ? fm_parse_fragment(d, d->desktop, markup) : NULL;
    free(markup);
    if (!m)
        return NULL;
    m->generated = true; /* (the system's copy, never part of the design kept) */
    fm_attr_set(m, "id", "_menu");
    fm_attr_set(m, "for", s->kind);
    if (!fm_attr_get(m, "placement") && !fm_attr_get(m, "place"))
        fm_attr_set(m, "placement", "pointer");
    char at[64];
    snprintf(at, sizeof at, "%d %d", s->x, s->y);
    fm_attr_set(m, "at", at);
    if (s->tw > 0) {
        snprintf(at, sizeof at, "%d %d %d %d", s->tx, s->ty, s->tw, s->th);
        fm_attr_set(m, "target", at);
    }
    const char *tips = fm_attr_get(m, "tips");
    want_tips = tips && !strcasecmp(tips, "true");
    build_doc = d;
    fm_node *acts[24], *dos[MF_MAX];
    int nd = 0;
    int nacts = collect(m, acts, 0, 24, dos, &nd, MF_MAX);
    give_letters(s);
    for (int i = 0; i < s->n; i++)
        mf_groups_of(s, i, s->a[i].groups, sizeof s->a[i].groups);
    bool used[MF_MAX] = {false};
    /* 1. where the design put an action itself */
    for (int k = 0; k < nd; k++) {
        const char *want = fm_attr_get(dos[k], "do");
        int at_i = -1;
        for (int i = 0; i < s->n && at_i < 0; i++)
            if (!used[i] && !strcasecmp(s->a[i].id, want))
                at_i = i;
        if (at_i < 0) {
            fm_remove(dos[k]); /* (not offered here) */
            continue;
        }
        used[at_i] = true;
        action_attrs(dos[k], s, at_i);
        if (!has_words(dos[k])) {
            sbuf b;
            sb_init(&b);
            xml_text(&b, s->a[at_i].label);
            fm_parse_fragment(d, dos[k], b.p ? b.p : "");
            sb_free(&b);
        }
    }
    /* 2. each other action's one place: its priority category's, else the first place written that takes it,
     * else the first plain <acts/> */
    int home[MF_MAX];
    fm_node *plain = NULL;
    for (int k = 0; k < nacts && !plain; k++)
        if (!want_of(acts[k]))
            plain = acts[k];
    for (int i = 0; i < s->n; i++) {
        home[i] = -1;
        if (used[i])
            continue;
        const char *pri = priority_of(s, i);
        for (int k = 0; k < nacts && pri && home[i] < 0; k++)
            if (names_cat(acts[k], pri) && takes(s, i, want_of(acts[k]), 0))
                home[i] = k;
        for (int k = 0; k < nacts && home[i] < 0; k++)
            if (want_of(acts[k]) && takes(s, i, want_of(acts[k]), 0))
                home[i] = k;
        for (int k = 0; k < nacts && home[i] < 0; k++)
            if (acts[k] == plain)
                home[i] = k;
    }
    for (int k = 0; k < nacts; k++) {
        int order[MF_MAX], no = 0;
        for (int i = 0; i < s->n; i++)
            if (home[i] == k)
                order[no++] = i;
        const char *ord = fm_attr_get(acts[k], "order");
        if (ord && !strcasecmp(ord, "use"))
            sort_by_use(order, no, s);
        if (!no) {
            fm_attr_set(acts[k], "empty", "true"); /* (nothing of its kind here: a design may hide it) */
            continue;
        }
        sbuf b;
        sb_init(&b);
        for (int j = 0; j < no; j++)
            action_markup(&b, s, order[j]);
        fm_node *first = fm_parse_fragment(d, acts[k], b.p ? b.p : "");
        sb_free(&b);
        int j = 0;
        for (fm_node *c = first; c && j < no; c = c->next)
            if (c->kind == FM_ELEM) {
                action_attrs(c, s, order[j]);
                used[order[j++]] = true;
            }
    }
    /* 3. nothing it offers goes missing */
    int rest[MF_MAX], nr = 0;
    for (int i = 0; i < s->n; i++)
        if (!used[i])
            rest[nr++] = i;
    if (nr) {
        fm_node *r = fm_parse_fragment(d, m, "<acts class=\"rest\"/>");
        sbuf b;
        sb_init(&b);
        for (int j = 0; j < nr; j++)
            action_markup(&b, s, rest[j]);
        fm_node *first = r ? fm_parse_fragment(d, r, b.p ? b.p : "") : NULL;
        sb_free(&b);
        int j = 0;
        for (fm_node *c = first; c && j < nr; c = c->next)
            if (c->kind == FM_ELEM)
                action_attrs(c, s, rest[j++]);
    }
    if (s->note[0]) {
        sbuf b;
        sb_init(&b);
        sb_add(&b, "<text class=\"note\">");
        xml_text(&b, s->note);
        sb_add(&b, "</text>");
        fm_parse_fragment(d, m, b.p);
        sb_free(&b);
    }
    fm_set_open(d, m, true);
    d->changed = true;
    return m;
}

/* ---- picking ---- */

bool mf_picks(const fm_node *menu, const char *how) {
    const char *p = menu ? fm_attr_get(menu, "pick") : NULL;
    return p && word_in(p, how);
}

static void filter_walk(fm_doc *d, fm_node *n, const mf_spec *s, const char *typed, int *matches) {
    for (fm_node *c = n->first; c; c = c->next) {
        if (c->kind != FM_ELEM)
            continue;
        const char *what = fm_attr_get(c, "what"), *id = fm_attr_get(c, "do");
        if (what && id && !strncmp(what, "menu:", 5)) {
            int i = atoi(what + 5);
            bool ok = !typed[0] || (i >= 0 && i < s->n && (strcasestr(s->a[i].label, typed) || strcasestr(s->a[i].id, typed)));
            fm_attr_set(c, "filtered", ok ? "in" : "out");
            *matches += ok;
            continue;
        }
        if (!strcmp(c->tag, "filter")) { /* the design's place for the words typed */
            while (c->first)
                fm_remove(c->first);
            sbuf b;
            sb_init(&b);
            xml_text(&b, typed);
            if (b.p)
                fm_parse_fragment(d, c, b.p);
            sb_free(&b);
            fm_attr_set(c, "empty", typed[0] ? "false" : "true");
            continue;
        }
        filter_walk(d, c, s, typed, matches);
    }
}

int mf_filter(fm_doc *d, fm_node *menu, const mf_spec *s, const char *typed) {
    int matches = 0;
    if (menu && d)
        filter_walk(d, menu, s, typed, &matches);
    if (d)
        d->changed = true;
    return matches;
}
