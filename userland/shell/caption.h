#pragma once
#include <stdbool.h>
#include "gfx.h"

/* The companion's voice on the desktop when its conversation is closed: a
 * small card near its light (the orb, or wherever the design lets them
 * reach it) that never covers the window they are working in. It fades in,
 * stays as long as the words take to read, and fades out; a click opens the
 * conversation, the x puts it away. (caption.c) */
enum {
    CAPTION_SPEAK = 1,    /* also say it aloud (voice_say) */
    CAPTION_STICKY = 2,   /* stays until they click it or put it away */
    CAPTION_IF_QUIET = 4, /* only if no other line is showing */
};
/* Shows one line from the companion. Returns false when it was not shown:
 * empty, the conversation is open (add it there instead), or IF_QUIET and
 * something else is up. A new line replaces the one showing. */
bool caption_say(const char *text, int flags);
void caption_dismiss(void);
bool caption_showing(void);

/* Hooks for the desktop: where the companion's orb was drawn this frame
 * (fm_host_draw_orb), clicks on the card before anything else sees them
 * (start of the frame), and the card itself (end of the frame). */
void caption_anchor(rect orb);
void caption_input(void);
void caption_draw(void);
