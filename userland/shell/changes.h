#pragma once
/* What changed, kept so it can be put back (changes.c): every setting chosen (settings_set) and every change to
 * the design (each time the desktop's document is saved different from before), newest first, the last thirty.
 * The companion is told the recent ones (changes_prompt); when they seem upset or stuck after one, it offers to
 * put it back and does on their yes ([[putback]] the newest, [[putback: N]] the N-th). Putting back is never
 * locked, not even on a child's computer. */
#include <stdbool.h>
#include <stddef.h>

void changes_note_setting(const char *key, const char *old_value, const char *new_value);
void changes_note_design(const char *old_text); /* the whole document as it was before */
void changes_prompt(char *out, size_t n);       /* the recent ones, numbered, for its instructions */
/* The n-th newest (1 the newest) put back; *said what it was ("the design, as it was at 14:05"). */
bool changes_put_back(int n, char *said, size_t len);
