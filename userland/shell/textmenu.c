/* The right-click menu of a text field (textedit.h keeps where it opened and what it can do): drawn over
 * everything, it has the pointer and the keys while it is up, and what is chosen is typed into the field as
 * the shortcut would be. */
#include "app.h"
#include "controls.h"
#include "fm/fm.h"
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

void ui_text_menu_input(void) {
    te_menu *m = te_menu_state();
    if (!m->open)
        return;
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

/* ---- the right-click menu of anything else (desktop.c fills it) ---- */

#define CTX_MAX 6
static struct {
    bool open, shown;
    int x, y, n, sel;
    char label[CTX_MAX][96];
    char act[CTX_MAX][200];
    bool danger[CTX_MAX];
    char note[120];
} CM;

void ctx_menu_open(int x, int y) {
    memset(&CM, 0, sizeof CM);
    CM.open = true, CM.x = x, CM.y = y, CM.sel = -1;
}

void ctx_menu_add(const char *label, const char *act, bool danger) {
    if (CM.n == CTX_MAX)
        return;
    snprintf(CM.label[CM.n], sizeof CM.label[0], "%s", label);
    snprintf(CM.act[CM.n], sizeof CM.act[0], "%s", act);
    CM.danger[CM.n++] = danger;
}

void ctx_menu_note(const char *note) { snprintf(CM.note, sizeof CM.note, "%s", note); }

bool ctx_menu_showing(void) { return CM.open; }

static rect ctx_rect(font *f, int *row) {
    *row = font_line_height(f) + 14;
    int w = 240;
    for (int i = 0; i < CM.n; i++) {
        int tw = text_width(f, CM.label[i]) + 48;
        if (tw > w)
            w = tw;
    }
    int note_h = CM.note[0] ? font_line_height(f) + 10 : 0;
    int h = CM.n * *row + 12 + note_h;
    int x = CM.x + w > A.W - 8 ? A.W - 8 - w : CM.x, y = CM.y + h > A.H - 8 ? CM.y - h : CM.y;
    return (rect){x < 8 ? 8 : x, y < 8 ? 8 : y, w, h};
}

void ctx_menu_input(void) {
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
        else if (k == K_ENTER && CM.sel >= 0)
            chosen = CM.sel;
    }
    int at = pt_in(r, A.in.mx, A.in.my) ? (A.in.my - r.y - 6) / row : -1;
    if (at >= CM.n)
        at = -1;
    if (A.in.moved && pt_in(r, A.in.mx, A.in.my))
        CM.sel = at;
    if (chosen < 0 && A.in.clicked && at >= 0)
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
        LOG("context menu: %s\n", act[0] ? act : "(nothing)");
        if (act[0])
            desktop_ctx_chosen(act); /* (it may open the menu again: a question) */
    }
}

void ctx_menu_draw(void) {
    if (!CM.open)
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
        if (i == CM.sel)
            fill_round(A.frame, it, rad > 8 ? 8 : rad * 0.6f, color_alpha(p.ink, 0.1f));
        int ty = it.y + (row - font_line_height(f)) / 2;
        color ink = CM.danger[i] ? RGBA(0xe0, 0x4b, 0x4b, 255) : p.ink;
        draw_text(A.frame, f, it.x + 12, ty, CM.label[i], ink);
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
