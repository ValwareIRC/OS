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

static void make_live(void) {
    drop_live();
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "<menu id=\"_menu\" for=\"%s\" placement=\"pointer\" at=\"%d %d\">", CM.kind, CM.x, CM.y);
    for (int i = 0; i < CM.n; i++) {
        int fl = CM.flags[i];
        if ((fl & CM_SEP) && i)
            sb_add(&b, "<divider/>");
        char cls[48] = "";
        snprintf(cls, sizeof cls, "%s%s%s%s", fl & CM_DANGER ? " danger" : "", fl & CM_OFF ? " off" : "",
                 fl & CM_ON ? " on" : "", fl & CM_QUIET ? " quiet" : "");
        sb_add(&b, "<button");
        if (cls[0])
            sb_printf(&b, " class=\"%s\"", cls + 1);
        if (!(fl & CM_OFF)) /* (one that can't be done now does nothing) */
            sb_printf(&b, " what=\"menu:%d\"", i);
        sb_add(&b, ">");
        if (CM.keys[i][0]) {
            sb_add(&b, "<text>");
            xml_text(&b, CM.label[i]);
            sb_add(&b, "</text><spacer/><text class=\"keys\">");
            xml_text(&b, CM.keys[i]);
            sb_add(&b, "</text>");
        } else {
            xml_text(&b, CM.label[i]);
        }
        sb_add(&b, "</button>");
    }
    if (CM.note[0]) {
        sb_add(&b, "<text class=\"note\">");
        xml_text(&b, CM.note);
        sb_add(&b, "</text>");
    }
    sb_add(&b, "</menu>");
    fm_node *m = fm_parse_fragment(A.ui, A.ui->desktop, b.p ? b.p : "");
    sb_free(&b);
    CM.pending = false;
    if (!m || strcmp(m->id, "_menu")) {
        CM.live = false;
        return;
    }
    m->generated = true; /* (the system's, never part of the design kept) */
    fm_set_open(A.ui, m, true);
    A.ui->changed = true;
    CM.live = true;
    LOG("context menu: %d things, for %s\n", CM.n, CM.kind);
    menus_check_styled(); /* the design has none of its own yet: the companion writes them (menus.c) */
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
    int n = 0;
    for (fm_node *c = m->first; c && n < CTX_MAX; c = c->next)
        if (c->kind == FM_ELEM && !strcmp(c->tag, "button") && fm_attr_get(c, "what"))
            items[n++] = c;
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
    int k = 0;
    for (fm_node *c = m ? m->first : NULL; c; c = c->next)
        if (c->kind == FM_ELEM && !strcmp(c->tag, "button") && fm_attr_get(c, "what"))
            c->hover = k++ == CM.sel;
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
