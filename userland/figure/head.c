/* The head: a sculpted shape rather than a sphere (a broad cranium, a face
 * that is nearly flat at the front, cheeks that taper to a small chin, as in
 * anime), with the features drawn onto a texture projected from the front,
 * the way VRoid and most anime models do it. Ears are their own small
 * shapes. */
#include <stdlib.h>

#include "head.h"
#include "prim.h"

head_params head_defaults(void) {
    return (head_params){.w = 0.186f, .h = 0.228f, .d = 0.205f, .jaw = 0.44f, .chin = 0.22f, .cheek = 0.55f,
                         .flat = 0.45f, .eye_y = -0.07f, .ears = 1};
}

v3 head_point(const head_params *hp, float ux, float uy, float uz) {
    float X = ux * hp->w * 0.5f, Y = uy * hp->h * 0.5f, Z = uz * hp->d * 0.5f;
    float ey = hp->eye_y * 2; /* in the unit sphere's -1..1 */
    float front = smoothf(-0.25f, 0.55f, uz);
    /* below the eyes: the cheeks taper to the chin, which comes down and a little forward */
    if (uy < ey) {
        float t = clampf((ey - uy) / (1 + ey), 0, 1);
        float taper = 1 - (1 - hp->jaw) * powf(t, 1.7f);
        taper += hp->cheek * 0.07f * sinf(PI_F * t) * (1 - t);
        X *= lerpf(1, taper, 0.25f + 0.75f * front);
        Y -= hp->chin * hp->h * 0.07f * t * t * front;
        Z += hp->h * 0.025f * t * front;
        /* the jaw's underside: up towards the neck at the back */
        if (uz < 0.2f)
            Y += hp->h * 0.08f * t * (1 - front);
    }
    /* a rounder, fuller cranium at the back */
    if (uz < 0)
        Z *= 1 + 0.12f * smoothf(-0.3f, 0.8f, uy + 0.5f);
    /* the face: nearly a plane at the front */
    if (uz > 0)
        Z = hp->d * 0.5f * powf(uz, 1.0f / (1 + hp->flat * 1.6f)) * (0.92f + 0.08f * (1 - front));
    return V3(X, Y, Z);
}

v3 head_point_at(const head_params *hp, float th, float phi) {
    return head_point(hp, cosf(phi) * sinf(th), sinf(phi), cosf(phi) * cosf(th));
}

v3 head_normal_at(const head_params *hp, float th, float phi) {
    const float e = 0.01f;
    v3 a = head_point_at(hp, th - e, phi), b = head_point_at(hp, th + e, phi);
    v3 c = head_point_at(hp, th, phi - e), d = head_point_at(hp, th, phi + e);
    v3 n = vcross(vsub(b, a), vsub(d, c));
    if (cosf(phi) < 0.02f) /* at the poles the sideways step vanishes */
        n = V3(0, phi > 0 ? 1.0f : -1.0f, 0);
    if (vdot(n, head_point_at(hp, th, phi)) < 0)
        n = vmul(n, -1);
    return vnorm(n);
}

head_info head_build(fmodel *m, const head_params *hp, v3 center, int bone, rgb skin, int texw) {
    head_info hi = {.center = center, .hp = *hp, .tex = -1};
    hi.tex = fig_add_tex(m, texw, texw);
    fmat face = fig_mat(MAT_FACE, skin);
    face.tex = hi.tex;
    face.rim = 0.1f;
    face.shift = -0.62f; /* faces stay mostly lit: no hard shadow across the nose */
    face.toony = 0.5f;
    hi.mat = fig_add_mat(m, face);
    hi.skin_mat = fig_add_mat(m, fig_mat(MAT_SKIN, skin));

    const int rings = 34, segs = 48;
    int nv = (rings + 1) * (segs + 1), nt = rings * segs * 2;
    fmesh *me = fig_add_mesh(m, nv, nt, hi.mat, true, false);
    if (!me)
        return hi;
    int k = 0;
    for (int i = 0; i <= rings; i++) {
        float phi = -PI_F / 2 + PI_F * i / rings;
        for (int j = 0; j <= segs; j++, k++) {
            float th = -PI_F + 2 * PI_F * j / segs;
            float ux = cosf(phi) * sinf(th), uy = sinf(phi), uz = cosf(phi) * cosf(th);
            v3 p = head_point(hp, ux, uy, uz);
            me->pos[k] = vadd(center, p);
            float u, v;
            head_uv(hp, p.x, p.y, &u, &v);
            if (uz < 0.18f) /* the sides and back: plain skin at the texture's edge */
                u = p.x > 0 ? 0.997f : 0.003f;
            me->uv[k * 2] = clampf(u, 0.003f, 0.997f);
            me->uv[k * 2 + 1] = clampf(v, 0.003f, 0.997f);
        }
    }
    int t = 0;
    for (int i = 0; i < rings; i++)
        for (int j = 0; j < segs; j++) {
            int a = i * (segs + 1) + j, b = a + 1, d = a + segs + 1, e = d + 1;
            me->tri[t++] = a, me->tri[t++] = b, me->tri[t++] = e;
            me->tri[t++] = a, me->tri[t++] = e, me->tri[t++] = d;
        }
    fig_mesh_normals(me);
    fig_mesh_rigid(me, bone);

    if (hp->ears) {
        for (int s = -1; s <= 1; s += 2) {
            v3 c = vadd(center, V3(s * hp->w * 0.47f, hp->h * (hp->eye_y - 0.02f), -hp->d * 0.06f));
            if (hp->ears == 2) { /* elf: long, pointing up and back */
                quat q = qmul(qaxis(V3(0, 0, 1), -s * 0.9f), qaxis(V3(1, 0, 0), 0.45f));
                prim_ellipsoid(m, hi.skin_mat, vadd(c, V3(s * 0.012f, 0.012f, -0.006f)), V3(0.012f, 0.038f, 0.007f), q,
                               bone, 8, 10);
            } else {
                quat q = qaxis(V3(0, 1, 0), s * 0.35f);
                prim_ellipsoid(m, hi.skin_mat, c, V3(0.008f, 0.024f, 0.014f), q, bone, 8, 10);
            }
        }
    }
    return hi;
}
