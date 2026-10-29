#pragma once
#include <stdbool.h>
#include "gfx.h"

/* Light and dark, and the colours the built-in apps take from the design
 * (theme.c).
 *
 * Every design has a natural scheme (the one the AI drew it in). The person
 * can ask for the other one at any time: if the design wrote colours for it
 * (an @dark / @light block, or @media (prefers-color-scheme: ...)), those
 * are used; otherwise the shell derives a variant by turning every colour's
 * lightness around while keeping its hue. The choice is kept in
 * /etc/os/theme and applied at boot. */
void theme_set_dark(bool dark);
bool theme_is_dark(void);
void theme_toggle(void);
/* Back to whatever scheme the design was drawn in (forgets the choice). */
void theme_follow_design(void);
/* what="theme:toggle|dark|light|auto" from a design's button; false if the
 * word after "theme:" is unknown. */
bool theme_action(const char *what);

/* The desktop calls these at the very start and end of each frame: they
 * take the hotkey (Ctrl+Shift+D), follow redesigns and companion restyles,
 * apply a pending switch, and cross-fade from the old look to the new. */
void theme_frame_begin(void);
void theme_frame_end(void);
/* What is on screen now fades out over whatever the next frames show. */
void theme_crossfade(void);

/* The terminal's colours: from --term-* variables or a `terminal { }` rule
 * when the design gives them, otherwise derived from the surface and text
 * with good contrast. */
struct term_colors {
    color bg, fg, cursor, selection;
    color ansi[16];
    int cursor_shape; /* 0 block, 1 bar, 2 underline */
    char font[32];    /* monospace family ("" = the default) */
};
void theme_term_colors(struct term_colors *tc, color surface, color text);

/* The whole slot of the window frame just drawn (its padding included), so
 * an app such as the terminal can paint edge to edge; `content` if unknown. */
rect theme_slot_box(rect content);
/* Fills r, rounding whichever corners meet the frame's own rounded ones. */
void theme_fill_slot(rect r, color c);
/* A text colour that reads on what is really painted under r: `want` if it
 * reaches `min` (WCAG ratio), else the nearest colour that does. */
color theme_ink(rect r, color want, float min);
/* The nearest colour to fg with at least `min` contrast against bg. */
color theme_fit(color fg, color bg, float min);
/* The average colour already painted inside r (sampled). */
color theme_under(rect r);
