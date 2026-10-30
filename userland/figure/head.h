#pragma once
#include "fig.h"

typedef struct {
    float w, h, d;   /* width, crown to chin, front to back (metres) */
    float jaw;       /* width at the chin, as a share of w (0.2 pointed .. 0.6 square) */
    float chin;      /* how far the chin reaches down (0..1) */
    float cheek;     /* fullness (0..1) */
    float flat;      /* how flat the front of the face is (0..1) */
    float eye_y;     /* eye line, from the head's centre, as a share of h (negative: lower) */
    int ears;        /* 0 none (hidden), 1 round, 2 elf */
} head_params;

typedef struct {
    v3 center;       /* model space */
    head_params hp;
    int tex;         /* the face texture */
    int mat;         /* the face material */
    int skin_mat;
} head_info;

head_params head_defaults(void);
/* The head's surface in the direction (ux, uy, uz) of a unit sphere, from its centre. */
v3 head_point(const head_params *hp, float ux, float uy, float uz);
/* ...by angles: th around (0 the front, +pi/2 the figure's left), phi up from the middle */
v3 head_point_at(const head_params *hp, float th, float phi);
v3 head_normal_at(const head_params *hp, float th, float phi);
/* The head on `bone`, centred at `center`; the face texture is texw x texw. */
head_info head_build(fmodel *m, const head_params *hp, v3 center, int bone, rgb skin, int texw);
/* Where a point on the face (metres from the head's centre, y up) is in the face texture (0..1). */
static inline void head_uv(const head_params *hp, float x, float y, float *u, float *v) {
    *u = 0.5f + x / hp->w;
    *v = 0.5f - y / hp->h;
}
