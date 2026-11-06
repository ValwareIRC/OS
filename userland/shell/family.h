#pragma once
/* The child lock (family.c): a computer a grown-up set up for a child. What is locked and the grown-up's password
 * are init's to keep and check (init/family.inc): the desktop runs as the child and never holds the password
 * longer than a grown-up's few minutes with it open.
 *   what is locked, each on or off:   't' how long a day and at what hours (time)
 *                                     'w' the web and topics, for their age
 *                                     's' Settings and the design (putting a change back never is)
 *                                     'i' installing and removing apps
 *                                     'a' the companion's limits (no commands, no deleting, nothing risky)
 * When something locked is asked for, a small box asks for the grown-up's password; once typed, the computer
 * stays open to them for ten minutes. "Time's up for today" covers the screen the same way: a grown-up can add
 * time. */
#include <stdbool.h>
#include <stddef.h>
#include "controls.h"
#include "font.h"
#include "gfx.h"

void family_boot(void);  /* at the start: what init keeps (it answers at once) */
bool family_child(void); /* this computer is set up for a child */
bool family_locks(char what); /* locked now: set up so, and no grown-up has it open */
bool family_open(void);       /* a grown-up typed the password a moment ago */
void family_close(void);      /* ...and is done: locked again at once */

/* A grown-up's password first ("Installing Minecraft"): then(arg) once it is right, nothing if they cancel.
 * If nothing is locked or a grown-up has it open, then(arg) at once. */
void family_ask(const char *doing, void (*then)(const char *arg), const char *arg);
bool family_asking(void);   /* the box is up: the screen underneath gets no input */
void family_ask_draw(void); /* the box, over everything (main.c, after the frame) */

/* Every frame (main.c, after the lock screen): the time counted; time's up or outside the hours set, the whole
 * screen says so and takes the input (true). */
bool family_frame(void);

/* Inside a window (Settings, the Terminal): true when it may be used; else it draws, in r, what is locked and the
 * password field, and returns false. `lock` as for family_locks; 0: always, on a child's computer, until a
 * grown-up opens it (Settings > Child lock). */
bool family_gate(rect r, char lock, const char *what, const palette *p, font *body, font *small);

/* The child, as the grown-up told it (never the password): their name, pronouns and date of birth. */
const char *family_name(void);
const char *family_pronouns(void);
const char *family_birth(void); /* "YYYY-MM-DD" or "" */
int age_from_birth(const char *birth); /* years, or -1 */
int family_minutes_left(void);  /* today, or -1 with no limit */

/* For the companion's instructions: who it is talking to and the limits (empty for a grown-up's computer). */
void family_prompt(char *out, size_t n);

/* The grown-up's set-up, as a form: in onboarding (onboarding true: "Hand over to NAME") and in Settings > Child
 * lock (changes, a new password, taking it away). Returns the height used. */
int family_form(rect c, int y, const palette *p, font *body, font *small, font *head, bool onboarding);
bool family_handed_over(void); /* onboarding: the grown-up finished and handed it over (once) */
void family_form_reset(void);

/* Settings > Child lock (a built-in page: height used). */
int family_settings_page(rect c, int y);
