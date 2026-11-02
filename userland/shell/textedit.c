#include "textedit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ED_NONE, ED_TYPE, ED_DELETE, ED_OTHER };

static te_menu menu;
static const text_edit *focused_ed; /* the field drawn with the caret most recently, and when */
static uint64_t focused_at;

te_menu *te_menu_state(void) { return &menu; }

/* a right-click in the field being typed in: the caret goes there unless it is on the selection, and the menu opens */
static bool open_menu(text_edit *e, const input_state *in, int o, bool secret, uint64_t now) {
    if (!in->right_clicked || focused_ed != e || now - focused_at > 300)
        return false;
    int a = e->caret < e->anchor ? e->caret : e->anchor, b = e->caret > e->anchor ? e->caret : e->anchor;
    if (o <= a || o >= b) /* on the selection keeps it; anywhere else, even at its edge, moves the caret */
        e->caret = e->anchor = o;
    menu.open = true;
    menu.sel = -1, menu.shown = false;
    menu.x = in->mx, menu.y = in->my;
    menu.selection = e->caret != e->anchor;
    menu.undo = e->nundo > 0;
    menu.secret = secret;
    e->moved_at = now;
    return true;
}

static int len_of(const char *b) { return (int)strlen(b); }
static int lo(const text_edit *e) { return e->caret < e->anchor ? e->caret : e->anchor; }
static int hi(const text_edit *e) { return e->caret > e->anchor ? e->caret : e->anchor; }

static int cp_prev(const char *b, int o) {
    if (o <= 0)
        return 0;
    do
        o--;
    while (o > 0 && (b[o] & 0xc0) == 0x80);
    return o;
}

static int cp_next(const char *b, int o) {
    int n = len_of(b);
    if (o >= n)
        return n;
    do
        o++;
    while (o < n && (b[o] & 0xc0) == 0x80);
    return o;
}

