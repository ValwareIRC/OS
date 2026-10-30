/* The body: a skeleton laid out from a few proportions, and a skin made
 * from a smooth implicit shape (the torso as cross-sections from the hips
 * up; limbs as tapered capsules; chest, hips and shoulders as blended
 * ellipsoids), so the joins are smooth and clothes can be cut from the same
 * shape grown by the cloth's thickness. Built in VRM's T-pose. Skin weights
 * come from which parts of the shape are nearest each point, so joints
 * blend as the shape does. Hands are made separately, finger by finger. */
#include <stdlib.h>
#include <string.h>

#include "body.h"
#include "prim.h"

const char *const body_bone_names[BN_COUNT] = {
    "hips", "spine", "chest", "upperChest", "neck", "head", "leftEye", "rightEye",
    "leftShoulder", "leftUpperArm", "leftLowerArm", "leftHand",
    "rightShoulder", "rightUpperArm", "rightLowerArm", "rightHand",
    "leftUpperLeg", "leftLowerLeg", "leftFoot", "leftToes",
    "rightUpperLeg", "rightLowerLeg", "rightFoot", "rightToes",
    "leftThumbMetacarpal", "leftThumbProximal", "leftThumbDistal",
    "leftIndexProximal", "leftIndexIntermediate", "leftIndexDistal",
    "leftMiddleProximal", "leftMiddleIntermediate", "leftMiddleDistal",
    "leftRingProximal", "leftRingIntermediate", "leftRingDistal",
    "leftLittleProximal", "leftLittleIntermediate", "leftLittleDistal",
    "rightThumbMetacarpal", "rightThumbProximal", "rightThumbDistal",
    "rightIndexProximal", "rightIndexIntermediate", "rightIndexDistal",
    "rightMiddleProximal", "rightMiddleIntermediate", "rightMiddleDistal",
    "rightRingProximal", "rightRingIntermediate", "rightRingDistal",
    "rightLittleProximal", "rightLittleIntermediate", "rightLittleDistal",
};

body_params body_defaults(void) {
    return (body_params){.height = 1.52f, .heads = 6.4f, .shoulders = 0.19f, .hips = 1.0f, .waist = 1.0f,
                         .chest = 0.6f, .legs = 0.47f, .arms = 1.0f, .neck = 1.0f, .build = 1.0f, .skin = 0xfbe4d8};
}

/* ---- the shape ---- */

static void add_prim(body_info *bi, body_prim p) {
    if (bi->nprim < BODY_MAX_PRIMS)
        bi->prim[bi->nprim++] = p;
}

static body_prim capsule(v3 a, v3 b, float ra, float rb, int group, float k, int bone, int bone2, float w2) {
    return (body_prim){.kind = 0, .a = a, .b = b, .ra = ra, .rb = rb, .group = group, .k = k, .bone = bone, .bone2 = bone2,
                       .w2 = w2, .q = qid()};
}

static body_prim ellipsoid(v3 c, v3 r, quat q, int group, float k, int bone, int bone2, float w2) {
    return (body_prim){.kind = 1, .a = c, .r = r, .q = q, .group = group, .k = k, .bone = bone, .bone2 = bone2, .w2 = w2};
}

static float cr(float p0, float p1, float p2, float p3, float t) { /* Catmull-Rom */
    return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}

/* the torso: an ellipse at each height, closed at the top and bottom */
static float loft_dist(const body_info *bi, v3 p) {
    const float *Y = bi->loft_y;
    float y = clampf(p.y, Y[0], Y[LOFT_N - 1]);
    int s = 0;
    while (s < LOFT_N - 2 && y > Y[s + 1])
        s++;
    float t = (y - Y[s]) / (Y[s + 1] - Y[s]);
    int i0 = s > 0 ? s - 1 : 0, i3 = s + 2 < LOFT_N ? s + 2 : LOFT_N - 1;
    float a = cr(bi->loft_a[i0], bi->loft_a[s], bi->loft_a[s + 1], bi->loft_a[i3], t);
    float b = cr(bi->loft_b[i0], bi->loft_b[s], bi->loft_b[s + 1], bi->loft_b[i3], t);
    float z = cr(bi->loft_z[i0], bi->loft_z[s], bi->loft_z[s + 1], bi->loft_z[i3], t);
    float d = sd_ellipse2(p.x, p.z - z, fmaxq(a, 0.005f), fmaxq(b, 0.005f));
    float dy = p.y < Y[0] ? Y[0] - p.y : p.y > Y[LOFT_N - 1] ? p.y - Y[LOFT_N - 1] : 0;
    if (dy > 0)
        d = d > 0 ? sqrtf(d * d + dy * dy) : dy;
    return d;
}

