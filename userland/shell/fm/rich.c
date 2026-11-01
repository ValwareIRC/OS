/* Rich text: a paragraph whose words come from text and inline elements
 * (<b> <strong> <i> <em> <u> <s> <del> <mark> <code> <kbd> <small> <a>
 * <span> <br>), flowed together line by line, each word in the style of the
 * element it is in. The inline elements are not boxes in the layout; they
 * get the union of their words as their box, so they can be hovered and
 * clicked (a link with what="..."). */
#include "fmi.h"
#include "../../lib/json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_SEGS 600

struct seg {
    fm_node *el;    /* whose style: the inline element, or the paragraph itself */
    char *word;     /* in the text buffer */
    float x, y;     /* from the paragraph's content corner */
    float w, h, asc;
    bool space_after;
    bool first_of_el, last_of_el; /* for an inline element's padding and background ends */
};

struct fm_rich {
    struct seg seg[MAX_SEGS];
    int n;
    char *buf;
    float w, h;
};

static const char *const inline_tags[] = {"b", "strong", "i", "em", "u", "s", "del", "ins", "mark", "code", "kbd",
                                          "small", "a", "span", "br", "sup", "sub", "q", "abbr", "cite", NULL};

bool fmi_is_inline(const fm_node *n) {
    if (n->kind != FM_ELEM)
        return false;
    for (int i = 0; inline_tags[i]; i++)
        if (same_tag(inline_tags[i], n->tag))
            return true;
    return false;
}

static bool all_inline(const fm_node *n) {
    for (const fm_node *k = n->first; k; k = k->next) {
        if (k->kind == FM_TEXT || k->pseudo)
            continue;
        if (!fmi_is_inline(k) || !all_inline(k))
            return false;
    }
    return true;
}

/* A paragraph to flow as rich text: words and inline elements only, at least one inline element (or a
 * decoration, which only rich text draws) */
bool fmi_is_rich(const fm_node *n) {
    if (n->kind != FM_ELEM || fmi_is_inline(n) || tag_is(n, "canvas") || tag_is(n, "svg") || tag_is(n, "pixels") ||
        tag_is(n, "search") || tag_is(n, "input"))
        return false;
    bool any_inline = false, any_text = false;
    for (const fm_node *k = n->first; k; k = k->next) {
        if (k->kind == FM_TEXT) {
            any_text |= k->text && k->text[strspn(k->text, " \t\r\n")];
            continue;
        }
        if (k->pseudo)
            continue;
        if (!fmi_is_inline(k) || !all_inline(k))
            return false;
        any_inline = true;
    }
    return any_inline || (any_text && n->st.decoration);
}

void fmi_rich_free(fm_node *n) {
    if (n->rich) {
        free(n->rich->buf);
        free(n->rich);
        n->rich = NULL;
    }
}

/* ---- collecting the words ---- */

struct piece {
    fm_node *el;
    size_t at, len; /* in the buffer */
    bool space_after, brk;
};

static struct piece pieces[MAX_SEGS];
static int npieces;
static sbuf text;
static bool pending_space;

static void add_words(fm_node *el, const char *t) {
    const fm_style *st = &el->st;
    char tmp[1024];
    fmi_text_for_draw(st, t, tmp, sizeof tmp); /* its case (uppercase...); it trims the ends, so they are read here */
    bool lead = *t == ' ' || *t == '\n' || *t == '\t' || *t == '\r';
    size_t tl = strlen(t);
    bool trail = tl && (t[tl - 1] == ' ' || t[tl - 1] == '\n' || t[tl - 1] == '\t' || t[tl - 1] == '\r');
    if (lead && npieces)
        pieces[npieces - 1].space_after = true;
    for (const char *p = tmp; *p;) {
        if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') {
            if (npieces)
                pieces[npieces - 1].space_after = true;
            else
                pending_space = true;
            while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')
                p++;
            continue;
        }
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n' && *e != '\t' && *e != '\r')
            e++;
        if (npieces < MAX_SEGS) {
            pieces[npieces] = (struct piece){el, text.len, (size_t)(e - p), false, false};
            sb_addn(&text, p, (size_t)(e - p));
            sb_addn(&text, "", 1);
            npieces++;
        }
        p = e;
    }
    if (trail && npieces)
        pieces[npieces - 1].space_after = true;
}

