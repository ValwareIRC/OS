#pragma once
#include <stdbool.h>

/* The OS's own offline mind (mind.c). */
bool mind_builtin_present(void); /* the model came with this image */
bool mind_builtin_running(void);
void mind_builtin_start(void);   /* in the background; it answers once the model is loaded */
void mind_builtin_stop(void);
/* Its server is still alive (loading or answering): safe to ask from a job
 * child, which can't wait on the shell's children. */
bool mind_builtin_alive(void);
/* Its model is loaded and it can answer (false while it wakes up). */
bool mind_builtin_ready(void);
/* Seconds since it began waking (0 when it isn't). */
int mind_builtin_waking_for(void);
/* About how long until it answers, from now (0 when it can): for an app waiting on it. */
int mind_builtin_load_seconds(void);
int mind_builtin_context(void); /* tokens it was started with room for (0: not running) */
/* From the main loop: starts it once a model still being read from the disc is whole (mind_builtin_start
 * asked meanwhile). */
void mind_builtin_tick(void);
/* It ended by itself (crashed, or killed for memory): true once, with why in plain words. */
bool mind_builtin_died(char *why, int n);

/* Standing in: their mind is elsewhere and stopped answering (`why`): they are asked whether the one on this
 * computer may think instead until it's back (never switched without their yes). */
void mind_fallback_offer(const char *why);
void mind_fallback_draw(void); /* the question, over everything, while it is asked */
/* From the main loop: true once, when the stand-in is ready and what went unanswered should be asked again. */
bool mind_fallback_tick(void);
void mind_fallback_forget(void); /* they chose a mind themselves (Settings): no stand-in, nothing to go back to */
bool mind_fallback_pending(void); /* asking them, or the stand-in still starting: nothing else is sent meanwhile */
bool mind_fallback_asking(void);  /* the question is on screen (it holds the pointer and keys) */
/* An error that says their mind couldn't be reached (no connection, no answer), not that it refused. */
bool mind_unreachable(const char *error);