static inline float prim_dist(const body_prim *pr, v3 p, float *t) {
    if (pr->kind == 0)
        return sd_capsule(p, pr->a, pr->b, pr->ra, pr->rb, t);
    if (t)
        *t = 0;
    return sd_ellipsoid(p, pr->a, pr->r, pr->q);
}

#define MAX_GROUPS 8

float body_sdf(void *ctx, v3 p) {
    const body_info *bi = ctx;
    float T = loft_dist(bi, p);
    float L[MAX_GROUPS];
    float kj[MAX_GROUPS];
    for (int g = 0; g < MAX_GROUPS; g++)
        L[g] = 1e9f, kj[g] = 0;
    for (int i = 0; i < bi->nprim; i++) {
        const body_prim *pr = &bi->prim[i];
        float d = prim_dist(pr, p, NULL);
        if (pr->group == 0)
            T = smin(T, d, pr->k);
        else {
            L[pr->group] = smin(L[pr->group], d, 0.006f * bi->s);
            kj[pr->group] = fmaxq(kj[pr->group], pr->k);
        }
    }
    /* each limb joins the torso, but limbs never join each other */
    float out = T;
    for (int g = 1; g < MAX_GROUPS; g++)
        if (L[g] < 1e8f)
            out = fminq(out, smin(T, L[g], kj[g]));
    return out;
}

/* the torso's bones, by height */
static void loft_bones(const body_info *bi, float y, float w, float *acc) {
    float hy[4] = {bi->hips_y, bi->waist_y - 0.02f * bi->s, bi->chest_y - 0.04f * bi->s, bi->chest_y + 0.035f * bi->s};
    int bn[4] = {BN_HIPS, BN_SPINE, BN_CHEST, BN_UPPERCHEST};
    if (y <= hy[0]) {
        acc[bn[0]] += w;
        return;
    }
    for (int i = 0; i < 3; i++)
        if (y <= hy[i + 1]) {
            float t = smoothf(hy[i], hy[i + 1], y);
            acc[bn[i]] += w * (1 - t), acc[bn[i + 1]] += w * t;
            return;
        }
    acc[bn[3]] += w;
}

void body_weights(const body_info *bi, v3 p, uint8_t idx[4], float wt[4]) {
    float acc[BN_BODY_COUNT] = {0};
    float d[BODY_MAX_PRIMS], tt[BODY_MAX_PRIMS];
    float dl = loft_dist(bi, p), dmin = dl;
    for (int i = 0; i < bi->nprim; i++) {
        d[i] = prim_dist(&bi->prim[i], p, &tt[i]);
        dmin = fminq(dmin, d[i]);
    }
    float sw = 0.008f * bi->s; /* how wide the blend between parts is */
    loft_bones(bi, p.y, expf(-(dl - dmin) / sw), acc);
    for (int i = 0; i < bi->nprim; i++) {
        const body_prim *pr = &bi->prim[i];
        float w = expf(-(d[i] - dmin) / sw);
        if (w < 1e-5f)
            continue;
        float w2 = pr->w2 < 0 ? smoothf(-pr->w2, 1.0f, tt[i]) : pr->w2; /* negative: shared towards the far end */
        acc[pr->bone] += w * (1 - (pr->bone2 >= 0 ? w2 : 0));
        if (pr->bone2 >= 0)
            acc[pr->bone2] += w * w2;
    }
    /* the four strongest */
    for (int k = 0; k < 4; k++) {
        int best = -1;
        for (int b = 0; b < BN_BODY_COUNT; b++)
            if (acc[b] > 0 && (best < 0 || acc[b] > acc[best]))
                best = b;
        if (best < 0) {
            idx[k] = (uint8_t)bi->bone[BN_HIPS], wt[k] = 0;
            continue;
        }
        idx[k] = (uint8_t)bi->bone[best];
        wt[k] = acc[best];
        acc[best] = 0;
    }
    float s = wt[0] + wt[1] + wt[2] + wt[3];
    if (s <= 0) {
        wt[0] = 1, wt[1] = wt[2] = wt[3] = 0;
        return;
    }
    for (int k = 0; k < 4; k++)
        wt[k] /= s;
}

