#pragma once
#include "gfx.h"
#include <stdbool.h>
#include <stddef.h>

/* The terminal app: a shell on a pty, drawn into a window body. */
void term_draw(rect body, bool focused, color surface, color text, float px);
void term_close(void);
const char *term_title(void); /* set by the program (OSC 0/2), or NULL */
void term_dump(char *out, size_t len);
