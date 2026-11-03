#pragma once
#include <stdbool.h>

struct conversation;

/* The companion's body (agent.c): what it feels of the system at every moment, and what it can do with it.
 *
 * It is the system, not someone watching it. What happens (a window opens, they close an app, a download
 * lands, it runs something) is kept as its own sense of itself and is in front of it whenever it thinks;
 * nothing of it is announced to it, nor by it.
 *
 * It acts with tags in its replies; they run, what they return comes back to it, and it goes on, step
 * after step, until the job is done. Everything it changes is kept in a journal and undone with [[undo]]
 * where it can be. Only what can't be undone at all (wiping a disk) waits for their yes. */

/* Something happened: 'p' they did it, 's' it did, 'o' the system. Felt, never sent to it by itself. */
void body_feel(char who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Its body as it feels it now, and how it moves it: for its system prompt. */
const char *body_prompt(void);

/* Runs the body tags in its reply (taking them out of it). True when they will come back with results and
 * it goes on (agent_tick sends them when they're ready). */
bool agent_take(struct conversation *c, bool from_person);
void agent_tick(void);
bool agent_busy(void);
const char *agent_doing(void); /* what it is doing now, in a few words, or "" */
void agent_stop(void);         /* they said stop: the steps end */
void agent_new_request(void);  /* they asked for something: a fresh count of steps */
bool agent_installing(void);   /* an install it asked for is under way */

/* NAME, directly in the state folder, is the system's alone: security settings, app grants, keys (agent.c) */
bool agent_state_private(const char *name);