/* ---- the skeleton and the shape's parts ---- */

void body_layout(fmodel *m, const body_params *bp, body_info *bi) {
    memset(bi, 0, sizeof *bi);
    bi->bp = *bp;
    float H = bp->height, s = H / 1.52f, B = bp->build;
    bi->H = H, bi->s = s;
    /* the head, scaled so that crown to chin is H / heads */
    head_params hp = head_defaults();
    float headh = H / bp->heads;
    float hs = headh / (hp.h * (1 + hp.chin * 0.07f));
    hp.w *= hs, hp.h *= hs, hp.d *= hs;
    bi->hp = hp;
    bi->chin_y = H - headh;
    bi->neck_y = bi->chin_y - 0.25f * headh * bp->neck;
    bi->shoulder_y = bi->neck_y - 0.018f * s;
    bi->crotch_y = bp->legs * H;
    bi->hips_y = bi->crotch_y + 0.065f * s;
    float torso = bi->neck_y - bi->crotch_y;
    bi->waist_y = bi->crotch_y + torso * 0.44f;
    bi->chest_y = bi->crotch_y + torso * 0.72f;
    bi->shoulder_x = bp->shoulders * H * 0.5f;
    bi->hip_x = 0.083f * s * bp->hips;
    bi->head_center = V3(0, bi->chin_y + hp.h * 0.5f + hp.chin * hp.h * 0.07f, 0.012f * s);
    float upper = 0.17f * H * bp->arms, fore = 0.145f * H * bp->arms;
    float knee_y = bi->crotch_y * 0.56f, ankle_y = 0.058f * s;
    for (int k = 0; k < 2; k++) {
        float sx = k == 0 ? 1.0f : -1.0f; /* 0: her left, +X */
        bi->elbow[k] = V3(sx * (bi->shoulder_x + upper), bi->shoulder_y, -0.006f * s);
        bi->wrist[k] = V3(sx * (bi->shoulder_x + upper + fore), bi->shoulder_y, 0);
        bi->knee[k] = V3(sx * (bi->hip_x - 0.014f * s), knee_y, 0.008f * s);
        bi->ankle[k] = V3(sx * (bi->hip_x - 0.018f * s), ankle_y, -0.012f * s);
    }

    /* bones, parents first */
    int *bn = bi->bone;
    for (int i = 0; i < BN_COUNT; i++)
        bn[i] = -1;
#define BONE(id, parent, head, tail) bn[id] = fig_add_bone(m, body_bone_names[id], (parent) >= 0 ? bn[parent] : -1, head, tail)
    BONE(BN_HIPS, -1, V3(0, bi->hips_y, 0), V3(0, bi->waist_y - 0.02f * s, 0));
    BONE(BN_SPINE, BN_HIPS, V3(0, bi->waist_y - 0.02f * s, 0), V3(0, bi->chest_y - 0.04f * s, 0));
    BONE(BN_CHEST, BN_SPINE, V3(0, bi->chest_y - 0.04f * s, 0), V3(0, bi->chest_y + 0.035f * s, 0));
    BONE(BN_UPPERCHEST, BN_CHEST, V3(0, bi->chest_y + 0.035f * s, 0), V3(0, bi->neck_y, -0.01f * s));
    BONE(BN_NECK, BN_UPPERCHEST, V3(0, bi->neck_y, -0.012f * s), V3(0, bi->chin_y + 0.012f * s, -0.016f * s));
    BONE(BN_HEAD, BN_NECK, V3(0, bi->chin_y + 0.012f * s, -0.016f * s), V3(0, H, -0.016f * s));
    v3 eye = vadd(bi->head_center, V3(0, hp.eye_y * hp.h + 0.012f * s, hp.d * 0.38f));
    BONE(BN_LEYE, BN_HEAD, vadd(eye, V3(0.032f * s, 0, 0)), vadd(eye, V3(0.032f * s, 0, 0.02f)));
    BONE(BN_REYE, BN_HEAD, vadd(eye, V3(-0.032f * s, 0, 0)), vadd(eye, V3(-0.032f * s, 0, 0.02f)));
    for (int k = 0; k < 2; k++) {
        float sx = k == 0 ? 1.0f : -1.0f;
        int o = k == 0 ? 0 : BN_RSHOULDER - BN_LSHOULDER;
        v3 sh = V3(sx * bi->shoulder_x, bi->shoulder_y, 0);
        BONE(BN_LSHOULDER + o, BN_UPPERCHEST, V3(sx * 0.022f * s, bi->shoulder_y + 0.004f * s, -0.006f * s), sh);
        BONE(BN_LUPPERARM + o, BN_LSHOULDER + o, sh, bi->elbow[k]);
        BONE(BN_LLOWERARM + o, BN_LUPPERARM + o, bi->elbow[k], bi->wrist[k]);
        BONE(BN_LHAND + o, BN_LLOWERARM + o, bi->wrist[k], vadd(bi->wrist[k], V3(sx * 0.08f * s, 0, 0)));
        int lo = k == 0 ? 0 : BN_RUPPERLEG - BN_LUPPERLEG;
        v3 hipj = V3(sx * bi->hip_x, bi->hips_y - 0.03f * s, 0);
        BONE(BN_LUPPERLEG + lo, BN_HIPS, hipj, bi->knee[k]);
        BONE(BN_LLOWERLEG + lo, BN_LUPPERLEG + lo, bi->knee[k], bi->ankle[k]);
        BONE(BN_LFOOT + lo, BN_LLOWERLEG + lo, bi->ankle[k], V3(bi->ankle[k].x, 0.016f * s, 0.105f * s));
        BONE(BN_LTOES + lo, BN_LFOOT + lo, V3(bi->ankle[k].x, 0.016f * s, 0.105f * s), V3(bi->ankle[k].x, 0.012f * s, 0.15f * s));
    }
    /* fingers: along the hand, palm down, thumb forwards */
    for (int k = 0; k < 2; k++) {
        float sx = k == 0 ? 1.0f : -1.0f;
        int base = k == 0 ? BN_LFINGERS : BN_RFINGERS;
        int hand = bn[k == 0 ? BN_LHAND : BN_RHAND];
        v3 w = bi->wrist[k];
        /* thumb: from the wrist's inner front, out and forwards */
        v3 t0 = vadd(w, V3(sx * 0.018f * s, -0.006f * s, 0.022f * s));
        v3 t1 = vadd(t0, V3(sx * 0.03f * s, -0.002f * s, 0.024f * s));
        v3 t2 = vadd(t1, V3(sx * 0.024f * s, -0.001f * s, 0.014f * s));
        v3 t3 = vadd(t2, V3(sx * 0.02f * s, 0, 0.008f * s));
        bn[base] = fig_add_bone(m, body_bone_names[base], hand, t0, t1);
        bn[base + 1] = fig_add_bone(m, body_bone_names[base + 1], bn[base], t1, t2);
        bn[base + 2] = fig_add_bone(m, body_bone_names[base + 2], bn[base + 1], t2, t3);
        static const float fz[4] = {0.022f, 0.007f, -0.008f, -0.022f}, flen[4] = {0.07f, 0.078f, 0.072f, 0.058f};
        for (int f = 0; f < 4; f++) {
            float L = flen[f] * s, x0 = 0.082f * s - (f == 3 ? 0.006f * s : 0);
            v3 a = vadd(w, V3(sx * x0, 0, fz[f] * s));
            v3 b = vadd(a, V3(sx * L * 0.44f, 0, 0)), c = vadd(b, V3(sx * L * 0.31f, 0, 0)), d = vadd(c, V3(sx * L * 0.25f, 0, 0));
            int id = base + 3 + f * 3;
            bn[id] = fig_add_bone(m, body_bone_names[id], hand, a, b);
            bn[id + 1] = fig_add_bone(m, body_bone_names[id + 1], bn[id], b, c);
            bn[id + 2] = fig_add_bone(m, body_bone_names[id + 2], bn[id + 1], c, d);
        }
    }
#undef BONE

    /* the torso's cross-sections: height, half width, half depth, centre depth */
    float hipsk = bp->hips, waistk = bp->waist;
    float sec[LOFT_N][4] = {
        {bi->crotch_y - 0.012f * s, 0.055f, 0.05f, 0.0f},
        {bi->crotch_y + 0.03f * s, 0.128f * hipsk, 0.082f, -0.004f},
        {bi->hips_y, 0.148f * hipsk, 0.09f, -0.01f},
        {bi->hips_y + 0.06f * s, 0.13f * lerpf(hipsk, waistk, 0.5f), 0.082f, -0.008f},
        {bi->waist_y, 0.103f * waistk, 0.072f, -0.004f},
        {lerpf(bi->waist_y, bi->chest_y, 0.55f), 0.11f, 0.077f, 0.0f},
        {bi->chest_y, 0.118f, 0.082f, 0.003f},
        {lerpf(bi->chest_y, bi->shoulder_y, 0.6f), 0.123f, 0.078f, -0.004f},
        {bi->shoulder_y + 0.004f * s, 0.104f, 0.064f, -0.012f},
        {bi->neck_y + 0.014f * s, 0.048f, 0.042f, -0.014f},
    };
    for (int i = 0; i < LOFT_N; i++) {
        bi->loft_y[i] = sec[i][0];
        bi->loft_a[i] = sec[i][1] * s * (i >= 1 && i <= 8 ? lerpf(1, B, 0.4f) : 1);
        bi->loft_b[i] = sec[i][2] * s * (i >= 1 && i <= 8 ? lerpf(1, B, 0.4f) : 1);
        bi->loft_z[i] = sec[i][3] * s;
    }
    /* the neck, into the head */
    add_prim(bi, capsule(V3(0, bi->neck_y - 0.03f * s, -0.012f * s), V3(0, bi->chin_y + 0.035f * s, -0.02f * s),
                         0.035f * s, 0.032f * s, 0, 0.022f * s, BN_NECK, BN_HEAD, -0.75f));
    /* chest, hips, shoulders */
    if (bp->chest > 0.05f)
        for (int k = -1; k <= 1; k += 2) {
            float c = bp->chest;
            add_prim(bi, ellipsoid(V3(k * 0.05f * s, bi->chest_y - 0.006f * s, (0.003f + 0.082f * 0.5f) * s),
                                   vmul(V3(0.05f, 0.046f, 0.04f), s * (0.55f + 0.45f * c)), qeuler(0.2f, k * 0.25f, 0), 0,
                                   0.022f * s, BN_CHEST, BN_UPPERCHEST, 0.4f));
        }
    for (int k = -1; k <= 1; k += 2) {
        add_prim(bi, ellipsoid(V3(k * 0.052f * s * hipsk, bi->hips_y - 0.028f * s, -0.052f * s), vmul(V3(0.064f, 0.066f, 0.05f), s * hipsk),
                               qid(), 0, 0.03f * s, BN_HIPS, -1, 0));
        int o = k > 0 ? 0 : BN_RSHOULDER - BN_LSHOULDER;
        add_prim(bi, ellipsoid(V3(k * (bi->shoulder_x - 0.004f * s), bi->shoulder_y - 0.006f * s, -0.004f * s),
                               vmul(V3(0.044f, 0.04f, 0.041f), s * B), qid(), 0, 0.028f * s, BN_LUPPERARM + o, BN_LSHOULDER + o, 0.45f));
    }
    /* arms: groups 1 and 2; legs: 3 and 4 */
    for (int k = 0; k < 2; k++) {
        float sx = k == 0 ? 1.0f : -1.0f;
        int o = k == 0 ? 0 : BN_RSHOULDER - BN_LSHOULDER, lo = k == 0 ? 0 : BN_RUPPERLEG - BN_LUPPERLEG;
        v3 sh = V3(sx * bi->shoulder_x, bi->shoulder_y, 0);
        add_prim(bi, capsule(sh, bi->elbow[k], 0.034f * s * B, 0.026f * s * B, 1 + k, 0.034f * s, BN_LUPPERARM + o, -1, 0));
        add_prim(bi, capsule(bi->elbow[k], bi->wrist[k], 0.026f * s * B, 0.0195f * s * B, 1 + k, 0.034f * s, BN_LLOWERARM + o,
                             BN_LHAND + o, -0.92f));
        v3 hipj = V3(sx * bi->hip_x, bi->hips_y - 0.03f * s, 0);
        add_prim(bi, capsule(hipj, bi->knee[k], 0.066f * s * B, 0.041f * s * B, 3 + k, 0.03f * s, BN_LUPPERLEG + lo, -1, 0));
        add_prim(bi, capsule(bi->knee[k], bi->ankle[k], 0.043f * s * B, 0.026f * s * B, 3 + k, 0.03f * s, BN_LLOWERLEG + lo, -1, 0));
        v3 calf = vlerp(bi->ankle[k], bi->knee[k], 0.68f);
        add_prim(bi, ellipsoid(vadd(calf, V3(0, 0, -0.012f * s)), vmul(V3(0.041f, 0.08f, 0.041f), s * B), qid(), 3 + k, 0.03f * s,
                               BN_LLOWERLEG + lo, -1, 0));
        v3 an = bi->ankle[k];
        add_prim(bi, ellipsoid(V3(an.x, 0.034f * s, 0.045f * s), vmul(V3(0.034f, 0.03f, 0.098f), s), qeuler(-0.08f, 0, 0), 3 + k,
                               0.03f * s, BN_LFOOT + lo, BN_LTOES + lo, 0.0f));
        add_prim(bi, ellipsoid(V3(an.x, 0.04f * s, -0.018f * s), vmul(V3(0.028f, 0.036f, 0.034f), s), qid(), 3 + k, 0.03f * s,
                               BN_LFOOT + lo, -1, 0));
    }
}