static bool wordch(char ch) {
    unsigned char c = (unsigned char)ch;
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int word_left(const char *b, int o) {
    while (o > 0 && !wordch(b[o - 1]))
        o = cp_prev(b, o);
    while (o > 0 && wordch(b[o - 1]))
        o = cp_prev(b, o);
    return o;
}

static int word_right(const char *b, int o) {
    int n = len_of(b);
    while (o < n && !wordch(b[o]))
        o = cp_next(b, o);
    while (o < n && wordch(b[o]))
        o = cp_next(b, o);
    return o;
}

/* ---- wrapped lines (multi-line) ---- */

static unsigned long hash_of(const char *s) {
    unsigned long h = 1469598103934665603ul;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ul;
    return h;
}

static void add_line(text_edit *e, int start) {
    if (e->nlines == e->lines_cap) {
        e->lines_cap = e->lines_cap ? e->lines_cap * 2 : 64;
        e->lines = realloc(e->lines, sizeof(int) * (size_t)e->lines_cap);
    }
    e->lines[e->nlines++] = start;
}

/* lines broken at spaces where they would pass w (inside a long word where there is none), and at every \n */
static void layout(text_edit *e, const char *buf, font *f, int w) {
    unsigned long h = hash_of(buf);
    if (e->nlines && e->lay_hash == h && e->lay_w == w && e->lay_font == f)
        return;
    e->lay_hash = h, e->lay_w = w, e->lay_font = f;
    e->line_h = (int)(font_line_height(f) * 1.3f + 0.5f);
    e->nlines = 0;
    add_line(e, 0);
    int n = len_of(buf), start = 0, brk = -1;
    for (int i = 0; i < n;) {
        if (buf[i] == '\n') {
            i++;
            add_line(e, i);
            start = i, brk = -1;
            continue;
        }
        int nx = cp_next(buf, i);
        if (i > start && text_width_n(f, buf + start, nx - start) > w) {
            int at = brk > start ? brk : i;
            add_line(e, at);
            start = at, brk = -1, i = at;
            continue;
        }
        if (buf[i] == ' ')
            brk = nx;
        i = nx;
    }
}

static int line_of(const text_edit *e, int off) {
    int l = 0;
    while (l + 1 < e->nlines && e->lines[l + 1] <= off)
        l++;
    return l;
}

/* where line l ends: before its \n, or before the space it wrapped at, so the caret at the end of a line stays
 * on it (a word broken for want of room ends where the next line starts) */
static int line_end(const text_edit *e, const char *buf, int l) {
    if (l + 1 >= e->nlines)
        return len_of(buf);
    int end = e->lines[l + 1];
    if (end <= e->lines[l])
        return end;
    return buf[end - 1] == '\n' || buf[end - 1] == ' ' ? end - 1 : end;
}

/* the character boundary on line l nearest to x */
static int off_in_line(const text_edit *e, const char *buf, int l, int x) {
    int a = e->lines[l], b = line_end(e, buf, l), best = a, bestd = 1 << 30;
    for (int o = a;; o = cp_next(buf, o)) {
        int d = abs(text_width_n(e->lay_font, buf + a, o - a) - x);
        if (d < bestd)
            bestd = d, best = o;
        else
            break;
        if (o >= b)
            break;
    }
    /* a wrapped line's last boundary is the next line's first: stay on this one */
    if (best == b && l + 1 < e->nlines && b == e->lines[l + 1] && b > a)
        best = cp_prev(buf, b);
    return best;
}

/* the caller changed the text (cleared it after sending, filled it in): start again at its end */
static void sync(text_edit *e, const char *buf) {
    int n = len_of(buf);
    if (n != e->known_len || e->caret > n || e->anchor > n) {
        e->caret = e->anchor = n;
        e->known_len = n;
        e->last_kind = ED_NONE;
    }
}

void te_reset(text_edit *e, const char *buf) {
    e->caret = e->anchor = e->known_len = len_of(buf);
    e->scroll = 0;
    e->last_kind = ED_NONE;
    e->dragging = false;
}

void te_select_all(text_edit *e, const char *buf) {
    te_reset(e, buf);
    e->anchor = 0;
}

void te_select_stem(text_edit *e, const char *buf, bool folder) {
    te_reset(e, buf);
    const char *dot = folder ? NULL : strrchr(buf, '.');
    e->anchor = 0;
    e->caret = dot && dot > buf ? (int)(dot - buf) : len_of(buf);
}

static void move(text_edit *e, int to, bool extend, uint64_t now) {
    e->caret = to;
    if (!extend)
        e->anchor = to;
    e->moved_at = now;
    e->last_kind = ED_NONE;
    e->follow = true;
    e->vertical = false;
}

static void push(char **stack, int *carets, int *n, char *s, int caret) {
    if (*n == TE_HISTORY) {
        free(stack[0]);
        memmove(stack, stack + 1, sizeof(char *) * (TE_HISTORY - 1));
        memmove(carets, carets + 1, sizeof(int) * (TE_HISTORY - 1));
        (*n)--;
    }
    stack[*n] = s;
    carets[*n] = caret;
    (*n)++;
}

static void clear_redo(text_edit *e) {
    while (e->nredo)
        free(e->redo[--e->nredo]);
}

/* replaces [a, b) with s (n bytes), keeping what was there for undo; false when it would not fit */
static bool replace(text_edit *e, char *buf, int cap, int a, int b, const char *s, int n, int kind, uint64_t now) {
    int len = len_of(buf);
    if (len - (b - a) + n > cap - 1) { /* only whole characters of what fits */
        n = cap - 1 - (len - (b - a));
        while (n > 0 && (s[n] & 0xc0) == 0x80)
            n--;
        if (n <= 0 && a == b)
            return false;
        if (n < 0)
            n = 0;
    }
    if (kind != e->last_kind || kind == ED_OTHER || now - e->last_edit > 1200 ||
        (kind == ED_TYPE && n == 1 && s[0] == ' ')) {
        push(e->undo, e->undo_caret, &e->nundo, strdup(buf), e->caret);
        clear_redo(e);
    }
    memmove(buf + a + n, buf + b, (size_t)(len - b) + 1);
    memcpy(buf + a, s, (size_t)n);
    e->caret = e->anchor = a + n;
    e->known_len = len_of(buf);
    e->last_kind = kind;
    e->last_edit = e->moved_at = now;
    e->follow = true;
    e->vertical = false;
    return true;
}

static void restore(text_edit *e, char *buf, int cap, bool forward, uint64_t now) {
    char **from = forward ? e->redo : e->undo, **to = forward ? e->undo : e->redo;
    int *fc = forward ? e->redo_caret : e->undo_caret, *tc = forward ? e->undo_caret : e->redo_caret;
    int *fn = forward ? &e->nredo : &e->nundo, *tn = forward ? &e->nundo : &e->nredo;
    if (!*fn)
        return;
    char *s = from[--*fn];
    int caret = fc[*fn];
    push(to, tc, tn, strdup(buf), e->caret);
    snprintf(buf, (size_t)cap, "%s", s);
    free(s);
    int n = len_of(buf);
    e->caret = e->anchor = caret > n ? n : caret;
    e->known_len = n;
    e->last_kind = ED_NONE;
    e->moved_at = now;
}

char *te_selected(const text_edit *e, const char *buf) {
    int a = lo(e), b = hi(e), n = len_of(buf);
    if (a == b || b > n)
        return NULL;
    char *s = malloc((size_t)(b - a) + 1);
    memcpy(s, buf + a, (size_t)(b - a));
    s[b - a] = 0;
    return s;
}

bool te_key(text_edit *e, char *buf, int cap, int key, bool ctrl, bool shift, bool secret, uint64_t now,
            bool *changed) {
    sync(e, buf);
    bool sel = e->caret != e->anchor;
    int len = len_of(buf), before = e->known_len;
    char *was = NULL;
    (void)before;
    if (changed)
        *changed = false;
    bool ch = false;
    if (e->multiline && e->lay_font) {
        layout(e, buf, e->lay_font, e->lay_w);
        int l = line_of(e, e->caret);
        if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN) {
            int page = e->view_lines > 1 ? e->view_lines - 1 : 1;
            int step = key == K_UP ? -1 : key == K_DOWN ? 1 : key == K_PGUP ? -page : page;
            int x = e->vertical ? e->want_x : text_width_n(e->lay_font, buf + e->lines[l], e->caret - e->lines[l]);
            int to = l + step;
            move(e, to < 0 ? 0 : to >= e->nlines ? len : off_in_line(e, buf, to, x), shift, now);
            e->vertical = true, e->want_x = x;
            return true;
        }
        if ((key == K_HOME || key == K_END) && !ctrl) {
            move(e, key == K_HOME ? e->lines[l] : line_end(e, buf, l), shift, now);
            return true;
        }
    }
    if (e->multiline && key == K_ENTER) {
        ch = replace(e, buf, cap, lo(e), hi(e), "\n", 1, ED_OTHER, now);
        if (changed)
            *changed = ch;
        return true;
    }
    switch (key) {
    case K_LEFT:
        if (sel && !shift)
            move(e, lo(e), false, now);
        else
            move(e, ctrl ? word_left(buf, e->caret) : cp_prev(buf, e->caret), shift, now);
        break;
    case K_RIGHT:
        if (sel && !shift)
            move(e, hi(e), false, now);
        else
            move(e, ctrl ? word_right(buf, e->caret) : cp_next(buf, e->caret), shift, now);
        break;
    case K_HOME: move(e, 0, shift, now); break;
    case K_END: move(e, len, shift, now); break;
    case K_BACKSPACE:
        if (sel)
            ch = replace(e, buf, cap, lo(e), hi(e), "", 0, ED_OTHER, now);
        else if (e->caret > 0)
            ch = replace(e, buf, cap, ctrl ? word_left(buf, e->caret) : cp_prev(buf, e->caret), e->caret, "", 0,
                         ED_DELETE, now);
        break;
    case K_DELETE:
        if (sel)
            ch = replace(e, buf, cap, lo(e), hi(e), "", 0, ED_OTHER, now);
        else if (e->caret < len)
            ch = replace(e, buf, cap, e->caret, ctrl ? word_right(buf, e->caret) : cp_next(buf, e->caret), "", 0,
                         ED_DELETE, now);
        break;
    case K_PASTE: {
        /* one line: breaks and tabs become spaces, other control characters go */
        const char *clip = clipboard_text();
        size_t cl = strlen(clip);
        char *t = malloc(cl + 1);
        int n = 0;
        for (const char *p = clip; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '\n' && e->multiline)
                t[n++] = '\n';
            else if (c == '\n' || c == '\t')
                t[n++] = ' ';
            else if (c >= 32 && c != 127)
                t[n++] = (char)c;
        }
        while (n && t[n - 1] == ' ' && !e->multiline)
            n--;
        if (n)
            ch = replace(e, buf, cap, lo(e), hi(e), t, n, ED_OTHER, now);
        free(t);
        break;
    }
    default:
        if (ctrl && key < 0x100) {
            switch (key) {
            case 'a': case 'A': e->anchor = 0, e->caret = len, e->moved_at = now; break;
            case 'c': case 'C':
                if (!secret && (was = te_selected(e, buf)))
                    clipboard_set(was);
                break;
            case 'x': case 'X':
                if (!secret && (was = te_selected(e, buf))) {
                    clipboard_set(was);
                    ch = replace(e, buf, cap, lo(e), hi(e), "", 0, ED_OTHER, now);
                }
                break;
            case 'z': restore(e, buf, cap, false, now), ch = true; break;
            case 'Z': case 'y': case 'Y': restore(e, buf, cap, true, now), ch = true; break;
            default: return false; /* the window's or the app's shortcut */
            }
            break;
        }
        if (key >= 32 && key < 127) {
            char c = (char)key;
            ch = replace(e, buf, cap, lo(e), hi(e), &c, 1, ED_TYPE, now);
        } else if ((key >= 0xa0 && key < K_ENTER) || (key > K_PRINT && key < 0x110000)) {
            char u[4];
            int n = 0;
            if (key < 0x800)
                u[n++] = (char)(0xc0 | key >> 6);
            else if (key < 0x10000)
                u[n++] = (char)(0xe0 | key >> 12), u[n++] = (char)(0x80 | ((key >> 6) & 0x3f));
            else
                u[n++] = (char)(0xf0 | key >> 18), u[n++] = (char)(0x80 | ((key >> 12) & 0x3f)),
                u[n++] = (char)(0x80 | ((key >> 6) & 0x3f));
            u[n++] = (char)(0x80 | (key & 0x3f));
            ch = replace(e, buf, cap, lo(e), hi(e), u, n, ED_TYPE, now);
        } else {
            return false; /* Enter, Esc, Tab, Up, Down, Page keys: the caller's */
        }
    }
    free(was);
    e->known_len = len_of(buf);
    if (changed)
        *changed = ch;
    return true;
}