static void collect(fm_node *el) {
    for (fm_node *k = el->first; k; k = k->next) {
        if (k->kind == FM_TEXT) {
            if (k->text && !k->generated)
                add_words(el, k->text);
            else if (k->text && k->generated && el->pseudo) /* ::before / ::after words */
                add_words(el, k->text);
        } else if (tag_is(k, "br")) {
            if (npieces)
                pieces[npieces - 1].brk = true;
        } else if (!k->st.hidden) {
            collect(k);
        }
    }
}

/* ---- laying them out ---- */

static float pad_l(const fm_node *el, const fm_node *para) { return el == para ? 0 : el->st.pad[3] + el->st.margin[3]; }
static float pad_r(const fm_node *el, const fm_node *para) { return el == para ? 0 : el->st.pad[1] + el->st.margin[1]; }

/* Flows the words into lines `width` wide; returns how much room it takes (w, h). Kept on the node. */
void fmi_rich_layout(fm_node *para, float width, float *w_out, float *h_out) {
    npieces = 0;
    pending_space = false;
    sb_init(&text);
    collect(para);
    if (!para->rich)
        para->rich = calloc(1, sizeof *para->rich);
    struct fm_rich *R = para->rich;
    free(R->buf);
    R->buf = text.p ? text.p : strdup("");
    R->n = 0;
    float x = 0, y = 0, line_h = 0, line_asc = 0, maxw = 0;
    int line_start = 0;
    const fm_style *pst = &para->st;
    for (int i = 0; i <= npieces; i++) {
        bool end = i == npieces;
        struct piece *p = end ? NULL : &pieces[i];
        float ww = 0, sw = 0, lh = 0, asc = 0, pl = 0, pr = 0;
        font *f = NULL;
        if (p) {
            f = fmi_text_font(&p->el->st, 1);
            ww = (float)text_width(f, R->buf + p->at);
            sw = p->space_after ? (float)text_width(f, " ") : 0;
            lh = (float)font_line_height(f) * p->el->st.line_height;
            asc = (float)font_ascent(f) + (lh - (float)font_line_height(f)) / 2;
            bool first = i == 0 || pieces[i - 1].el != p->el, last = i == npieces - 1 || pieces[i + 1].el != p->el;
            pl = first ? pad_l(p->el, para) : 0;
            pr = last ? pad_r(p->el, para) : 0;
        }
        bool wrap = p && x > 0 && x + pl + ww + pr > width && !pst->nowrap;
        if (end || wrap) { /* finish the line: words share a baseline; the line aligned as the paragraph says */
            float lw = 0;
            for (int k = line_start; k < R->n; k++)
                lw = fmaxf(lw, R->seg[k].x + R->seg[k].w);
            float shift = pst->text_align == FM_T_CENTER ? (width - lw) / 2 : pst->text_align == FM_T_END ? width - lw : 0;
            if (shift < 0 || width > 1e6f)
                shift = 0;
            for (int k = line_start; k < R->n; k++) {
                R->seg[k].y = y + (line_asc - R->seg[k].asc);
                R->seg[k].x += shift;
            }
            maxw = fmaxf(maxw, lw);
            y += line_h;
            x = 0, line_h = line_asc = 0, line_start = R->n;
            if (end)
                break;
        }
        if (R->n >= MAX_SEGS)
            break;
        struct seg *s = &R->seg[R->n++];
        *s = (struct seg){p->el, R->buf + p->at, x + pl, 0, ww, lh, asc, p->space_after,
                          i == 0 || pieces[i - 1].el != p->el, i == npieces - 1 || pieces[i + 1].el != p->el};
        x += pl + ww + pr + sw;
        line_h = fmaxf(line_h, lh);
        line_asc = fmaxf(line_asc, asc);
        if (p->brk) { /* <br>: the next word starts a line */
            float lw = x;
            maxw = fmaxf(maxw, lw);
            for (int k = line_start; k < R->n; k++)
                R->seg[k].y = y + (line_asc - R->seg[k].asc);
            y += line_h;
            x = 0, line_h = line_asc = 0, line_start = R->n;
        }
    }
    R->w = maxw;
    R->h = y;
    *w_out = maxw;
    *h_out = y;
}

