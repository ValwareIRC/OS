#pragma once
#include <stdbool.h>

/* Keyboard layouts (keymap.c): Linux key codes to the characters printed on
 * that country's keys, with AltGr, Caps Lock and dead accents. */

int keymap_count(void);
const char *keymap_id(int i);   /* "us", "gb", "de", ... */
const char *keymap_name(int i); /* "English (US)" */
/* Switches layout by id; false if there is no such layout. */
bool keymap_set(const char *id);
const char *keymap_current(void);
/* One key press: writes up to two characters (a dead accent that doesn't
 * combine comes out on its own, then the key) and returns how many. */
int keymap_translate(int code, bool shift, bool altgr, bool caps, int out[2]);