/* ---- measuring and drawing ---- */

#define BULLET "\xe2\x80\xa2"

/* what is drawn: the text, or a bullet for each character of a secret */
static const char *shown(const char *buf, bool secret, char *tmp, int cap) {
    if (!secret)
        return buf;
    int k = 0;
    for (const char *p = buf; *p && k < cap - 4; p++)
        if ((*p & 0xc0) != 0x80)
            memcpy(tmp + k, BULLET, 3), k += 3;
    tmp[k] = 0;
    return tmp;
}

/* the offset in what is drawn for a byte offset in buf */
static int shown_off(const char *buf, int off, bool secret) {
    if (!secret)
        return off;
    int k = 0;
    for (int i = 0; i < off && buf[i]; i++)
        if ((buf[i] & 0xc0) != 0x80)
            k += 3;
    return k;
}

static int x_of(font *f, const char *buf, int off, bool secret, const char *s) {
    return text_width_n(f, s, shown_off(buf, off, secret));
}

/* the character boundary nearest to x (pixels from where the line starts) */
static int off_at(font *f, const char *buf, int x, bool secret, const char *s) {
    int n = len_of(buf), best = 0, bestd = 1 << 30;
    for (int o = 0;; o = cp_next(buf, o)) {
        int d = abs(x_of(f, buf, o, secret, s) - x);
        if (d < bestd)
            bestd = d, best = o;
        else
            break; /* widths only grow */
        if (o >= n)
            break;
    }
    return best;
}

