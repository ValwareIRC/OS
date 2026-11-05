#define _GNU_SOURCE
/* The right-click menu of a text field (textedit.h keeps where it opened and what it can do): drawn over
 * everything, it has the pointer and the keys while it is up, and what is chosen is typed into the field as
 * the shortcut would be. */
#include "app.h"
#include "controls.h"
#include "fm/fm.h"
#include "menufill.h"
#include "menu_actions.h"
#include "../lib/json.h"
#include <ctype.h>
#include <math.h>
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
        menu_ctx c;
        memset(&c, 0, sizeof c);
        if (m->text[0]) { /* the words selected: search for them, look them up, ask about them */
            snprintf(c.text, sizeof c.text, "%s", m->text);
            c.text_selected = true;
        }
        menu_context(&c);
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
 * What is in it: the actions the thing right-clicked offers (desktop.c, files.c, the text fields above), what
 * follows from what it is and what is offered everywhere (menu_actions.c). What it is, its shape, layout, place
 * and how one is picked: the design's own <menu for="..."> (menufill.c puts the actions in it), or a ring around
 * the pointer before the design has one. Picking: the pointer (and press, hold, let go over one), the arrow keys
 * and Enter, typing to narrow it down, a letter each (pick="letters"), a flick toward one (pick="flick"); Esc
 * closes it. Before the desktop (the first screens) it is drawn here, plainly. */

static struct {
    bool open, shown; /* the plain one is up (no desktop yet) */
    bool pending;     /* filled: made next frame (never while the design is being walked: a pick may open another) */
    bool live;        /* the design's <menu id="_menu"> is up */
    int sel;
    mf_spec spec;
    char run[MF_MAX][MENU_RUN];
    char typed[64];   /* typed while it is up: what it is narrowed to */
    int ox, oy;       /* where the pointer was when it opened (flicks, press-hold-release) */
    bool moved_far;   /* the pointer went somewhere since (a release there picks) */
    uint64_t opened_at;
} CM;

void ctx_menu_open(int x, int y) {
    bool live = CM.live; /* (the one up goes when the new one is made) */
    memset(&CM, 0, sizeof CM);
    CM.live = live;
    CM.pending = true, CM.sel = -1;
    CM.spec.x = x, CM.spec.y = y;
    CM.ox = A.in.mx, CM.oy = A.in.my;
    snprintf(CM.spec.kind, sizeof CM.spec.kind, "desktop");
    menu_context(NULL);
}

void ctx_menu_for(const char *kind) { snprintf(CM.spec.kind, sizeof CM.spec.kind, "%s", kind); }

void ctx_menu_target(int x, int y, int w, int h) {
    CM.spec.tx = x, CM.spec.ty = y, CM.spec.tw = w, CM.spec.th = h;
}

