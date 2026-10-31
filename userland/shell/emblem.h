#pragma once
#include "gfx.h"
#include <stdbool.h>

/* The companion's own look (emblem.c). Nothing is built in: until she has
 * one, nothing is drawn. It is made in conversation, fitted to the design and
 * to what the person wants, and shows what she is doing. */
enum emblem_kind { EM_NONE, EM_PICTURE = 50, EM_DRAWN, EM_MODEL };
/* Their own picture (a file on this computer) becomes her look; false if it can't be read. */
bool emblem_set_picture(const char *path);
enum emblem_state { ES_IDLE, ES_THINKING, ES_LISTENING, ES_SPEAKING, ES_WORKING, ES_DONE, ES_ERROR };

int emblem_kind(void);
void emblem_clear(void); /* no look of her own (she's a face, or nothing) */
const char *emblem_name(int kind);
int emblem_from_name(const char *s); /* -1 if it isn't one */
/* What it is doing now (`active`: the caller knows it's busy); level 0..1 while it speaks. */
int emblem_state(bool active, float *level);
/* Others set this while they work on something (the web, a command). */
extern bool emblem_busy_hint;

/* Her look, centred at cx,cy, about r across from centre to edge, in the
 * design's colours a and b. False when there is nothing to show. */
bool emblem_draw(canvas *c, float cx, float cy, float r, color a, color b, bool active);
/* One look in one state. */
bool emblem_draw_kind(canvas *c, int kind, float cx, float cy, float r, color a, color b, int state, float level);

/* Where she sits, her choice: "slot" (the design's own spot for her, its
 * <ai>) or "X Y SIZE" (the centre as fractions of the screen, the size in
 * pixels). False if it isn't one. */
bool emblem_set_place(const char *how);
/* Placed freely, and there is a look to show there: the square she sits in, on the screen. */
bool emblem_free_place(rect *r);
/* Where she placed herself, look or not (a face uses it too). */
bool emblem_placed(rect *r);
/* Draws her where she placed herself, if she did (the desktop, under the windows). */
bool emblem_draw_free(canvas *c, color a, color b, bool active);

/* Looks drawn elsewhere: a drawing the AI wrote (a small canvas script) and
 * a 3D model (os-figure renders it). False when there is nothing to show yet. */
bool emblem_draw_drawing(canvas *c, float cx, float cy, float r, color a, color b, int state, float level, float t);
bool emblem_draw_model(canvas *c, float cx, float cy, float r, int state, float level);
/* A drawing the AI wrote of itself (a canvas script with draw(w, h, t)) becomes her look. */
bool emblem_set_drawing(const char *script);
/* A 3D model of theirs (.glb or .gltf) becomes her look; false if it can't be read. */
bool emblem_set_model(const char *path);
/* A gesture happened (a recorded clip or [[act:...]]): looks that can't wave hop instead. */
void emblem_act(void);