bool te_mouse(text_edit *e, const char *buf, font *f, rect box, int text_x, const input_state *in, bool secret,
              uint64_t now) {
    sync(e, buf);
    bool inside = pt_in(box, in->mx, in->my);
    if (e->multiline) {
        if (!e->lay_font)
            return inside && in->pressed;
        layout(e, buf, e->lay_font, e->lay_w);
        if (inside && in->wheel) {
            e->vscroll -= in->wheel * e->line_h * 3;
            e->follow = false;
        }
        int l = (in->my - box.y + e->vscroll) / (e->line_h ? e->line_h : 1);
        l = l < 0 ? 0 : l >= e->nlines ? e->nlines - 1 : l;
        int o = off_in_line(e, buf, l, in->mx - text_x);
        if (in->my - box.y + e->vscroll >= e->nlines * e->line_h)
            o = len_of(buf);
        if (inside && open_menu(e, in, o, secret, now))
            return true;
        if (in->pressed && inside) {
            bool again = now - e->click_at < 450 && abs(in->mx - e->click_x) < 6;
            e->clicks = again ? e->clicks % 3 + 1 : 1;
            e->click_at = now, e->click_x = in->mx;
            if (e->clicks == 2) {
                int a = o, b = o;
                if (o < len_of(buf) && wordch(buf[o])) {
                    while (a > 0 && wordch(buf[a - 1]))
                        a = cp_prev(buf, a);
                    b = word_right(buf, o);
                } else if (o > 0 && wordch(buf[o - 1])) {
                    a = word_left(buf, o);
                }
                e->anchor = a, e->caret = b;
            } else if (e->clicks == 3) { /* the paragraph */
                int a = o, b = o, n = len_of(buf);
                while (a > 0 && buf[a - 1] != '\n')
                    a--;
                while (b < n && buf[b] != '\n')
                    b++;
                e->anchor = a, e->caret = b;
            } else {
                e->caret = o;
                if (!in->shift)
                    e->anchor = o;
            }
            e->dragging = e->clicks == 1;
            e->moved_at = now;
            e->last_kind = ED_NONE;
            e->vertical = false;
            return true;
        }
        if (e->dragging) {
            if (!in->down)
                e->dragging = false;
            else {
                e->caret = o, e->moved_at = now;
                if (in->my < box.y || in->my > box.y + box.h)
                    e->follow = true; /* past the top or bottom: scroll along */
            }
        }
        return false;
    }
    char tmp[4096];
    const char *s = shown(buf, secret, tmp, sizeof tmp);
    int x = in->mx - (text_x - e->scroll);
    if (inside && open_menu(e, in, off_at(f, buf, x, secret, s), secret, now))
        return true;
    if (in->pressed && inside) {
        int o = off_at(f, buf, x, secret, s);
        bool again = now - e->click_at < 450 && abs(in->mx - e->click_x) < 6;
        e->clicks = again ? e->clicks % 3 + 1 : 1;
        e->click_at = now, e->click_x = in->mx;
        if (e->clicks == 2 && !secret) { /* a word, or the gap between two */
            int a = o, b = o;
            if (o < len_of(buf) && wordch(buf[o])) {
                while (a > 0 && wordch(buf[a - 1]))
                    a = cp_prev(buf, a);
                b = word_right(buf, o);
            } else if (o > 0 && wordch(buf[o - 1])) {
                b = o;
                a = word_left(buf, o);
            }
            e->anchor = a, e->caret = b;
        } else if (e->clicks >= 2) {
            e->anchor = 0, e->caret = len_of(buf);
        } else {
            e->caret = o;
            if (!in->shift)
                e->anchor = o;
        }
        e->dragging = e->clicks == 1;
        e->moved_at = now;
        e->last_kind = ED_NONE;
        return true;
    }
    if (e->dragging) {
        if (!in->down) {
            e->dragging = false;
        } else {
            /* past either end the line scrolls along */
            if (in->mx < box.x)
                e->scroll -= 8;
            else if (in->mx > box.x + box.w)
                e->scroll += 8;
            e->caret = off_at(f, buf, in->mx - (text_x - e->scroll), secret, s);
            e->moved_at = now;
        }
    }
    return false;
}