/* ---- meshes ---- */

bool body_build_mesh(fmodel *m, body_info *bi, int mat, float cell) {
    float s = bi->s;
    float reach = bi->wrist[0].x + 0.03f * s;
    v3 lo = V3(-reach, -0.01f, -0.16f * s), hi = V3(reach, bi->chin_y + 0.05f * s, 0.2f * s);
    smesh sm;
    if (!sdf_mesh(body_sdf, bi, lo, hi, cell, &sm))
        return false;
    fmesh *me = fig_add_mesh(m, sm.nv, sm.nt, mat, true, true);
    if (!me) {
        smesh_free(&sm);
        return false;
    }
    bi->mesh = m->nmeshes - 1;
    memcpy(me->pos, sm.pos, sizeof *sm.pos * sm.nv);
    memcpy(me->nrm, sm.nrm, sizeof *sm.nrm * sm.nv);
    memcpy(me->tri, sm.tri, sizeof *sm.tri * 3 * sm.nt);
    for (int i = 0; i < sm.nv; i++) {
        v3 p = sm.pos[i];
        body_weights(bi, p, me->bi[i], me->bw[i]);
        /* coordinates for patterns: round the body and up it, in metres */
        me->uv[i * 2] = atan2f(p.x, p.z) * 0.1f;
        me->uv[i * 2 + 1] = p.y;
        me->aux[i] = p.y * 4;
    }
    smesh_free(&sm);
    return true;
}

