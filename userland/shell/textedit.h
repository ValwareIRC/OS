#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "font.h"
#include "gfx.h"
#include "sys.h"

/* Editing one line of text, the same everywhere a field takes typing (the shell's own fields, Facet inputs in
 * designs and apps, the launcher, dialogs, renaming): a caret anywhere in the text, a selection (Shift with the
 * arrows, Home and End; Ctrl+A; the pointer: click, drag, double-click a word, triple-click all), Ctrl with the
 * arrows and Backspace/Delete by words, cut, copy and paste, typing over a selection, undo and redo.
 * The text itself stays with the caller (any char buffer); this keeps where the caret is. Offsets are bytes,
 * always on UTF-8 character boundaries. */

#define TE_HISTORY 24

typedef struct text_edit {
    int caret, anchor; /* anchor == caret: nothing selected */
    int scroll;        /* pixels the line is moved left so the caret stays in view */
    int known_len;     /* the length after our own last change: any other length means the caller set new text */
    uint64_t moved_at; /* the caret shows steadily for a moment after it moves, then blinks */
    uint64_t click_at;
    int clicks, click_x;
    bool dragging;
    int last_kind;     /* typing runs are undone as one */
    uint64_t last_edit;
    char *undo[TE_HISTORY], *redo[TE_HISTORY];
    int undo_caret[TE_HISTORY], redo_caret[TE_HISTORY], nundo, nredo;
    /* several lines (set by the caller): Enter makes a new line, Up/Down/Page keys move between lines, the text
     * wraps to the box, Home/End go to the ends of a line (with Ctrl, of the text) */
    bool multiline;
    bool follow;   /* the caret moved: bring it into view at the next draw (the wheel scrolls freely otherwise) */
    bool vertical; /* a run of Up/Down keeps its column */
    int want_x, vscroll, view_lines;
    int *lines, nlines, lines_cap, lay_w, line_h; /* the wrapped lines as last drawn */
    unsigned long lay_hash;
    font *lay_font;
} text_edit;

/* New text from the caller (or the first look at it): the caret goes to its end, the selection goes. */
void te_reset(text_edit *e, const char *buf);
/* Everything selected, as a field opened to change what is in it should be. */
void te_select_all(text_edit *e, const char *buf);
/* A file name to change: the part before its extension selected (all of it for a folder or a dotfile). */
void te_select_stem(text_edit *e, const char *buf, bool folder);
/* One key typed into buf (cap bytes, NUL-terminated). Returns true when the key was for the text (moving,
 * selecting, typing, deleting, clipboard, undo); *changed tells whether the text itself changed. Enter, Esc,
 * Tab, Up and Down are left to the caller (false). ctrl and shift are the modifiers held with the key. */
bool te_key(text_edit *e, char *buf, int cap, int key, bool ctrl, bool shift, bool secret, uint64_t now,
            bool *changed);
/* The pointer over a field whose line starts at text_x (before scrolling) inside box: a press places the
 * caret (Shift extends), dragging selects, two quick presses select a word, three the whole line.
 * Returns true when the press was inside box. */
bool te_mouse(text_edit *e, const char *buf, font *f, rect box, int text_x, const input_state *in, bool secret,
              uint64_t now);
/* Several lines: draws the wrapped text in box (scrolled so the caret is in view after it moves) with the
 * selection and caret; the pointer and the wheel go through te_mouse as for one line (text_x = box.x). */
/* Draws the line: the selection behind it, the text moved so the caret is in view, the caret when focused.
 * text_x/ty: where the line starts and its top. */
void te_draw(canvas *c, text_edit *e, const char *buf, font *f, rect box, int text_x, int ty, color fg,
             color selection, bool focused, bool secret, uint64_t now);
/* The right-click menu of the field being typed in (drawn and run by the shell, ui.c): where it opened and
 * what it can do there. Its items type the same keys a person would (Ctrl+X, Ctrl+V...), so every field
 * handles them as it handles typing. */
typedef struct te_menu {
    bool open;
    int x, y;
    bool selection, undo, secret;
    int sel;    /* the item the arrows or the pointer are on, -1 for none */
    bool shown; /* drawn once since it opened (its place is logged then) */
} te_menu;
te_menu *te_menu_state(void);
/* The selected text (a copy to free), or NULL when nothing is selected. */
char *te_selected(const text_edit *e, const char *buf);
/* Frees the undo history. */
void te_free(text_edit *e);
