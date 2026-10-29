#pragma once
#include <stdbool.h>
#include "controls.h"
#include "font.h"
#include "gfx.h"

/* Settings (settings.c): the Settings app's pages, and every choice made
 * in them, kept in /etc/os/settings.conf (saved to the disk by store.c
 * with the rest of /etc/os) and applied again at boot. */

/* The Settings window's content. */
void settings_draw(rect r);
/* At boot, once the design is loaded: the resolution, keyboard layout and
 * repeat, and the sound level they chose. */
void settings_boot(void);
/* Every frame on the desktop (from the tray): timers such as going back to
 * the old resolution when a new one wasn't confirmed. */
void settings_tick(void);
/* Opens Settings at the page with this title (NULL: where it was). */
void settings_open_page(const char *title);

/* ---- pages from elsewhere ----
 * Adds a page to the sidebar, after Mind and before Privacy; registering
 * the same title again replaces its draw function. `draw` gets the page's
 * content area (already clipped) each frame Settings is open on that page,
 * below the page title, and reads input from A.in like the rest of the
 * shell. Use settings_palette() and settings_font() to match the built-in
 * pages. Safe to call at any time, including from a constructor. */
void settings_register_page(const char *title, void (*draw)(rect r));
/* The colours of the page area, corrected for what is behind it. */
const palette *settings_palette(void);
enum { SF_BODY, SF_SMALL, SF_SECTION, SF_TITLE };
font *settings_font(int role);
/* A section heading at y inside the content area; returns where the
 * section's content starts. */
int settings_section(rect content, int y, const char *title);

/* ---- kept choices (/etc/os/settings.conf) ---- */
const char *settings_get(const char *key, const char *fallback);
int settings_get_int(const char *key, int fallback);
void settings_set(const char *key, const char *value);
void settings_set_int(const char *key, int value);

/* ---- light and dark (through shell/theme.c when it is there) ---- */
bool appearance_dark(void);
bool appearance_can_switch(void);
void appearance_set_dark(bool dark);

/* ---- the screen ----
 * Switches resolution and brings the shell along: frame and wallpaper
 * reallocated, the pointer and windows kept on screen. */
bool shell_set_resolution(int w, int h);

/* Erases everything kept about them (desktop.c; Privacy uses it). */
void forget_everything(void);
