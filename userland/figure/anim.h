#pragma once
/* Life: what makes her look alive in her frame. Breathing, a slow sway,
 * blinking, looking around and at you, moods on her face, gestures, and
 * her mouth moving with her voice. */
#include "clip.h"
#include "figure.h"

enum { MOOD_CALM, MOOD_HAPPY, MOOD_SAD, MOOD_EXCITED, MOOD_SLEEPY, MOOD_CURIOUS, MOOD_PROUD, MOOD_WORRIED, MOOD_COUNT };
extern const char *const mood_names[MOOD_COUNT];

#define LIFE_MAX_CLIPS 32

typedef struct {
    /* what's going on (set from outside) */
    float look_x, look_y;   /* where to look, -1..1 across the frame (x right, y up) */
    bool look_on;           /* following something (the pointer) */
    float speak;            /* voice level now, 0..1 */
    float f1, f2;           /* voice energy in two bands (vowel shape), 0..1 */
    bool speaking;          /* a reply is being spoken */
    int mood;
    bool thinking, listening, dozing;
    float energy;           /* 0 calm .. 1 lively (her manner) */

    /* inside */
    double t;
    double next_blink, blink_start;
    bool double_blink;
    float gx, gy, gtx, gty; /* eye gaze now and where it's going */
    double next_saccade;
    float hx, hy;           /* head look, smoothed */
    float mouth_open;
    int viseme;
    double viseme_until;
    double mood_since;
    int last_mood;
    /* gestures */
    clip clips[LIFE_MAX_CLIPS];
    int nclips;
    int act;                /* clip playing, -1 none */
    double act_start;
    int act_queue;          /* next one, -1 */
    int think_clip;
    double think_since;
    unsigned rng;
} life;

void life_init(life *L, const figure *f, unsigned seed);
void life_free(life *L);
/* a clip from outside (recorded); replaces a built-in one of the same name */
bool life_add_clip(life *L, const figure *f, const char *text);
/* start a gesture by name; false if there is none */
bool life_act(life *L, const char *name);
int life_mood_from_name(const char *name);
/* advance to `now` (seconds) and pose her */
void life_frame(life *L, const figure *f, double now, fpose *pose, face_state *fs);
/* whether anything is moving enough to need frames at full rate */
bool life_busy(const life *L);
