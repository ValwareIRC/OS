#pragma once
#include "head.h"

enum { HAIR_BOB, HAIR_LONG, HAIR_SHORT, HAIR_TWINTAILS, HAIR_PONYTAIL, HAIR_HIME, HAIR_WAVY, HAIR_BUNS, HAIR_PIXIE,
       HAIR_WOLF, HAIR_BRAID, HAIR_STYLES };
enum { BANGS_FULL, BANGS_SIDE, BANGS_PARTED, BANGS_BLUNT, BANGS_NONE, BANGS_STYLES };

typedef struct {
    int style, bangs;
    float length;   /* back hair: how far below the chin the tips reach, metres (negative: above it) */
    float volume;   /* 0..1 */
    float curl;     /* tips: + outwards, - inwards */
    float part;     /* where the bangs part or sweep from, -1 (her right) .. 1 */
    int ahoge;      /* a lock standing up from the crown */
    rgb color, tip; /* at the roots and at the tips */
} hair_params;

/* A sphere the hair keeps out of (shoulders, chest, neck). */
typedef struct {
    v3 c;
    float r;
} collider;

/* Each lock as a chain of points, for the springs that move it later. */
#define HAIR_MAX_LOCKS 96
#define LOCK_PTS 14
typedef struct {
    int nlocks;
    struct {
        v3 p[LOCK_PTS];
        int first_vert, nverts; /* in the hair mesh */
        float len;
    } lock[HAIR_MAX_LOCKS];
    int mesh;  /* the hair mesh's index in the model */
    int mat;
} hair_info;

hair_params hair_defaults(void);
void hair_build(fmodel *m, const head_info *hi, const hair_params *hp, int bone, const collider *col, int ncol,
                hair_info *out);
