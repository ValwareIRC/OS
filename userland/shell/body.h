#pragma once
#include <stdbool.h>

/* Onboarding's question about the companion's body (body.c): a light, the
 * face the OS makes, or a VRM model of theirs. Asked once, while the AI makes
 * their space. It takes the pointer and keys while it's up: take_input at the
 * start of the frame, draw at its end. */
void body_take_input(void);
void body_draw(void);
bool body_asking(void);
/* .vrm files in their home and its usual folders (paths into out). */
int body_find_vrms(char out[][300], int max);
/* The looks to choose from, as a gallery of living previews in r (cols
 * across), in the design's colours on the desktop; *picked when one was
 * chosen. Returns its height. */
#include "gfx.h"
int body_gallery(rect r, int cols, bool desk, bool *picked);
