#pragma once
#include "gfx.h"
#include <stdbool.h>

/* The companion's emblem: how it shows itself (emblem.c). The person
 * chooses it; it shows what the companion is doing. */
/* EM_COUNT built-in looks; then their own: a picture, a drawing the AI made, a 3D model */
enum emblem_kind { EM_GEM, EM_ORB, EM_EIGHTBALL, EM_EARTH, EM_PLANET, EM_HEART, EM_MOON, EM_COUNT,
                   EM_PICTURE = 50, EM_DRAWN, EM_MODEL };
/* Their own picture (a file on this computer) becomes the emblem; false if it can't be read. */
bool emblem_set_picture(const char *path);
enum emblem_state { ES_IDLE, ES_THINKING, ES_LISTENING, ES_SPEAKING, ES_WORKING, ES_DONE, ES_ERROR };

int emblem_kind(void);
void emblem_set(int kind);
const char *emblem_name(int kind);
int emblem_from_name(const char *s); /* -1 if it isn't one */
/* What it is doing now (`active`: the caller knows it's busy); level 0..1 while it speaks. */
int emblem_state(bool active, float *level);
/* Others set this while they work on something (the web, a command), for the circling light. */
extern bool emblem_busy_hint;

/* The chosen emblem, centred at cx,cy, about r across from centre to edge,
 * in the design's colours a and b. */
void emblem_draw(canvas *c, float cx, float cy, float r, color a, color b, bool active);
/* One look in one state (galleries, previews). */
void emblem_draw_kind(canvas *c, int kind, float cx, float cy, float r, color a, color b, int state, float level);

/* Looks of their own, drawn elsewhere: a drawing the AI wrote (emblem_draw.c,
 * a small canvas script) and a 3D model (os-figure renders it). False when
 * there is nothing to show yet; the gem stands in. */
bool emblem_draw_drawing(canvas *c, float cx, float cy, float r, color a, color b, int state, float level, float t);
bool emblem_draw_model(canvas *c, float cx, float cy, float r, int state, float level);
/* A drawing the AI wrote of itself (a canvas script with draw(w, h, t)) becomes the emblem. */
bool emblem_set_drawing(const char *script);
/* A 3D model of theirs (.glb or .gltf) becomes the emblem; false if it can't be read. */
bool emblem_set_model(const char *path);
/* A gesture happened (a recorded clip or [[act:...]]): looks that can't wave hop and spin instead. */
void emblem_act(void);
