#pragma once
#include <stdbool.h>

#define PERSONA_WANT 15 /* traits read in the person, and chosen for her */

/* Who the companion is (persona.c). While they talk she reads the person's
 * character, [[trait:Name]] from a fixed list, never shown or spoken of;
 * once 15 are known, the companion's own 15 are chosen to complement them and
 * her personality is written from those, each in a context of its own. The
 * core stays; a short "lately" drifts every few days from what they talk
 * about. Kept in STATE_DIR/persona*. */

void persona_load(void);
void persona_reset(void);       /* starting over (onboarding again) */
int persona_read(void);         /* how many of the person's traits are known */
bool persona_ready(void);       /* her personality is written */
/* Takes [[trait:...]] tags out of a reply (in place), keeping what they say. */
void persona_take(char *text);
/* What the onboarding conversation is told: the list, how many are known. */
const char *persona_onboarding_prompt(void);
/* What the companion is told: who she is, or (not yet) to get to know them. */
const char *persona_companion_prompt(void);
/* Builds her when it is time, and lets her drift; each frame. */
void persona_poll(void);
/* An existing person with no personality for her yet: a short chat, once. */
bool persona_wants_chat(void);
