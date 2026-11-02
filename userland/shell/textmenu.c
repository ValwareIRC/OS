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