/* An action's id, by what it does where that is known, else from its name (lower case, dashes). */
static void id_for(const char *act, const char *label, char *out, size_t n) {
    static const struct {
        const char *prefix, *id;
    } known[] = {{"open:", "open"},          {"uninstall?", "uninstall"}, {"uninstall:", "uninstall"},
                 {"min:", "minimize"},       {"show:", "show"},           {"max:", "maximize"},
                 {"close:", "close"},        {"do:redesign", "redesign"}, {"do:open:settings", "settings"},
                 {"why:", "ask"},            {"snapl:", "snap-left"},     {"snapr:", "snap-right"},
                 {"ontop:", "keep-on-top"},  {"closeall:", "close-windows"}, {"appinfo:", "app-info"}};
    for (size_t k = 0; k < sizeof known / sizeof known[0]; k++)
        if (!strncmp(act, known[k].prefix, strlen(known[k].prefix))) {
            snprintf(out, n, "%s", known[k].id);
            return;
        }
    size_t o = 0;
    for (const char *p = label; *p && o + 1 < n; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
            out[o++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        else if (o && out[o - 1] != '-')
            out[o++] = '-';
    }
    while (o && out[o - 1] == '-')
        o--;
    out[o] = 0;
}

bool ctx_menu_action(const char *id, const char *label, const char *act, int flags, const char *keys,
                     const char *icon, const char *desc) {
    char gen[48];
    if (!id || !*id) {
        id_for(act ? act : "", label, gen, sizeof gen);
        id = gen;
    }
    int before = CM.spec.n;
    mf_action *a = mf_add(&CM.spec, id, label, flags);
    if (!a || CM.spec.n == before)
        return a != NULL;
    snprintf(CM.run[CM.spec.n - 1], MENU_RUN, "%s", act ? act : "");
    snprintf(a->keys, sizeof a->keys, "%s", keys ? keys : "");
    snprintf(a->icon, sizeof a->icon, "%s", icon ? icon : "");
    snprintf(a->desc, sizeof a->desc, "%s", desc ? desc : "");
    return true;
}

void ctx_menu_item(const char *label, const char *act, int flags, const char *keys) {
    ctx_menu_action(NULL, label, act, flags, keys, NULL, NULL);
}

void ctx_menu_add(const char *label, const char *act, bool danger) {
    ctx_menu_item(label, act, danger ? CM_DANGER : 0, NULL);
}

void ctx_menu_note(const char *note) { snprintf(CM.spec.note, sizeof CM.spec.note, "%s", note); }

bool ctx_menu_showing(void) { return CM.open || CM.pending || CM.live; }

/* What was chosen goes to whoever filled it. */
static void ctx_route(int i) {
    char act[MENU_RUN];
    snprintf(act, sizeof act, "%s", CM.run[i]);
    LOG("context menu: %s (%s)\n", act[0] ? act : "(nothing)", CM.spec.a[i].id);
    if (!act[0])
        return;
    menu_chosen(CM.spec.a[i].id); /* (counted: what is chosen most comes first where a design asks) */
    if (!strncmp(act, "text:", 5))
        text_menu_take(atoi(act + 5));
    else if (!strncmp(act, "files:", 6))
        files_menu_take(act + 6);
    else if (!menu_do(act))
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

static void drop_from(fm_doc *d, fm_node *m) {
    if (inside(d->hovered, m))
        d->hovered = NULL;
    if (inside(d->pressed, m))
        d->pressed = NULL;
    if (inside(d->focus, m))
        d->focus = NULL;
    fm_remove(m);
    d->changed = true;
}

static void drop_live(void) {
    fm_node *m = live_menu();
    if (m)
        drop_from(A.ui, m);
}

static void make_live(void) {
    drop_live();
    CM.pending = false;
    menu_complete(&CM.spec, CM.run);
    fm_node *m = mf_build(A.ui, &CM.spec);
    CM.live = m != NULL;
    CM.opened_at = A.now;
    if (!m)
        return;
    bool own = mf_template(A.ui, CM.spec.kind) != NULL;
    LOG("context menu: %d things, for %s, %s\n", CM.spec.n, CM.spec.kind, own ? "the design's own" : "a ring");
    static int logged;
    if (logged++ < 3 || !own) /* where its actions are, for tests */
        LOGRECT("context-menu", m->vis);
    if (!own)
        menus_check_styled(); /* the design has none of its own yet: the companion makes them (menus.c) */
}

/* A menu shown open in a preview of a design (preview.c), while the companion changes them: of the kind being
 * changed (the selector names it: menu[for=window]...), an app's if none. */
void ctx_menu_sample_into(fm_doc *d, int x, int y, const char *kind) {
    if (!d || !d->desktop)
        return;
    static typeof(CM) keep;
    keep = CM;
    ctx_menu_open(x, y);
    const char *k = kind && *kind ? kind : "app";
    ctx_menu_for(k);
    char ask[96];
    snprintf(ask, sizeof ask, "Ask %s about this", A.d.ai_name[0] ? A.d.ai_name : "the companion");
    if (!strcmp(k, "window")) {
        ctx_menu_add("Minimize", "min:0", false);
        ctx_menu_add("Maximize", "max:0", false);
        ctx_menu_add("Close", "close:0", false);
    } else if (!strcmp(k, "text")) {
        ctx_menu_item("Cut", "text:1", 0, "Ctrl+X");
        ctx_menu_item("Copy", "text:2", 0, "Ctrl+C");
        ctx_menu_item("Paste", "text:3", 0, "Ctrl+V");
        ctx_menu_item("Select all", "text:4", 0, "Ctrl+A");
    } else if (!strcmp(k, "file")) {
        ctx_menu_add("Open", "files:1:0", false);
        ctx_menu_add("Rename", "files:4:0", false);
        ctx_menu_add("Move to Trash", "files:8:0", true);
    } else if (!strcmp(k, "desktop")) {
        ctx_menu_add("Redesign", "do:redesign", false);
        ctx_menu_add("Settings", "do:open:settings", false);
    } else {
        ctx_menu_add("Open Notes", "open:notes", false);
        ctx_menu_add("Uninstall Notes", "uninstall?notes", false);
    }
    ctx_menu_item(ask, "why::", CM_QUIET, NULL);
    CM.spec.tx = x - 40, CM.spec.ty = y - 40, CM.spec.tw = 80, CM.spec.th = 80;
    menu_complete(&CM.spec, CM.run);
    mf_build(d, &CM.spec);
    CM = keep;
}

/* Its actions, in the order the design put them, however deep (only those not narrowed away). */
static int actions_in(fm_node *n, fm_node **out, int k) {
    for (fm_node *c = n->first; c && k < MF_MAX; c = c->next) {
        if (c->kind != FM_ELEM)
            continue;
        const char *f = fm_attr_get(c, "filtered");
        if (fm_attr_get(c, "what") && !(f && !strcmp(f, "out")))
            out[k++] = c;
        else
            k = actions_in(c, out, k);
    }
    return k;
}

static int index_of(fm_node *n) {
    const char *w = n ? fm_attr_get(n, "what") : NULL;
    return w && !strncmp(w, "menu:", 5) ? atoi(w + 5) : -1;
}

/* A flick: the pointer gone well away from where it opened, toward one action: that one is chosen. */
static int flicked_to(fm_node **items, int n) {
    float dx = (float)(A.in.mx - CM.ox), dy = (float)(A.in.my - CM.oy);
    if (dx * dx + dy * dy < 40 * 40)
        return -1;
    float best = -2;
    int at = -1;
    for (int k = 0; k < n; k++) {
        float cx = items[k]->vis.x + items[k]->vis.w / 2.0f - (float)CM.ox, cy = items[k]->vis.y + items[k]->vis.h / 2.0f - (float)CM.oy;
        float l1 = sqrtf(dx * dx + dy * dy), l2 = sqrtf(cx * cx + cy * cy);
        if (l2 < 1)
            continue;
        float cosang = (dx * cx + dy * cy) / (l1 * l2);
        if (cosang > best)
            best = cosang, at = k;
    }
    return best > 0.7f ? at : -1;
}

/* The keys and the pointer while it is up. A press anywhere else closes it (on a window too, which the design never
 * sees). */
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
    fm_node *items[MF_MAX];
    int n = actions_in(m, items, 0);
    bool letters = mf_picks(m, "letters"), typing = !letters || mf_picks(m, "type");
    bool narrowed = false;
    for (int i = 0; i < A.in.nkeys; i++) {
        int k = A.in.keys[i];
        if (k == K_ESC) {
            if (CM.typed[0]) { /* (first, what was typed) */
                CM.typed[0] = 0;
                narrowed = true;
                continue;
            }
            fm_set_open(A.ui, m, false);
            break;
        } else if ((k == K_DOWN || k == K_UP || k == K_TAB) && n) {
            int d = k == K_UP ? -1 : 1;
            CM.sel = CM.sel < 0 ? (d > 0 ? 0 : n - 1) : (CM.sel + d + n) % n;
        } else if (k == K_ENTER && n) {
            fm_activate(A.ui, items[CM.sel >= 0 && CM.sel < n ? CM.sel : 0]);
            break;
        } else if (k == K_BACKSPACE && CM.typed[0]) {
            size_t l = strlen(CM.typed);
            while (l && (CM.typed[l - 1] & 0xc0) == 0x80)
                l--;
            CM.typed[l ? l - 1 : 0] = 0;
            narrowed = true;
        } else if (k >= 32 && k < 0x110000 && k != 127) {
            if (letters && !CM.typed[0] && k < 128 && isalpha(k)) { /* its letter picks it */
                for (int j = 0; j < n; j++) {
                    int ai = index_of(items[j]);
                    if (ai >= 0 && CM.spec.a[ai].letter == (char)tolower(k)) {
                        fm_activate(A.ui, items[j]);
                        return;
                    }
                }
            }
            if (typing && strlen(CM.typed) + 4 < sizeof CM.typed) {
                char u[5] = {0};
                if (k < 0x80) u[0] = (char)k;
                else if (k < 0x800) u[0] = (char)(0xc0 | k >> 6), u[1] = (char)(0x80 | (k & 63));
                else if (k < 0x10000) u[0] = (char)(0xe0 | k >> 12), u[1] = (char)(0x80 | ((k >> 6) & 63)), u[2] = (char)(0x80 | (k & 63));
                else u[0] = (char)(0xf0 | k >> 18), u[1] = (char)(0x80 | ((k >> 12) & 63)), u[2] = (char)(0x80 | ((k >> 6) & 63)), u[3] = (char)(0x80 | (k & 63));
                strcat(CM.typed, u);
                narrowed = true;
            }
        }
    }
    A.in.nkeys = 0;
    if (narrowed) {
        int left = mf_filter(A.ui, m, &CM.spec, CM.typed);
        CM.sel = left ? 0 : -1;
        LOG("context menu: narrowed to \"%s\" (%d)\n", CM.typed, left);
        return;
    }
    if (A.in.moved) {
        int dx = A.in.mx - CM.ox, dy = A.in.my - CM.oy;
        if (dx * dx + dy * dy > 8 * 8)
            CM.moved_far = true;
        int f = mf_picks(m, "flick") ? flicked_to(items, n) : -1;
        CM.sel = f; /* (the pointer chooses again: a flick, or what it is over) */
    }
    /* press, hold, let go over an action: that one (a right-click that only clicked leaves it open) */
    if (A.in.right_clicked && CM.moved_far && A.now - CM.opened_at > 120) {
        for (int k = 0; k < n; k++)
            if (pt_in(items[k]->vis, A.in.mx, A.in.my)) {
                fm_activate(A.ui, items[k]);
                return;
            }
        if (CM.sel >= 0 && CM.sel < n) {
            fm_activate(A.ui, items[CM.sel]);
            return;
        }
    }
    /* a flick, then a click anywhere: the one flicked to */
    if (A.in.pressed && CM.sel >= 0 && CM.sel < n && mf_picks(m, "flick") &&
        !fm_hit_layer(A.ui, 2, A.in.mx, A.in.my)) {
        fm_activate(A.ui, items[CM.sel]);
        A.in.pressed = A.in.clicked = false;
        return;
    }
    if ((A.in.pressed || (A.in.right_clicked && A.now - CM.opened_at > 120 && !CM.moved_far)) &&
        !fm_hit_layer(A.ui, 2, A.in.mx, A.in.my))
        fm_set_open(A.ui, m, false);
}

/* After the design has taken the pointer this frame: the one the keys or a flick chose looks chosen (as the
 * pointer's would: :hover). */
void ctx_menu_after_input(void) {
    if (!CM.live || CM.sel < 0)
        return;
    fm_node *m = live_menu();
    fm_node *items[MF_MAX];
    int n = m ? actions_in(m, items, 0) : 0;
    for (int k = 0; k < n; k++)
        items[k]->hover = k == CM.sel;
}

void ctx_menu_take(int i) {
    if (i < 0 || i >= CM.spec.n)
        return;
    fm_node *m = live_menu();
    if (m && m->open)
        fm_set_open(A.ui, m, false);
    ctx_route(i);
}

/* ---- plainly, before the desktop ---- */

static rect ctx_rect(font *f, int *row) {
    *row = font_line_height(f) + 14;
    int w = 240;
    for (int i = 0; i < CM.spec.n; i++) {
        int tw = text_width(f, CM.spec.a[i].label) + 48 +
                 (CM.spec.a[i].keys[0] ? text_width(f, CM.spec.a[i].keys) + 24 : 0);
        if (tw > w)
            w = tw;
    }
    int note_h = CM.spec.note[0] ? font_line_height(f) + 10 : 0;
    int h = CM.spec.n * *row + 12 + note_h;
    int x = CM.spec.x + w > A.W - 8 ? A.W - 8 - w : CM.spec.x, y = CM.spec.y + h > A.H - 8 ? CM.spec.y - h : CM.spec.y;
    return (rect){x < 8 ? 8 : x, y < 8 ? 8 : y, w, h};
}

void ctx_menu_input(void) {
    menus_tick(); /* (the companion making a design's menus: its answer) */
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
    int chosen = -1, n = CM.spec.n;
    for (int i = 0; i < A.in.nkeys && CM.open; i++) {
        int k = A.in.keys[i];
        if (k == K_ESC)
            CM.open = false;
        else if ((k == K_DOWN || k == K_UP) && n)
            CM.sel = CM.sel < 0 ? (k == K_DOWN ? 0 : n - 1) : (CM.sel + (k == K_DOWN ? 1 : -1) + n) % n;
        else if (k == K_ENTER && CM.sel >= 0 && !(CM.spec.a[CM.sel].flags & MF_OFF))
            chosen = CM.sel;
    }
    int at = pt_in(r, A.in.mx, A.in.my) ? (A.in.my - r.y - 6) / row : -1;
    if (at >= n)
        at = -1;
    if (A.in.moved && pt_in(r, A.in.mx, A.in.my))
        CM.sel = at;
    if (chosen < 0 && A.in.clicked && at >= 0 && !(CM.spec.a[at].flags & MF_OFF))
        chosen = at;
    else if (chosen < 0 && (A.in.pressed || A.in.right_clicked) && !pt_in(r, A.in.mx, A.in.my))
        CM.open = false;
    /* while it is up, the pointer and the keys are the menu's */
    A.in.pressed = A.in.clicked = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.nkeys = 0;
    if (chosen >= 0) {
        CM.open = false;
        ctx_route(chosen);
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
        for (int i = 0; i < CM.spec.n; i++) {
            char nm[40];
            snprintf(nm, sizeof nm, "context-menu-%d", i);
            LOGRECT(nm, ((rect){r.x + 6, r.y + 6 + i * row, r.w - 12, row}));
        }
    }
    float rad = A.d.radius * 0.5f + 4;
    ctl_panel(r, rad > 14 ? 14 : rad, 1);
    palette p = pal_on(pal_sample(rect_inset(r, 6)), 1);
    for (int i = 0; i < CM.spec.n; i++) {
        const mf_action *a = &CM.spec.a[i];
        rect it = {r.x + 6, r.y + 6 + i * row, r.w - 12, row};
        bool off = a->flags & MF_OFF;
        if (i == CM.sel && !off)
            fill_round(A.frame, it, rad > 8 ? 8 : rad * 0.6f, color_alpha(p.ink, 0.1f));
        int ty = it.y + (row - font_line_height(f)) / 2;
        color ink = a->flags & MF_DANGER ? RGBA(0xe0, 0x4b, 0x4b, 255) : p.ink;
        if (off || (a->flags & MF_QUIET))
            ink = color_alpha(ink, off ? 0.38f : 0.7f);
        draw_text(A.frame, f, it.x + 12, ty, a->label, ink);
        if (a->keys[0])
            draw_text(A.frame, f, it.x + it.w - 12 - text_width(f, a->keys), ty, a->keys, p.muted);
    }
    if (CM.spec.note[0])
        draw_text(A.frame, f, r.x + 18, r.y + 6 + CM.spec.n * row + 4, CM.spec.note, p.muted);
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
