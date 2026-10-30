#pragma once
#include "head.h"

enum { EYE_ROUND, EYE_ALMOND, EYE_DROOPY, EYE_SHARP, EYE_SLEEPY };
enum { PUPIL_ROUND, PUPIL_SLIT, PUPIL_STAR, PUPIL_HEART, PUPIL_RING };
enum { BROW_SOFT, BROW_STRAIGHT, BROW_THICK, BROW_ARCHED, BROW_SHORT };
enum { MOUTH_SMALL, MOUTH_CAT, MOUTH_FANG };
enum { NOSE_DOT, NOSE_LINE, NOSE_NONE };

/* How the face is drawn: chosen once, with the rest of the look. */
typedef struct {
    rgb skin, iris_top, iris_bot, white, lash, brow, lip, blush_col, pupil_col;
    int eye_shape, pupil, highlights, lashes, brow_style, mouth_style, nose;
    float eye_size;  /* 0.75 .. 1.3 */
    float eye_gap;   /* 0.7 .. 1.3 */
    float eye_tilt;  /* -1 .. 1: outer corners down or up */
    float blush;     /* 0 .. 1 */
    bool mole;       /* a beauty mark under one eye */
} face_params;

enum { FE_OPEN, FE_HALF, FE_CLOSED, FE_HAPPY, FE_WIDE, FE_SAD, FE_ANGRY, FE_WINK_L, FE_WINK_R, FE_SPARKLE, FE_HEART,
       FE_DIZZY, FE_TEARS };
enum { FB_NEUTRAL, FB_RAISED, FB_FURROW, FB_WORRY };
enum { FM_CLOSED, FM_SMILE, FM_OPEN_SMILE, FM_A, FM_I, FM_U, FM_E, FM_O, FM_FROWN, FM_SMALL_O, FM_CAT, FM_GRIT };

/* How it looks this moment. */
typedef struct {
    int eyes, brows, mouth;
    float open;      /* speech: how far the mouth is open, 0..1 */
    float gx, gy;    /* where the eyes look, -1..1 (right and up on the picture) */
    float blush;     /* extra blush */
} face_state;

face_params face_defaults(void);
void face_draw(ftex *t, const head_params *hp, const face_params *fp, const face_state *fs);
