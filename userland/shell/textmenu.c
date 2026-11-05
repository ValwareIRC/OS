#define _GNU_SOURCE
/* The right-click menu of a text field (textedit.h keeps where it opened and what it can do): drawn over
 * everything, it has the pointer and the keys while it is up, and what is chosen is typed into the field as
 * the shortcut would be. */
#include "app.h"
#include "controls.h"
#include "fm/fm.h"
#include "../lib/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- the right-click menu of a text field ---- */

static const struct {
    const char *label, *keys;
    int key, mod;
} text_items[] = {
    {"Undo", "Ctrl+Z", 'z', KM_CTRL},   {"Cut", "Ctrl+X", 'x', KM_CTRL},
    {"Copy", "Ctrl+C", 'c', KM_CTRL},   {"Paste", "Ctrl+V", K_PASTE, 0},
    {"Select all", "Ctrl+A", 'a', KM_CTRL},
};
#define NTEXT_ITEMS 5

static bool text_item_on(const te_menu *m, int i) {
    switch (i) {
    case 0: return m->undo;
    case 1: case 2: return m->selection && !m->secret;
    case 3: return clipboard_text()[0] != 0;
    default: return true;
    }
}

static rect text_menu_rect(const te_menu *m, font *f, int *row) {
    *row = font_line_height(f) + 14;
    int w = 220, h = NTEXT_ITEMS * *row + 12;
    int x = m->x + w > A.W - 8 ? A.W - 8 - w : m->x, y = m->y + h > A.H - 8 ? m->y - h : m->y;
    return (rect){x, y < 8 ? 8 : y, w, h};
}

/* A pick from the design's own menu (the desktop's: ctx_menu below), typed into the field next frame as the
 * shortcut would be. */
static int text_key, text_mod;

void text_menu_take(int i) {
    if (i >= 0 && i < NTEXT_ITEMS)
        text_key = text_items[i].key, text_mod = text_items[i].mod;
}

void ui_text_menu_input(void) {
    if (text_key && A.in.nkeys < (int)(sizeof A.in.keys / sizeof A.in.keys[0])) {
        A.in.keys[A.in.nkeys] = text_key, A.in.keymod[A.in.nkeys++] = (uint8_t)text_mod;
        text_key = 0;
    }
    te_menu *m = te_menu_state();
    if (!m->open)
        return;
    if (A.screen == SCR_DESKTOP && A.ui && A.ui->desktop) { /* on the desktop: the design's own menu, for="text" */
        ctx_menu_open(m->x, m->y);
        ctx_menu_for("text");
        for (int i = 0; i < NTEXT_ITEMS; i++) {
            char act[16];
            snprintf(act, sizeof act, "text:%d", i);
            ctx_menu_item(text_items[i].label, act, text_item_on(m, i) ? 0 : CM_OFF, text_items[i].keys);
        }
        m->open = false;
        return;
    }
    font *f = fm_host_font(A.d.font_body, A.d.body_px - 1);
    int row;
    rect r = text_menu_rect(m, f, &row);
    int key = 0, mod = 0;
    for (int i = 0; i < A.in.nkeys && m->open; i++) { /* the arrows choose, Enter takes it, Esc closes */
        int k = A.in.keys[i];
        if (k == K_ESC) {
            m->open = false;
        } else if (k == K_DOWN || k == K_UP) {
            int d = k == K_DOWN ? 1 : -1, s = m->sel;
            for (int n = 0; n < NTEXT_ITEMS; n++) {
                s = s < 0 ? (d > 0 ? 0 : NTEXT_ITEMS - 1) : (s + d + NTEXT_ITEMS) % NTEXT_ITEMS;
                if (text_item_on(m, s))
                    break;
            }
            m->sel = s;
        } else if (k == K_ENTER && m->sel >= 0 && text_item_on(m, m->sel)) {
            key = text_items[m->sel].key, mod = text_items[m->sel].mod;
            m->open = false;
        }
    }
    if (pt_in(r, A.in.mx, A.in.my) && A.in.moved) {
        int i = (A.in.my - r.y - 6) / row;
        m->sel = i >= 0 && i < NTEXT_ITEMS && text_item_on(m, i) ? i : -1;
    }
    if (key) {
    } else if (A.in.clicked && pt_in(r, A.in.mx, A.in.my)) {
        int i = (A.in.my - r.y - 6) / row;
        if (i >= 0 && i < NTEXT_ITEMS && text_item_on(m, i)) {
            key = text_items[i].key, mod = text_items[i].mod;
            m->open = false;
        }
    } else if ((A.in.pressed || A.in.right_clicked) && !pt_in(r, A.in.mx, A.in.my)) {
        m->open = false;
    }
    /* while it is up, the pointer and the keys are the menu's; what it chose is typed into the field */
    A.in.pressed = A.in.clicked = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.nkeys = 0;
    if (key) {
        A.in.keys[0] = key, A.in.keymod[0] = (uint8_t)mod;
        A.in.nkeys = 1;
    }
}

