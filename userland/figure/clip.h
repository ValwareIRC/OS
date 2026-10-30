#pragma once
/* Animation clips: each bone's rotation, frame by frame, plus where the hips
 * travel and when a hand or foot is planted on something (contacts). The
 * same text format comes from our Blender script ("Send to OS", recordings
 * made with FreeMoCap) and from the clips the OS makes itself.
 *
 *   osclip 1
 *   name wave
 *   fps 30
 *   frames 45
 *   loop 0
 *   bones rightUpperArm rightLowerArm rightHand
 *   root 0                      (1: each frame ends with the hips' offset x y z)
 *   contact 0 20 leftFoot ground
 *   f <x y z w per bone> [x y z]
 *   ...                          (one f line per frame) */
#include "fig.h"

#define CLIP_MAX_BONES 64
#define CLIP_MAX_CONTACTS 24

typedef struct {
    int f0, f1;          /* frames */
    char bone[24];
    char kind[12];       /* ground, ledge, rim, wall, hand */
    int b;               /* the model's bone, once bound */
} clip_contact;

typedef struct {
    char name[32];
    float fps;
    int frames;
    bool loop;
    int nb;
    char bone[CLIP_MAX_BONES][24];
    int map[CLIP_MAX_BONES];   /* the model's bone index, -1 if it has none */
    quat *rot;                 /* frames * nb */
    v3 *root;                  /* frames, or NULL */
    int ncontact;
    clip_contact contact[CLIP_MAX_CONTACTS];
} clip;

bool clip_parse(clip *c, const char *text);
/* the text form; the caller frees it */
char *clip_format(const clip *c);
void clip_free(clip *c);
void clip_bind(clip *c, const fmodel *m);
float clip_length(const clip *c);
/* the clip at time t (seconds) laid over the pose with weight w (0..1) */
void clip_apply(const clip *c, float t, fpose *p, float w);
/* the hips' offset at time t (zero if the clip has none) */
v3 clip_root(const clip *c, float t);

/* ---- making clips in code: key poses, eased between ---- */
typedef struct {
    float t;                    /* seconds */
    v3 euler[CLIP_MAX_BONES];   /* per bone: rotation about X, then Y, then Z */
} clip_key;

/* bones: names; keys: in time order. Eased (smoothstep) between keys. */
bool clip_from_keys(clip *c, const char *name, float fps, bool loop, int nb, const char *const *bones, const clip_key *keys,
                    int nkeys);

/* the clips the OS makes itself, until recorded ones replace them */
bool clip_builtin(clip *c, const char *name);
extern const char *const clip_builtin_names[];
