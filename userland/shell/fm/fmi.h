#pragma once
/* Internals shared by the engine's files. */
#include "fm.h"

#include <stdbool.h>
#include <string.h>
#include <strings.h>

static inline bool tag_is(const fm_node *n, const char *t) { return n && n->kind == FM_ELEM && !strcasecmp(n->tag, t); }

/* Popovers and sheets are not part of the flow: they open over everything. */
static inline bool is_popover(const fm_node *n) {
    return n->kind == FM_ELEM && (tag_is(n, "popover") || tag_is(n, "sheet") || tag_is(n, "menu") ||
                                  tag_is(n, "tooltip"));
}

/* Elements that are never drawn: document metadata. */
static inline bool is_meta(const fm_node *n) {
    return n->kind == FM_ELEM && (tag_is(n, "meta") || tag_is(n, "apps") || tag_is(n, "style") ||
                                  tag_is(n, "wallpaper") || tag_is(n, "template") || tag_is(n, "app"));
}

/* System tray elements: drawn by the shell, each opens its own popover. */
static inline bool is_tray(const fm_node *n) {
    return tag_is(n, "volume") || tag_is(n, "network") || tag_is(n, "notifications") || tag_is(n, "quicksettings");
}

static inline bool is_blank_text(const fm_node *n) {
    if (n->kind != FM_TEXT || !n->text)
        return n->kind == FM_TEXT;
    for (const char *p = n->text; *p; p++)
        if (*p != ' ' && *p != '\n' && *p != '\t' && *p != '\r')
            return false;
    return true;
}

/* In the flow of its parent's layout? */
static inline bool in_flow(const fm_node *n) {
    if (n->kind == FM_TEXT) /* a canvas script is not text to show */
        return !is_blank_text(n) && !(n->parent && tag_is(n->parent, "canvas"));
    return !n->st.hidden && !is_popover(n) && !is_meta(n) && !n->st.absolute;
}

/* The element's own text, if it has one that is not in child text nodes:
 * bind="...", text="...", clocks, search fields. Returns NULL otherwise. */
const char *fmi_own_text(fm_doc *d, fm_node *n, char *buf, size_t len);
font *fmi_font(const fm_node *n, const fm_style *st);
/* Text of a text node or element, upper-cased if the style says so. */
void fmi_text_for_draw(const fm_style *st, const char *src, char *out, size_t len);
const char *fmi_place(const fm_node *n, const char *which); /* place=/dock= attribute or style */
/* Typography (fm/type.c): the face for a style at a scale, with its weight,
 * slant and letter-spacing; lower/capitalize; and text drawn with the
 * style's shadow, alignment, wrapping and overflow. */
font *fmi_text_font(const fm_style *st, float scale);
void fmi_text_case(const fm_style *st, char *s);
void fmi_draw_line_in(canvas *c, font *f, rect r, const char *s, color col, const fm_style *st, float scale);
void fmi_draw_block(canvas *c, font *f, rect r, const char *s, color col, const fm_style *st, float scale);
int fmi_measure_block(font *f, int width, const char *s, const fm_style *st);
float fmi_icon_size(const fm_node *n);
void fmi_sync_live(fm_doc *d);
void fmi_arrange(fm_doc *d, fm_node *n, rect box);
void fmi_measure(fm_doc *d, fm_node *n, float aw, float ah);
void fmi_place_popovers(fm_doc *d);
void fmi_transform(fm_node *n, float s, float dx, float dy);
fm_node *fmi_toggler_of(fm_doc *d, const fm_node *pop);
const char *fmi_action_of(const fm_node *n);
fm_node *fmi_tpl_root(fm_doc *d);
void fmi_style_subtree(fm_doc *d, fm_node *n);
void fmi_sync_subtree(fm_doc *d, fm_node *n);
fm_node *fmi_frame_layout(fm_doc *d, int win, rect r, bool focused, rect *content);
void fmi_canvas_draw(fm_doc *d, canvas *cv, fm_node *n, rect r, float op);
bool fmi_canvas_click(fm_doc *d, fm_node *n, int x, int y);
void fmi_canvas_forget(fm_node *n);