/* ---- the right-click menu of anything else ----
 * What is in it is the system's: what the thing right-clicked offers, by its real name (desktop.c, files.c, the
 * text fields above). How it looks is the design's: on the desktop it is a <menu> of the design itself, made where
 * they right-clicked (for="app|window|desktop|text|file", what it is for) and styled by the design's own rules
 * (menu, menu button, :hover, :focus, .danger, .off, .on, .quiet, menu[for=...]), as the companion wrote them for
 * it (menus.c asks it to, for a design that has none yet). It was one look for everything, drawn here, like any
 * other system's menu. Before the desktop (the first screens) it still is, plainly. */

#define CTX_MAX 24
static struct {
    bool open, shown; /* the plain one is up (no desktop yet) */
    bool pending;     /* filled: made next frame (never while the design is being walked: a pick may open another) */
    bool live;        /* the design's <menu id="_menu"> is up */
    int x, y, n, sel;
    int tx, ty, tw, th; /* the thing clicked, where it is known (placement="on") */
    char kind[16];
    char label[CTX_MAX][96], keys[CTX_MAX][24];
    char act[CTX_MAX][200];
    int flags[CTX_MAX];
    char note[120];
} CM;

void ctx_menu_open(int x, int y) {
    bool live = CM.live; /* (the one up goes when the new one is made) */
    memset(&CM, 0, sizeof CM);
    CM.live = live;
    CM.pending = true, CM.x = x, CM.y = y, CM.sel = -1;
    snprintf(CM.kind, sizeof CM.kind, "desktop");
}

void ctx_menu_for(const char *kind) { snprintf(CM.kind, sizeof CM.kind, "%s", kind); }

void ctx_menu_target(int x, int y, int w, int h) { CM.tx = x, CM.ty = y, CM.tw = w, CM.th = h; }

void ctx_menu_item(const char *label, const char *act, int flags, const char *keys) {
    if (CM.n == CTX_MAX)
        return;
    snprintf(CM.label[CM.n], sizeof CM.label[0], "%s", label);
    snprintf(CM.act[CM.n], sizeof CM.act[0], "%s", act ? act : "");
    snprintf(CM.keys[CM.n], sizeof CM.keys[0], "%s", keys ? keys : "");
    CM.flags[CM.n++] = flags;
}

void ctx_menu_add(const char *label, const char *act, bool danger) {
    ctx_menu_item(label, act, danger ? CM_DANGER : 0, NULL);
}

void ctx_menu_note(const char *note) { snprintf(CM.note, sizeof CM.note, "%s", note); }

bool ctx_menu_showing(void) { return CM.open || CM.pending || CM.live; }

/* What was chosen goes to whoever filled it. */
static void ctx_route(const char *act) {
    LOG("context menu: %s\n", act[0] ? act : "(nothing)");
    if (!act[0])
        return;
    if (!strncmp(act, "text:", 5))
        text_menu_take(atoi(act + 5));
    else if (!strncmp(act, "files:", 6))
        files_menu_take(act + 6);
    else
        desktop_ctx_chosen(act); /* (it may open the menu again: a question) */
}