/* a finger: a tube along its three bones */
static void finger(fmodel *m, int mat, const int *bones, const fmodel *mm, float r0, float r1) {
    const fbone *b0 = &mm->bone[bones[0]], *b1 = &mm->bone[bones[1]], *b2 = &mm->bone[bones[2]];
    enum { N = 10 };
    v3 path[N];
    float rad[N];
    v3 j[4] = {b0->head, b1->head, b2->head, b2->tail};
    for (int i = 0; i < N; i++) {
        float t = (float)i / (N - 1) * 3;
        int seg = t >= 3 ? 2 : (int)t;
        float f = t - seg;
        path[i] = vlerp(j[seg], j[seg + 1], f);
        rad[i] = lerpf(r0, r1, (float)i / (N - 1));
    }
    /* start a little inside the palm, end rounded */
    path[0] = vlerp(j[0], j[1], -0.25f);
    fmesh *me = prim_tube(m, mat, path, rad, N, 10, true, V3(0, 1, 0));
    if (!me)
        return;
    int ring = 11;
    for (int v = 0; v < me->nv; v++) {
        int i = v < N * ring ? v / ring : (v == N * ring ? 0 : N - 1);
        float t = (float)i / (N - 1) * 3;
        int seg = t >= 3 ? 2 : (int)t;
        float f = t - seg;
        /* share between the segment and the next one near the joint */
        int a = bones[seg], b = seg < 2 ? bones[seg + 1] : bones[seg];
        float w = seg < 2 ? smoothf(0.7f, 1.0f, f) : 0;
        me->bi[v][0] = (uint8_t)a, me->bw[v][0] = 1 - w;
        me->bi[v][1] = (uint8_t)b, me->bw[v][1] = w;
        me->bw[v][2] = me->bw[v][3] = 0;
    }
}