static void draw_lines(canvas *c, text_edit *e, const char *buf, font *f, rect box, color fg, color selection,
                       bool focused, uint64_t now) {
    layout(e, buf, f, box.w - 4);
    int lh = e->line_h, fh = font_line_height(f), top = (lh - fh) / 2;
    e->view_lines = box.h / (lh ? lh : 1);
    int cl = line_of(e, e->caret);
    if (e->follow) { /* the caret's line in view */
        if (cl * lh - e->vscroll < 0)
            e->vscroll = cl * lh;
        if ((cl + 1) * lh - e->vscroll > box.h)
            e->vscroll = (cl + 1) * lh - box.h;
        e->follow = false;
    }
    int most = e->nlines * lh - box.h;
    if (e->vscroll > most)
        e->vscroll = most;
    if (e->vscroll < 0)
        e->vscroll = 0;
    int k0 = c->cx0, k1 = c->cy0, k2 = c->cx1, k3 = c->cy1;
    if (box.x > c->cx0) c->cx0 = box.x;
    if (box.y > c->cy0) c->cy0 = box.y;
    if (box.x + box.w < c->cx1) c->cx1 = box.x + box.w;
    if (box.y + box.h < c->cy1) c->cy1 = box.y + box.h;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
    int a = lo(e), b = hi(e);
    char seg[4096];
    for (int l = e->vscroll / (lh ? lh : 1); l < e->nlines; l++) {
        int y = box.y + l * lh - e->vscroll;
        if (y > box.y + box.h)
            break;
        int s0 = e->lines[l], s1 = line_end(e, buf, l);
        if (focused && a != b && a <= s1 && b >= s0) { /* this line's part of the selection */
            int x0 = a > s0 ? text_width_n(f, buf + s0, a - s0) : 0;
            int x1 = b < s1 ? text_width_n(f, buf + s0, b - s0) : text_width_n(f, buf + s0, s1 - s0) + (b > s1 ? 6 : 0);
            fill_rect(c, (rect){box.x + x0, y, x1 - x0, lh}, selection);
        }
        int n = s1 - s0 < (int)sizeof seg - 1 ? s1 - s0 : (int)sizeof seg - 1;
        memcpy(seg, buf + s0, (size_t)n);
        seg[n] = 0;
        draw_text(c, f, box.x, y + top, seg, fg);
    }
    if (focused && (now - e->moved_at < 600 || (now / 530) % 2 == 0)) {
        int s0 = e->lines[cl];
        fill_rect(c, (rect){box.x + text_width_n(f, buf + s0, e->caret - s0), box.y + cl * lh - e->vscroll + top, 2, fh},
                  fg);
    }
    c->cx0 = k0, c->cy0 = k1, c->cx1 = k2, c->cy1 = k3;
}