/* ---- in the design ---- */

static fm_node *live_menu(void) {
    if (!A.ui || !A.ui->desktop)
        return NULL;
    for (fm_node *c = A.ui->desktop->first; c; c = c->next)
        if (c->kind == FM_ELEM && !strcmp(c->id, "_menu"))
            return c;
    return NULL;
}

static bool inside(const fm_node *n, const fm_node *top) {
    for (; n; n = n->parent)
        if (n == top)
            return true;
    return false;
}

static void drop_live(void) {
    fm_node *m = live_menu();
    if (!m)
        return;
    fm_doc *d = A.ui;
    if (inside(d->hovered, m))
        d->hovered = NULL;
    if (inside(d->pressed, m))
        d->pressed = NULL;
    if (inside(d->focus, m))
        d->focus = NULL;
    fm_remove(m);
    d->changed = true;
}

static void xml_text(sbuf *b, const char *s) {
    for (; *s; s++) {
        if (*s == '<') sb_add(b, "&lt;");
        else if (*s == '>') sb_add(b, "&gt;");
        else if (*s == '&') sb_add(b, "&amp;");
        else if (*s == '"') sb_add(b, "&quot;");
        else sb_addn(b, s, 1);
    }
}

/* The action's id, for a design's do="ID": by what it does, else its name (lower case, dashes). */
static void action_id(int i, char *out, size_t n) {
    static const struct {
        const char *prefix, *id;
    } known[] = {{"open:", "open"},        {"uninstall?", "uninstall"}, {"uninstall:", "uninstall"},
                 {"min:", "minimize"},     {"show:", "show"},           {"max:", "maximize"},
                 {"close:", "close"},      {"do:redesign", "redesign"}, {"do:open:settings", "settings"},
                 {"why:", "ask"}};
    for (size_t k = 0; k < sizeof known / sizeof known[0]; k++)
        if (!strncmp(CM.act[i], known[k].prefix, strlen(known[k].prefix))) {
            snprintf(out, n, "%s", known[k].id);
            return;
        }
    size_t o = 0;
    for (const char *p = CM.label[i]; *p && o + 1 < n; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
            out[o++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        else if (o && out[o - 1] != '-')
            out[o++] = '-';
    }
    while (o && out[o - 1] == '-')
        o--;
    out[o] = 0;
}

/* The system's own categories, by action id: a design may use them (<acts group="clipboard">), regroup them, or
 * ignore them; it may make its own (<category name="..." take="ids or categories"/>). */
static const struct {
    const char *name, *ids;
} builtin_groups[] = {
    {"clipboard", " cut copy paste "},
    {"editing", " undo redo select-all "},
    {"window", " minimize show maximize close "},
    {"app", " open uninstall "},
    {"desktop", " redesign settings "},
    {"files", " open quick-look open-in-notes rename copy cut paste move-to-trash put-back erase-for-good "
              "empty-trash new-folder select-all "},
    {"sorting", " sort-by-name sort-by-size sort-by-date sort-by-kind as-list as-icons "},
    {"ask", " ask "},
};

/* The categories of action i, the system's, space-separated ("clipboard files"). */
static void groups_of(int i, char *out, size_t n) {
    char id[48], pat[52];
    action_id(i, id, sizeof id);
    snprintf(pat, sizeof pat, " %s ", id);
    out[0] = 0;
    size_t o = 0;
    for (size_t g = 0; g < sizeof builtin_groups / sizeof builtin_groups[0]; g++)
        if (strstr(builtin_groups[g].ids, pat) && o + strlen(builtin_groups[g].name) + 2 < n)
            o += (size_t)snprintf(out + o, n - o, "%s%s", o ? " " : "", builtin_groups[g].name);
}

/* A design's own categories, read from its menu: <category name="everyday" take="copy paste rename"/>. */
#define MAX_CATS 16
static struct {
    char name[32], take[200];
} cats[MAX_CATS];
static int ncats;

/* Is action i one that `want` names (ids, the system's categories, the design's own, space-separated)? */
static bool takes(int i, const char *want, int depth) {
    char id[48], groups[160];
    action_id(i, id, sizeof id);
    groups_of(i, groups, sizeof groups);
    char w[256];
    snprintf(w, sizeof w, "%s", want);
    for (char *save = NULL, *t = strtok_r(w, " ,", &save); t; t = strtok_r(NULL, " ,", &save)) {
        if (!strcasecmp(t, id))
            return true;
        char pat[40];
        snprintf(pat, sizeof pat, " %s ", t);
        char gs[164];
        snprintf(gs, sizeof gs, " %s ", groups);
        if (strcasestr(gs, pat))
            return true;
        for (int c = 0; c < ncats && depth < 4; c++)
            if (!strcasecmp(cats[c].name, t) && takes(i, cats[c].take, depth + 1))
                return true;
    }
    return false;
}

static void classes_of(int i, char *cls, size_t n) {
    int fl = CM.flags[i];
    snprintf(cls, n, "%s%s%s%s", fl & CM_DANGER ? " danger" : "", fl & CM_OFF ? " off" : "", fl & CM_ON ? " on" : "",
             fl & CM_QUIET ? " quiet" : "");
}

/* One action as a button, for <acts>. */
static void action_markup(sbuf *b, int i) {
    char cls[48], id[48];
    classes_of(i, cls, sizeof cls);
    action_id(i, id, sizeof id);
    char groups[160];
    groups_of(i, groups, sizeof groups);
    sb_printf(b, "<button do=\"%s\"", id);
    if (groups[0])
        sb_printf(b, " group=\"%s\"", groups);
    if (cls[0])
        sb_printf(b, " class=\"%s\"", cls + 1);
    if (!(CM.flags[i] & CM_OFF)) /* (one that can't be done now does nothing) */
        sb_printf(b, " what=\"menu:%d\"", i);
    sb_add(b, ">");
    if (CM.keys[i][0]) {
        sb_add(b, "<text>");
        xml_text(b, CM.label[i]);
        sb_add(b, "</text><spacer/><text class=\"keys\">");
        xml_text(b, CM.keys[i]);
        sb_add(b, "</text>");
    } else {
        xml_text(b, CM.label[i]);
    }
    sb_add(b, "</button>");
}

/* The design's own menu for this kind: <menu for="app window ..."> (or for="all"), never one the system made. */
static fm_node *template_of(fm_doc *d, const char *kind) {
    fm_node *any = NULL;
    for (fm_node *c = d && d->desktop ? d->desktop->first : NULL; c; c = c->next) {
        if (c->kind != FM_ELEM || strcmp(c->tag, "menu") || c->generated || c->id[0] == '_')
            continue;
        const char *f = fm_attr_get(c, "for");
        if (!f)
            continue;
        size_t kl = strlen(kind);
        for (const char *p = f; (p = strstr(p, kind)); p += kl)
            if ((p == f || p[-1] == ' ' || p[-1] == ',') && (!p[kl] || p[kl] == ' ' || p[kl] == ','))
                return c;
        if (!any && (strstr(f, "all") || strstr(f, "any")))
            any = c;
    }
    return any;
}

/* What the design put where: each element with do="ID" gets that action (its name, if it has no words of its
 * own) or goes, where this thing doesn't offer it; <acts> gets the rest. */
static void fill_dos(fm_doc *d, fm_node *n, bool *used) {
    for (fm_node *c = n->first, *next; c; c = next) {
        next = c->next;
        if (c->kind != FM_ELEM)
            continue;
        const char *want = fm_attr_get(c, "do");
        if (want && *want && strcmp(c->tag, "acts")) {
            int at = -1;
            for (int i = 0; i < CM.n && at < 0; i++) {
                char id[48];
                action_id(i, id, sizeof id);
                if (!used[i] && !strcasecmp(id, want))
                    at = i;
            }
            if (at < 0) {
                fm_remove(c); /* (not offered here) */
                continue;
            }
            used[at] = true;
            char cls[48], what[24];
            classes_of(at, cls, sizeof cls);
            if (cls[0]) {
                const char *had = fm_attr_get(c, "class");
                char all[160];
                snprintf(all, sizeof all, "%s%s%s", had ? had : "", had ? " " : "", cls + 1);
                fm_attr_set(c, "class", all);
            }
            if (!(CM.flags[at] & CM_OFF)) {
                snprintf(what, sizeof what, "menu:%d", at);
                fm_attr_set(c, "what", what);
            }
            bool words = false;
            for (fm_node *k = c->first; k && !words; k = k->next)
                words = k->kind == FM_ELEM || (k->kind == FM_TEXT && k->text && k->text[strspn(k->text, " \t\r\n")]);
            if (!words) {
                sbuf b;
                sb_init(&b);
                xml_text(&b, CM.label[at]);
                fm_parse_fragment(d, c, b.p ? b.p : "");
                sb_free(&b);
            }
            continue;
        }
        fill_dos(d, c, used);
    }
}

/* Every <acts> in it, in the order written, and the design's own <category> definitions (read, then taken out:
 * they are words, not things to draw). */
static int collect(fm_node *n, fm_node **acts, int k, int max) {
    for (fm_node *c = n->first, *next; c; c = next) {
        next = c->next;
        if (c->kind != FM_ELEM)
            continue;
        if (!strcmp(c->tag, "category")) {
            const char *nm = fm_attr_get(c, "name"), *tk = fm_attr_get(c, "take");
            if (nm && tk && ncats < MAX_CATS) {
                snprintf(cats[ncats].name, sizeof cats[0].name, "%s", nm);
                snprintf(cats[ncats].take, sizeof cats[0].take, "%s", tk);
                ncats++;
            }
            fm_remove(c);
            continue;
        }
        if (!strcmp(c->tag, "acts") && k < max)
            acts[k++] = c;
        k = collect(c, acts, k, max);
    }
    return k;
}

/* The menu for what was clicked, in doc `d`: the design's own for this kind, filled; else a ring of its actions
 * around the pointer. Returns it (id "_menu", the system's, never kept with the design), open. */
static fm_node *build_menu(fm_doc *d) {
    fm_node *tpl = template_of(d, CM.kind);
    char *markup = NULL;
    if (tpl) {
        markup = fm_serialize_node(d, tpl);
    } else {
        sbuf b;
        sb_init(&b);
        sb_printf(&b, "<menu for=\"%s\" placement=\"around\"><acts/></menu>", CM.kind);
        markup = b.p;
    }
    fm_node *m = markup ? fm_parse_fragment(d, d->desktop, markup) : NULL;
    free(markup);
    if (!m)
        return NULL;
    m->generated = true; /* (the system's copy, never part of the design kept) */
    fm_attr_set(m, "id", "_menu");
    fm_attr_set(m, "for", CM.kind);
    if (!fm_attr_get(m, "placement") && !fm_attr_get(m, "place"))
        fm_attr_set(m, "placement", "pointer");
    char at[64];
    snprintf(at, sizeof at, "%d %d", CM.x, CM.y);
    fm_attr_set(m, "at", at);
    if (CM.tw > 0) {
        snprintf(at, sizeof at, "%d %d %d %d", CM.tx, CM.ty, CM.tw, CM.th);
        fm_attr_set(m, "target", at);
    }
    bool used[CTX_MAX] = {false};
    ncats = 0;
    fm_node *all[16];
    int nacts = collect(m, all, 0, 16); /* (its own categories first: do= and acts may name them) */
    fill_dos(d, m, used);
    /* <acts group="..."> or take="...": those actions, in their own place; then <acts/> (no group), the rest */
    fm_node *plain = NULL;
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < nacts; k++) {
            const char *g = fm_attr_get(all[k], "group"), *t = fm_attr_get(all[k], "take");
            const char *want = g && *g ? g : t && *t ? t : NULL;
            if ((pass == 0) != (want != NULL))
                continue;
            if (!want && plain)
                continue; /* (the rest go to the first) */
            if (!want)
                plain = all[k];
            sbuf b;
            sb_init(&b);
            for (int i = 0; i < CM.n; i++)
                if (!used[i] && (!want || takes(i, want, 0))) {
                    used[i] = true;
                    action_markup(&b, i);
                }
            if (b.p)
                fm_parse_fragment(d, all[k], b.p);
            else if (want) /* (none of that kind here: the place for them goes too) */
                fm_attr_set(all[k], "empty", "true");
            sb_free(&b);
        }
    bool rest = false;
    for (int i = 0; i < CM.n; i++)
        rest |= !used[i];
    if (rest) { /* (it placed some and had no place for the rest: nothing it offers goes missing) */
        fm_node *acts = fm_parse_fragment(d, m, "<acts class=\"rest\"/>");
        sbuf b;
        sb_init(&b);
        for (int i = 0; i < CM.n; i++)
            if (!used[i])
                action_markup(&b, i);
        if (acts && b.p)
            fm_parse_fragment(d, acts, b.p);
        sb_free(&b);
    }
    if (CM.note[0]) {
        sbuf b;
        sb_init(&b);
        sb_add(&b, "<text class=\"note\">");
        xml_text(&b, CM.note);
        sb_add(&b, "</text>");
        fm_parse_fragment(d, m, b.p);
        sb_free(&b);
    }
    fm_set_open(d, m, true);
    d->changed = true;
    return m;
}

