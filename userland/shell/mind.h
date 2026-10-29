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
