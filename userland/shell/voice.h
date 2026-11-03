#pragma once
#include "gfx.h"
#include <stdbool.h>

/* Speaking and listening, on this computer (voice.c). */
bool voice_can_speak(void);
bool voice_can_hear(void);
void voice_set_enabled(bool on);
bool voice_enabled(void);
void voice_say(const char *text); /* interrupts anything still being said */
/* Each line said gets a number: the last one, and whether it was cut off
 * (hushed) rather than finished. */
unsigned voice_said(void);
bool voice_cut(unsigned line);
void voice_hush(void);
bool voice_speaking(void);

void voice_listen_start(void);
void voice_listen_stop(void); /* then the words arrive through voice_poll_heard */
bool voice_listening(void);
/* An app has the microphone (the companion API): the companion doesn't
 * listen until it gives it back. NULL gives it back. */
void voice_lend_mic(const char *to);
const char *voice_mic_lent(void);
bool voice_hearing(void);     /* turning the recording into words */
char *voice_poll_heard(void); /* malloc'd text, or NULL */
bool voice_toggle_pressed(void);
void voice_indicator(int cx, int cy, color accent);

/* Its voice, chosen in conversation (voice.c): Kokoro voices (af_heart, bm_george, pf_dora, zf_xiaoxiao...),
 * each heard first from a short clip and downloaded once chosen; or a cloud voice ("openai:nova",
 * "elevenlabs:<voice id>") for people with a key. */
bool voice_preview(const char *name);       /* plays its clip; false if there is none */
const char *voice_use(const char *name);    /* chooses it (downloading it first if needed); what happened, in words */
const char *voice_current(void);            /* "af_heart", "openai:nova", or "" (the stand-in voice) */
void voice_set_speed(float speed);          /* 0.7 .. 1.4 */
/* A voice of its own: "af_heart 60, bm_george 40" blended into a new one (fetched first if needed), and shaping
 * ("pitch -2 formant 1 warmth 0.3 breath 0.2 effect room", or "reset"): what happened, in words. */
const char *voice_mix(const char *spec);
const char *voice_shape(const char *spec);
const char *voice_shape_now(void); /* the shaping, as os-voicefx's arguments ("" none) */
void voice_tone(const char *name);   /* how the next words are said: happy, sad, whisper... ([[tone:]]) */
void voice_poll(void);                      /* a download finishing */
/* What the companion is told about choosing a voice. */
const char *voice_prompt(void);