static void make_live(void) {
    drop_live();
    CM.pending = false;
    fm_node *m = build_menu(A.ui);
    CM.live = m != NULL;
    if (!m)
        return;
    LOG("context menu: %d things, for %s, %s\n", CM.n, CM.kind, template_of(A.ui, CM.kind) ? "the design's own" : "a ring");
    if (!template_of(A.ui, CM.kind))
        menus_check_styled(); /* the design has none of its own yet: the companion makes them (menus.c) */
}

/* A menu shown open in a preview of a design (preview.c), while the companion changes them: an app's. */
void ctx_menu_sample_into(fm_doc *d, int x, int y) {
    if (!d || !d->desktop)
        return;
    typeof(CM) keep = CM;
    ctx_menu_open(x, y);
    ctx_menu_for("app");
    ctx_menu_add("Open Notes", "open:notes", false);
    ctx_menu_add("Uninstall Notes", "uninstall?notes", false);
    char ask[96];
    snprintf(ask, sizeof ask, "Ask %s about this", A.d.ai_name[0] ? A.d.ai_name : "the companion");
    ctx_menu_item(ask, "why::", CM_QUIET | CM_SEP, NULL);
    CM.tx = x - 40, CM.ty = y - 40, CM.tw = 80, CM.th = 80;
    build_menu(d);
    CM = keep;
}

