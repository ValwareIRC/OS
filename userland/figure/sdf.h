#pragma once
/* Shapes described by their distance (signed distance fields), and turning
 * them into meshes. The body and its clothes are made this way: smooth
 * where parts join, and clothes are the body grown by the cloth's
 * thickness, so they fit exactly. */
#include "fig.h"

/* the smooth minimum: two shapes joined with a fillet of radius about k */
static inline float smin(float a, float b, float k) {
    if (k <= 0)
        return a < b ? a : b;
    float h = clampf(0.5f + 0.5f * (b - a) / k, 0, 1);
    return lerpf(b, a, h) - k * h * (1 - h);
}

/* a cone with rounded ends: radius ra at a, rb at b; t: how far along (0..1) */
static inline float sd_capsule(v3 p, v3 a, v3 b, float ra, float rb, float *t) {
    v3 ba = vsub(b, a), pa = vsub(p, a);
    float l2 = vdot(ba, ba);
    float h = l2 > 0 ? clampf(vdot(pa, ba) / l2, 0, 1) : 0;
    if (t)
        *t = h;
    return vlen(vsub(pa, vmul(ba, h))) - lerpf(ra, rb, h);
}

/* an ellipsoid at c with radii r, turned by q (approximate, good near the surface) */
static inline float sd_ellipsoid(v3 p, v3 c, v3 r, quat q) {
    v3 l = qrot(qconj(q), vsub(p, c));
    float k0 = vlen(V3(l.x / r.x, l.y / r.y, l.z / r.z));
    float k1 = vlen(V3(l.x / (r.x * r.x), l.y / (r.y * r.y), l.z / (r.z * r.z)));
    return k1 > 1e-9f ? k0 * (k0 - 1) / k1 : -fminq(r.x, fminq(r.y, r.z));
}

/* the 2D ellipse version, for cross-sections */
static inline float sd_ellipse2(float x, float z, float a, float b) {
    float k0 = sqrtf((x * x) / (a * a) + (z * z) / (b * b));
    float k1 = sqrtf((x * x) / (a * a * a * a) + (z * z) / (b * b * b * b));
    return k1 > 1e-9f ? k0 * (k0 - 1) / k1 : -fminq(a, b);
}

/* A mesh being made: positions, normals, triangles. */
typedef struct {
    v3 *pos, *nrm;
    int nv, capv;
    int *tri;
    int nt, capt;
} smesh;

typedef float (*sdf_fn)(void *ctx, v3 p);

/* The surface where f is zero, inside the box lo..hi, from cells of `cell`
 * metres: vertices placed on the surface itself, normals from the field. */
bool sdf_mesh(sdf_fn f, void *ctx, v3 lo, v3 hi, float cell, smesh *out);
void smesh_free(smesh *m);
/* the field's gradient (its normal, where it is a distance) */
v3 sdf_normal(sdf_fn f, void *ctx, v3 p, float e);
