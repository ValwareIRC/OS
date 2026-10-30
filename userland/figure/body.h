#pragma once
/* The body: a skeleton laid out from proportions, with VRM's humanoid bone
 * names (so animations made for VRM models fit), and a skin made from a
 * smooth implicit shape. */
#include "head.h"
#include "sdf.h"

typedef struct {
    float height;    /* metres, floor to crown (1.2 .. 1.85) */
    float heads;     /* how many heads tall (3 chibi .. 7.5) */
    float shoulders; /* shoulder width, share of height (0.17 .. 0.24) */
    float hips;      /* 0.8 .. 1.25 */
    float waist;     /* 0.8 (narrow) .. 1.2 */
    float chest;     /* 0 .. 1.4 */
    float legs;      /* leg length, share of height (0.42 .. 0.54) */
    float arms;      /* 0.9 .. 1.1 */
    float neck;      /* 0.7 .. 1.4 */
    float build;     /* limbs: 0.8 slim .. 1.3 sturdy */
    rgb skin;
} body_params;

/* VRM humanoid bones */
enum {
    BN_HIPS, BN_SPINE, BN_CHEST, BN_UPPERCHEST, BN_NECK, BN_HEAD, BN_LEYE, BN_REYE,
    BN_LSHOULDER, BN_LUPPERARM, BN_LLOWERARM, BN_LHAND,
    BN_RSHOULDER, BN_RUPPERARM, BN_RLOWERARM, BN_RHAND,
    BN_LUPPERLEG, BN_LLOWERLEG, BN_LFOOT, BN_LTOES,
    BN_RUPPERLEG, BN_RLOWERLEG, BN_RFOOT, BN_RTOES,
    BN_BODY_COUNT,
    /* fingers: per hand thumb (metacarpal, proximal, distal), then index,
     * middle, ring, little (proximal, intermediate, distal) */
    BN_LFINGERS = BN_BODY_COUNT,
    BN_RFINGERS = BN_LFINGERS + 15,
    BN_COUNT = BN_RFINGERS + 15
};

extern const char *const body_bone_names[BN_COUNT];

/* the body's shape as distances: what clothes are cut from */
#define BODY_MAX_PRIMS 48
typedef struct {
    int kind;          /* 0 capsule, 1 ellipsoid, 2 torso loft */
    v3 a, b;           /* capsule ends / ellipsoid centre (a) */
    float ra, rb;      /* capsule radii */
    v3 r;              /* ellipsoid radii */
    quat q;
    int group;         /* 0 torso, 1.. a limb (joined to the torso only) */
    float k;           /* how smoothly it joins its group */
    int bone, bone2;   /* the bones it moves with (bone2: shares by w2) */
    float w2;
} body_prim;

#define LOFT_N 10
typedef struct {
    body_params bp;
    int bone[BN_COUNT];          /* model bone index of each humanoid bone */
    float H, s;                  /* height, and scale against 1.52 m */
    float crotch_y, hips_y, waist_y, chest_y, neck_y, shoulder_y, chin_y;
    float shoulder_x, hip_x;
    v3 elbow[2], wrist[2], knee[2], ankle[2];
    head_params hp;
    v3 head_center;
    /* the torso, as cross-sections from the crotch up */
    float loft_y[LOFT_N], loft_a[LOFT_N], loft_b[LOFT_N], loft_z[LOFT_N];
    body_prim prim[BODY_MAX_PRIMS];
    int nprim;
    int skin_mat;
    int mesh;                    /* the body's mesh */
} body_info;

body_params body_defaults(void);
/* the skeleton and the shape (no meshes yet) */
void body_layout(fmodel *m, const body_params *bp, body_info *bi);
/* the skin mesh, from the shape, `cell` metres fine */
bool body_build_mesh(fmodel *m, body_info *bi, int mat, float cell);
/* hands: palm and fingers, on their bones */
void body_build_hands(fmodel *m, body_info *bi, int mat);
/* the body's distance field (for clothes) */
float body_sdf(void *bi, v3 p);
/* skin weights for a point near the body: up to 4 bones */
void body_weights(const body_info *bi, v3 p, uint8_t bi4[4], float bw4[4]);
/* the relaxed pose: arms down, a little bend at the elbows */
void body_pose_relaxed(const body_info *bi, fpose *p);
