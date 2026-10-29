#pragma once
#include <stdbool.h>
#include "gfx.h"

/* Notes: a plain-text editor (notes.c). Each note is a file in
 * /root/Documents named by its title; it saves itself a moment after each
 * change. Selection with the mouse or Shift+arrows, Ctrl+C/X/V, Ctrl+Z/Y
 * (or Ctrl+Shift+Z), Ctrl+A, Ctrl+S, Ctrl+N for a new note. */
void notes_draw(rect r);
/* Loads a file into Notes (the window is the caller's). False, with a
 * toast, if it isn't text. */
bool notes_open(const char *path);
/* Forget me: the editor starts over empty. */
void notes_forget(void);
