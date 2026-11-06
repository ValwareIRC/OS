#pragma once
#include "gfx.h"
#include <stdbool.h>

/* The screen reader (reader.c): the companion reads what has their attention, anywhere: the OS's own screens (what
 * the controls say of themselves, the design's elements, menus) and other apps (os-a11y: the accessibility bus),
 * in its own voice and words, briefly as they move and more fully when they stop. Ctrl+Alt+R on or off,
 * Ctrl+Alt+W where am I, Ctrl+Alt+A say it again, Ctrl+Alt+D the whole screen, described, Ctrl+Alt+Right / Left /
 * Enter the next thing, the one before, use it; any key cuts it off. */

/* What a control of the OS's own is, said by the control as it is drawn (ui.c, controls.c, settings.c):
 * role "button" "switch" "field" "slider" "choice" "label" "tile"; state "on", "off", "empty", "says ...". */
void a11y_note(rect r, const char *role, const char *label, const char *state, bool focused);
void a11y_frame_begin(void); /* (a frame is drawn: what was noted before goes) */

bool reader_on(void);
void reader_set(bool on);
bool reader_full(void);       /* says more: a fuller description when they stop on something */
void reader_set_full(bool on);
void reader_keys(void);       /* after input: its keys, and any key cuts it off */
void reader_tick(void);       /* each turn: what has their attention, said when it changes */
void reader_draw(void);       /* the first screens: how to have them read aloud */
/* Something that happened, said whatever has their attention (a notification, a message, a window opening). */
void reader_announce(const char *text);