void body_build_hands(fmodel *m, body_info *bi, int mat) {
    float s = bi->s;
    for (int k = 0; k < 2; k++) {
        float sx = k == 0 ? 1.0f : -1.0f;
        int base = k == 0 ? BN_LFINGERS : BN_RFINGERS;
        int hand = bi->bone[k == 0 ? BN_LHAND : BN_RHAND];
        v3 w = bi->wrist[k];
        /* the palm: a flattened ellipsoid from the wrist to the knuckles */
        fmesh *palm = prim_ellipsoid(m, mat, vadd(w, V3(sx * 0.046f * s, -0.001f * s, 0)), vmul(V3(0.05f, 0.0135f, 0.037f), s),
                                     qeuler(0, 0, 0), hand, 10, 16);
        (void)palm;
        int thumb[3] = {bi->bone[base], bi->bone[base + 1], bi->bone[base + 2]};
        finger(m, mat, thumb, m, 0.0105f * s, 0.0075f * s);
        static const float fr[4] = {0.0084f, 0.0086f, 0.0081f, 0.0072f};
        for (int f = 0; f < 4; f++) {
            int id = base + 3 + f * 3;
            int fb[3] = {bi->bone[id], bi->bone[id + 1], bi->bone[id + 2]};
            finger(m, mat, fb, m, fr[f] * s, fr[f] * s * 0.78f);
        }
    }
}

