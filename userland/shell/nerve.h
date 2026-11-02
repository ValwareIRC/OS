#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The companion's nervous system (nerve.c, reflex.c).
 *
 * Everything that happens on the computer arrives here as an event: what happened, and who did it (the
 * person, the companion itself, or the system on its own). Nothing goes to it by itself. Reflexes decide
 * what each event is to it, the way a body does before a mind is bothered:
 *   ignore    nothing to keep
 *   remember  part of what it knows; it is in front of it the next time it thinks, and not before
 *   wake      worth its attention now: it is given a moment to judge it (act, speak, or let it be)
 *   urgent    the same, at once, whatever they are doing
 * What the person or it did itself is never news: rules settle it before any model is asked. The rest is
 * judged by a small model on this computer (reflex.c), never by the service its main mind runs on. */

enum nerve_actor { BY_PERSON = 'p', BY_AI = 's', BY_SYSTEM = 'o' };
enum nerve_verdict { V_PENDING, V_IGNORE, V_REMEMBER, V_WAKE, V_URGENT };

/* Something happened. weight: 0 routine .. 3 critical (a hint for the reflexes, not the decision). actor 0:
 * worked out here (the person if they just touched the computer, it if its body is acting, else the system). */
void nerve(char actor, const char *kind, int weight, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* Its body acting: what the system does meanwhile is its own. */
void nerve_ai_hands(bool acting);
/* The person touched the computer (input.c / the shell's input). */
void nerve_person_touched(void);

/* What it knows of lately (remembered events, and what it and they did): for every prompt. */
const char *nerve_context(void);
/* Each frame: reflexes, then waking it when one earned it and the moment is right. */
void nerve_tick(void);
/* The words of an event-wake it answered with nothing: true if this note was one (desktop.c drops both). */
bool nerve_is_wake_note(const char *text);

/* Reflex engines (reflex.c): which decides, and whether it's ready. */
const char *reflex_engine(void);   /* "llm", "jev" or "rules" */
const char *reflex_status(void);   /* in words, for Settings and for it */
