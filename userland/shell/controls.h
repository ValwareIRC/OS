#pragma once
#include <stdbool.h>
#include "font.h"
#include "gfx.h"

/* Controls shared by the tray's panels and Settings, drawn in the design's
 * own colours: a slider, a switch, a segmented choice, buttons. They read
 * this frame's input (A.in) themselves, like the rest of the shell's UI. */

typedef struct palette {
    color ink, muted, accent, on_accent, line, under;
    float op; /* everything fades with the panel */
} palette;

/* The design's colours, corrected so they read on `under` (the colour
 * already painted behind the control). */
palette pal_on(color under, float op);
/* The average colour painted inside r. */
color pal_sample(rect r);

/* A floating panel in the design's material (glass, frost, solid or neon),
 * faded by `op`: what the tray's popovers, toasts and menus are made of. */
void ctl_panel(rect r, float radius, float op);

enum { CTL_IDLE, CTL_CHANGING, CTL_RELEASED };
/* A horizontal slider over 0..1; `id` tells sliders apart while dragging.
 * Returns CTL_CHANGING while *v moves and CTL_RELEASED when let go. */
int ctl_slider(int id, rect r, float *v, const palette *p);
/* An on/off switch; returns true when clicked. */
bool ctl_switch(rect r, bool on, const palette *p);
/* A row of choices; returns the one clicked, or `sel`. */
int ctl_segmented(rect r, const char *const *labels, int n, int sel, font *f, const palette *p);
/* A rounded button, filled with the accent if primary. */
bool ctl_button(rect r, const char *label, font *f, bool primary, bool enabled, const palette *p);
/* A square tile that toggles (quick settings): symbol drawn by the caller. */
bool ctl_tile(rect r, bool on, const palette *p);
/* A slider is being dragged (anything under it should ignore the pointer). */
bool ctl_dragging(void);
/* The id of the slider being dragged, or -1. */
int ctl_drag_id(void);
/* Text cut to fit `w` with an ellipsis. */
void ctl_text_fit(canvas *c, font *f, int x, int y, int w, const char *s, color col);

/* Small line symbols for the tray and Settings, drawn in a box. */
enum {
    SYM_SPEAKER, SYM_SPEAKER_MUTED, SYM_NETWORK, SYM_NETWORK_OFF, SYM_BELL, SYM_BELL_OFF, SYM_CALENDAR,
    SYM_SLIDERS, SYM_MOON, SYM_SUN, SYM_DISPLAY, SYM_KEYBOARD, SYM_MIND, SYM_SHIELD, SYM_INFO, SYM_PALETTE,
    SYM_CHEVRON_LEFT, SYM_CHEVRON_RIGHT, SYM_CLOSE, SYM_CHECK, SYM_GEAR, SYM_SENSE,
};
/* `level` 0..1 for the speaker (how many waves). */
void ctl_symbol(canvas *c, rect box, int sym, float level, color col);
