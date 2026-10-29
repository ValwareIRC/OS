#pragma once
/* Window management: moving, resizing from every edge, snapping, the
 * Alt+Tab switcher, window animations and focus, for the built-in windows
 * and Linux (Wayland) app windows alike. desktop.c owns what is inside
 * windows and the Facet document owns how their frames look; wm.c decides
 * where windows are and how they get there. desktop.c and main.c call the
 * hooks below. */
#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

/* ---- main.c ---- */
/* Around each frame's work: frame times while a window moves are logged. */
void wm_frame_begin(void);
void wm_frame_end(void);
/* How long the main loop may sleep (one frame while things move). */
int wm_poll_ms(int idle_ms);
/* After the pointer is drawn: resize arrows on a window's border. */
void wm_cursor(canvas *c, int x, int y);

/* ---- desktop.c, in desktop_draw order ---- */
/* First thing: Alt+Tab, Alt+F4, Super+arrows. Takes the keys it uses. */
void wm_keys(void);
/* While a window is moved or resized, the picture of everything below it is
 * kept: this puts it on the frame and returns true (then the wallpaper,
 * the layer below windows and the windows below are not drawn again). */
bool wm_scene_cached(void);
/* With the windows in z order and the one under the pointer (`top`):
 * presses on borders, moving, resizing and snapping. May change `top`. */
void wm_pointer(const int *order, int n, bool over_ui, int *top);
/* Draws window w (through `draw`), animated when it is arriving, flying or
 * changing size; skipped when it is part of the kept picture. */
void wm_draw_window(int w, bool focused, void (*draw)(int w, bool focused));
/* After the windows: ones fading out or flying to their taskbar item. */
void wm_draw_ghosts(void);
/* Last: the Alt+Tab switcher. */
void wm_draw_overlay(void);

/* ---- window events (desktop.c) ---- */
void wm_opened(int w);          /* a window appeared */
void wm_unmapped(int w);        /* an app window went away */
void wm_close(int w);           /* fades out, then desktop_close_window */
void wm_minimize(int w);        /* flies into its taskbar item */
void wm_toggle_maximize(int w);
void wm_begin_move(int w);      /* its titlebar was pressed (twice: maximize) */
void wm_activate(int w);        /* raise, and bring back if minimized */

/* Provided by desktop.c: closes a window for good. */
void desktop_close_window(int w);