/* ---- the relaxed pose ---- */

void body_pose_relaxed(const body_info *bi, fpose *p) {
    for (int k = 0; k < 2; k++) {
        float sx = k == 0 ? 1.0f : -1.0f;
        int o = k == 0 ? 0 : BN_RSHOULDER - BN_LSHOULDER;
        /* arms down by the sides, a little forwards; elbows a little bent, palms in */
        p->local[bi->bone[BN_LSHOULDER + o]] = qaxis(V3(0, 0, 1), -sx * 0.06f);
        p->local[bi->bone[BN_LUPPERARM + o]] = qeuler(0, -sx * 0.12f, -sx * 1.2f); /* the same as the clips use */
        p->local[bi->bone[BN_LLOWERARM + o]] = qeuler(0, -sx * 0.28f, 0);
        p->local[bi->bone[BN_LHAND + o]] = qaxis(V3(0, 0, 1), -sx * 0.1f);
        /* fingers curled a little */
        int base = k == 0 ? BN_LFINGERS : BN_RFINGERS;
        for (int f = 0; f < 4; f++)
            for (int j = 0; j < 3; j++)
                p->local[bi->bone[base + 3 + f * 3 + j]] = qaxis(V3(0, 0, 1), -sx * (0.18f + 0.06f * f));
        p->local[bi->bone[base]] = qaxis(V3(0, 1, 0), -sx * 0.2f);
    }
}