/* Once placed: its inline elements take the boxes their words make (for hovering and clicking them) */
void fmi_rich_place(fm_node *para, rect content) {
    struct fm_rich *R = para->rich;
    if (!R)
        return;
    for (fm_node *k = para->first; k; k = k->next)
        k->laid_out = false;
    /* every inline element (at any depth) gets the union of its words */
    for (int i = 0; i < R->n; i++) {
        for (fm_node *el = R->seg[i].el; el && el != para; el = el->parent) {
            rect r = {content.x + (int)R->seg[i].x, content.y + (int)R->seg[i].y, (int)ceilf(R->seg[i].w),
                      (int)ceilf(R->seg[i].h)};
            if (!el->laid_out) {
                el->box = el->vis = r;
                el->laid_out = true;
                el->vscale = para->vscale;
            } else {
                int x0 = el->box.x < r.x ? el->box.x : r.x, y0 = el->box.y < r.y ? el->box.y : r.y;
                int x1 = el->box.x + el->box.w > r.x + r.w ? el->box.x + el->box.w : r.x + r.w;
                int y1 = el->box.y + el->box.h > r.y + r.h ? el->box.y + el->box.h : r.y + r.h;
                el->box = el->vis = (rect){x0, y0, x1 - x0, y1 - y0};
            }
        }
    }
    para->rich_at = content;
}

/* ---- drawing ---- */

static char decoration_of(const fm_node *el, const fm_node *para) {
    for (const fm_node *e = el; e; e = e->parent) {
        if (e->st.decoration)
            return e->st.decoration;
        if (e == para)
            break;
    }
    return 0;
}

void fmi_rich_draw(canvas *c, fm_node *para, float op) {
    struct fm_rich *R = para->rich;
    if (!R)
        return;
    rect at = para->rich_at;
    /* moved since it was laid out (transforms, morphs): follow the paragraph */
    int dx = para->vis.x - para->box.x, dy = para->vis.y - para->box.y;
    for (int i = 0; i < R->n; i++) {
        struct seg *s = &R->seg[i];
        const fm_style *st = &s->el->st;
        float x = at.x + dx + s->x, y = at.y + dy + s->y;
        /* an inline element's own background (mark, code, a chip), around its words */
        if (s->el != para && st->bg.kind == FM_BG_SOLID && C_A(st->bg.a)) {
            float l = s->first_of_el ? st->pad[3] : 0, r = s->last_of_el ? st->pad[1] : 0;
            float w = s->w + l + r + (s->space_after && !s->last_of_el ? (float)text_width(fmi_text_font(st, 1), " ") : 0);
            rect b = {(int)(x - l), (int)(y - st->pad[0]), (int)ceilf(w), (int)ceilf(s->h + st->pad[0] + st->pad[2])};
            fill_round(c, b, st->radius[0] < b.h / 2.0f ? st->radius[0] : b.h / 2.0f, color_alpha(st->bg.a, op));
        }
        font *f = fmi_text_font(st, 1);
        color col = color_alpha(s->el->av.init ? s->el->av.color : st->color, op);
        draw_text(c, f, (int)lroundf(x), (int)lroundf(y + (s->h - (float)font_line_height(f)) / 2), s->word, col);
        char deco = decoration_of(s->el, para);
        if (deco) {
            float thick = fmaxf(1, roundf(st->font_size / 14));
            float w = s->w + (s->space_after && !s->last_of_el ? (float)text_width(f, " ") : 0);
            float base = y + s->asc;
            float ly = deco == 'u' ? base + thick + 1 : deco == 'l' ? base - (float)font_ascent(f) * 0.32f
                                                                     : y + (s->h - (float)font_line_height(f)) / 2;
            fill_rect(c, (rect){(int)x, (int)ly, (int)ceilf(w), (int)thick}, col);
        }
    }
}