/* Its actions, in the order the design put them, however deep. */
static int actions_in(fm_node *n, fm_node **out, int k) {
    for (fm_node *c = n->first; c && k < CTX_MAX; c = c->next) {
        if (c->kind != FM_ELEM)
            continue;
        if (fm_attr_get(c, "what"))
            out[k++] = c;
        else
            k = actions_in(c, out, k);
    }
    return k;
}

/* The arrow keys choose among what can be done; Enter takes it, Esc closes it. The rest of the keys are the
 * menu's while it is up. A press anywhere else closes it (on a window too, which the design never sees). */
static void live_input(void) {
    fm_node *m = live_menu();
    if (!m) {
        CM.live = false;
        return;
    }
    if (!m->open) { /* picked, Esc, a click elsewhere: gone once it has faded */
        if (m->a_open < 0.01f) {
            drop_live();
            CM.live = false;
        }
        return;
    }
    fm_node *items[CTX_MAX];
    int n = actions_in(m, items, 0);
    for (int i = 0; i < A.in.nkeys; i++) {
        int k = A.in.keys[i];
        if (k == K_ESC) {
            fm_set_open(A.ui, m, false);
            break;
        } else if ((k == K_DOWN || k == K_UP) && n) {
            CM.sel = CM.sel < 0 ? (k == K_DOWN ? 0 : n - 1) : (CM.sel + (k == K_DOWN ? 1 : -1) + n) % n;
        } else if (k == K_ENTER && CM.sel >= 0 && CM.sel < n) {
            fm_activate(A.ui, items[CM.sel]);
            break;
        }
    }
    A.in.nkeys = 0;
    if (A.in.moved)
        CM.sel = -1; /* (the pointer chooses again) */
    if ((A.in.pressed || A.in.right_clicked) && !fm_hit_layer(A.ui, 2, A.in.mx, A.in.my))
        fm_set_open(A.ui, m, false);
}

