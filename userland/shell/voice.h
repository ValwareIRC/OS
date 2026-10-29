#pragma once
#include "gfx.h"
#include <stdbool.h>

/* Speaking and listening, on this computer (voice.c). */
bool voice_can_speak(void);
bool voice_can_hear(void);
void voice_set_enabled(bool on);
bool voice_enabled(void);
void voice_say(const char *text); /* interrupts anything still being said */
void voice_hush(void);
bool voice_speaking(void);

void voice_listen_start(void);
void voice_listen_stop(void); /* then the words arrive through voice_poll_heard */
bool voice_listening(void);
bool voice_hearing(void);     /* turning the recording into words */
char *voice_poll_heard(void); /* malloc'd text, or NULL */
bool voice_toggle_pressed(void);
void voice_indicator(int cx, int cy, color accent);
