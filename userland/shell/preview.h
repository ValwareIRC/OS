#pragma once
/* The preview: every change to the look of the space, from a small tweak to
 * a whole new design, is made first on a copy, shown in its own window (and
 * at the end of onboarding, in the middle of the screen). A new design is
 * drawn as the AI writes it: the wallpaper first, then each piece as it
 * arrives, gliding into place. They can talk to the companion while it
 * works, point at something in the picture, compare with how it was, and
 * only Apply makes it real. */
#include <stdbool.h>

#include "design.h"
#include "gfx.h"

struct fm_doc;

/* The document a change should be made to: the preview's, which starts as
 * a copy of the desktop if there is no preview yet (the window opens).
 * NULL if there is nothing to copy. */
struct fm_doc *preview_target(void);
/* The proposal as it stands (what a redesign starts from), or NULL. */
struct fm_doc *preview_doc(void);
/* A preview is open (its window on the desktop, or the onboarding one). */
bool preview_showing(void);
/* An edit was made to the preview's document (it glides into place). */
void preview_edited(void);

/* A new design is being written (design_begin calls this). */
void preview_drafting(void);
/* The finished design (the document is the preview's from now on). */
void preview_draft(struct fm_doc *doc, const design *d);
/* The design didn't come through: say why, keep what was there. */
void preview_failed(const char *why);

/* Every frame: reads the design as it is written. */
void preview_frame(void);
/* Draws the preview, the companion's last words and the controls in r. */
void preview_body(rect r, bool focused);
/* The window was closed from its title bar: nothing changes. */
void preview_closed(void);
void preview_apply(void);
void preview_discard(void);

/* For the companion's instructions while a preview is open, else "". */
const char *preview_prompt(void);