void te_draw(canvas *c, text_edit *e, const char *buf, font *f, rect box, int text_x, int ty, color fg,
             color selection, bool focused, bool secret, uint64_t now) {
    sync(e, buf);
    if (focused)
        focused_ed = e, focused_at = now;
    if (e->multiline) {
        draw_lines(c, e, buf, f, box, fg, selection, focused, now);
        return;
    }
    char tmp[4096];
    const char *s = shown(buf, secret, tmp, sizeof tmp);
    int w = text_width(f, s), room = box.x + box.w - text_x - 2;
    int cx = x_of(f, buf, e->caret, secret, s);
    /* the caret in view, and no empty space scrolled in past the end */
    if (cx - e->scroll > room)
        e->scroll = cx - room;
    if (cx - e->scroll < 0)
        e->scroll = cx;
    if (e->scroll > w - room)
        e->scroll = w - room;
    if (e->scroll < 0)
        e->scroll = 0;
    int x0 = text_x - e->scroll, lh = font_line_height(f);
    int k0 = c->cx0, k1 = c->cy0, k2 = c->cx1, k3 = c->cy1; /* inside whatever clip is already there */
    if (box.x > c->cx0) c->cx0 = box.x;
    if (box.y > c->cy0) c->cy0 = box.y;
    if (box.x + box.w < c->cx1) c->cx1 = box.x + box.w;
    if (box.y + box.h < c->cy1) c->cy1 = box.y + box.h;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
    if (focused && e->caret != e->anchor) {
        int a = x_of(f, buf, lo(e), secret, s), b = x_of(f, buf, hi(e), secret, s);
        fill_rect(c, (rect){x0 + a, ty, b - a, lh}, selection);
    }
    draw_text(c, f, x0, ty, s, fg);
    if (focused && (now - e->moved_at < 600 || (now / 530) % 2 == 0))
        fill_rect(c, (rect){x0 + cx, ty + 1, 2, lh - 2}, fg);
    c->cx0 = k0, c->cy0 = k1, c->cx1 = k2, c->cy1 = k3;
}

void te_free(text_edit *e) {
    free(e->lines);
    e->lines = NULL, e->nlines = e->lines_cap = 0;
    while (e->nundo)
        free(e->undo[--e->nundo]);
    clear_redo(e);
}