/* After the design has taken the pointer this frame: the one the arrows chose looks chosen (as the pointer's
 * would: :hover), until the pointer moves. */
void ctx_menu_after_input(void) {
    if (!CM.live || CM.sel < 0)
        return;
    fm_node *m = live_menu();
    fm_node *items[CTX_MAX];
    int n = m ? actions_in(m, items, 0) : 0;
    for (int k = 0; k < n; k++)
        items[k]->hover = k == CM.sel;
}

void ctx_menu_take(int i) {
    if (i < 0 || i >= CM.n)
        return;
    char act[200];
    snprintf(act, sizeof act, "%s", CM.act[i]);
    fm_node *m = live_menu();
    if (m && m->open)
        fm_set_open(A.ui, m, false);
    ctx_route(act);
}

/* ---- plainly, before the desktop ---- */

static rect ctx_rect(font *f, int *row) {
    *row = font_line_height(f) + 14;
    int w = 240;
    for (int i = 0; i < CM.n; i++) {
        int tw = text_width(f, CM.label[i]) + 48 + (CM.keys[i][0] ? text_width(f, CM.keys[i]) + 24 : 0);
        if (tw > w)
            w = tw;
    }
    int note_h = CM.note[0] ? font_line_height(f) + 10 : 0;
    int h = CM.n * *row + 12 + note_h;
    int x = CM.x + w > A.W - 8 ? A.W - 8 - w : CM.x, y = CM.y + h > A.H - 8 ? CM.y - h : CM.y;
    return (rect){x < 8 ? 8 : x, y < 8 ? 8 : y, w, h};
}

