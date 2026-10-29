#pragma once
#include <stdbool.h>

/* Screenshots and screen recording (capture.c): Print Screen opens a bar of
 * choices over the frozen screen; recordings run with a stop pill on top. */

/* Opens the bar (kind 0 screenshot, 1 record, -1 as last time). */
void capture_open(int kind);
/* A whole-screen screenshot at once, no questions. */
void capture_screenshot_now(void);
bool capture_active(void);
bool capture_recording(void);

/* desktop.c: input first (the bar takes it all while open)... */
void capture_input(void);
/* ...and drawing last, over everything. */
void capture_draw(void);
/* main.c: each finished frame, before and after the pointer is drawn. */
void capture_frame(bool pointer_drawn);
