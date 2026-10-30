#pragma once
/* How the companion is present on the desktop: as a light (the orb), as a
 * face in its round frame, or as a face that sometimes goes out walking.
 * The face is drawn by os-figure (userland/figure), a process of its own;
 * this is the shell's side: starting it, showing its frames in the frame,
 * telling it where the pointer is, when she thinks, listens and speaks. */
#include "app.h"

enum { PRESENCE_LIGHT, PRESENCE_FACE, PRESENCE_WANDER };

void presence_init(void);
int presence_mode(void);
void presence_set_mode(int mode);
/* every pass of the main loop */
void presence_poll(void);
int presence_fds(int *fds, int max);
/* her portrait in r (a circle); false when there is no face to show (the
 * light is drawn instead) */
bool presence_draw_home(canvas *c, rect r, bool active);
/* her own frame in a corner, when the design has no orb big enough */
void presence_draw_corner(canvas *c);
/* her portrait in a design's <avatar>: shape circle|rect, tint none|codec */
bool presence_draw_avatar(canvas *c, rect r, bool active, const char *shape, const char *tint, const char *shot);
/* runs and removes her body's tags in a reply: [[act:...]] [[mood:...]] [[figure:...]] */
void presence_tags(char *text, bool act);
/* lines for the companion's prompt about her body ("" when she has none) */
const char *presence_prompt(void);
/* her look's markup (the saved one, or the default) */
const char *presence_look(void);
bool presence_set_look(const char *markup);