void ctx_menu_input(void) {
    menus_tick(); /* (the companion writing a design's menus: its answer) */
    if (CM.pending) {
        if (A.screen == SCR_DESKTOP && A.ui && A.ui->desktop) {
            make_live();
        } else {
            CM.pending = false;
            CM.open = true;
        }
    }
    if (CM.live) {
        live_input();
        return;
    }
    if (!CM.open)
        return;
    font *f = fm_host_font(A.d.font_body, A.d.body_px - 1);
    int row;
    rect r = ctx_rect(f, &row);
    int chosen = -1;
    for (int i = 0; i < A.in.nkeys && CM.open; i++) {
        int k = A.in.keys[i];
        if (k == K_ESC)
            CM.open = false;
        else if ((k == K_DOWN || k == K_UP) && CM.n)
            CM.sel = CM.sel < 0 ? (k == K_DOWN ? 0 : CM.n - 1) : (CM.sel + (k == K_DOWN ? 1 : -1) + CM.n) % CM.n;
        else if (k == K_ENTER && CM.sel >= 0 && !(CM.flags[CM.sel] & CM_OFF))
            chosen = CM.sel;
    }
    int at = pt_in(r, A.in.mx, A.in.my) ? (A.in.my - r.y - 6) / row : -1;
    if (at >= CM.n)
        at = -1;
    if (A.in.moved && pt_in(r, A.in.mx, A.in.my))
        CM.sel = at;
    if (chosen < 0 && A.in.clicked && at >= 0 && !(CM.flags[at] & CM_OFF))
        chosen = at;
    else if (chosen < 0 && (A.in.pressed || A.in.right_clicked) && !pt_in(r, A.in.mx, A.in.my))
        CM.open = false;
    /* while it is up, the pointer and the keys are the menu's */
    A.in.pressed = A.in.clicked = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.nkeys = 0;
    if (chosen >= 0) {
        char act[200];
        snprintf(act, sizeof act, "%s", CM.act[chosen]);
        CM.open = false;
        ctx_route(act);
    }
}

void ctx_menu_draw(void) {
    if (!CM.open || CM.live)
        return;
    font *f = fm_host_font(A.d.font_body, A.d.body_px - 1);
    int row;
    rect r = ctx_rect(f, &row);
    if (!CM.shown) {
        CM.shown = true;
        LOGRECT("context-menu", r);
        for (int i = 0; i < CM.n; i++) {
            char nm[40];
            snprintf(nm, sizeof nm, "context-menu-%d", i);
            LOGRECT(nm, ((rect){r.x + 6, r.y + 6 + i * row, r.w - 12, row}));
        }
    }
    float rad = A.d.radius * 0.5f + 4;
    ctl_panel(r, rad > 14 ? 14 : rad, 1);
    palette p = pal_on(pal_sample(rect_inset(r, 6)), 1);
    for (int i = 0; i < CM.n; i++) {
        rect it = {r.x + 6, r.y + 6 + i * row, r.w - 12, row};
        bool off = CM.flags[i] & CM_OFF;
        if (i == CM.sel && !off)
            fill_round(A.frame, it, rad > 8 ? 8 : rad * 0.6f, color_alpha(p.ink, 0.1f));
        int ty = it.y + (row - font_line_height(f)) / 2;
        color ink = CM.flags[i] & CM_DANGER ? RGBA(0xe0, 0x4b, 0x4b, 255) : p.ink;
        if (off || (CM.flags[i] & CM_QUIET))
            ink = color_alpha(ink, off ? 0.38f : 0.7f);
        draw_text(A.frame, f, it.x + 12, ty, CM.label[i], ink);
        if (CM.keys[i][0])
            draw_text(A.frame, f, it.x + it.w - 12 - text_width(f, CM.keys[i]), ty, CM.keys[i], p.muted);
    }
    if (CM.note[0])
        draw_text(A.frame, f, r.x + 18, r.y + 6 + CM.n * row + 4, CM.note, p.muted);
}

void ui_text_menu_draw(void) {
    te_menu *m = te_menu_state();
    if (!m->open)
        return;
    font *f = fm_host_font(A.d.font_body, A.d.body_px - 1);
    int row;
    rect r = text_menu_rect(m, f, &row);
    if (!m->shown) {
        m->shown = true;
        LOGRECT("text-menu", r);
    }
    float rad = A.d.radius * 0.5f + 4;
    ctl_panel(r, rad > 14 ? 14 : rad, 1);
    palette p = pal_on(pal_sample(rect_inset(r, 6)), 1);
    for (int i = 0; i < NTEXT_ITEMS; i++) {
        rect it = {r.x + 6, r.y + 6 + i * row, r.w - 12, row};
        bool on = text_item_on(m, i);
        if (on && i == m->sel)
            fill_round(A.frame, it, rad > 8 ? 8 : rad * 0.6f, color_alpha(p.ink, 0.1f));
        int ty = it.y + (row - font_line_height(f)) / 2;
        color ink = on ? p.ink : color_alpha(p.ink, 0.38f);
        draw_text(A.frame, f, it.x + 12, ty, text_items[i].label, ink);
        int kw = text_width(f, text_items[i].keys);
        draw_text(A.frame, f, it.x + it.w - 12 - kw, ty, text_items[i].keys, color_alpha(p.muted, on ? 1 : 0.5f));
    }
}
