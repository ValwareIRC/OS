/* Simple shapes the figure is partly made of: ellipsoids, tubes swept along
 * a path (with a radius that changes along it), and their skinning. */
#include <stdlib.h>
#include <string.h>

#include "prim.h"

/* An ellipsoid (radii along X, Y, Z) at c, turned by q; all on one bone. */
fmesh *prim_ellipsoid(fmodel *m, int mat, v3 c, v3 r, quat q, int bone, int rings, int segs) {
    int nv = (rings + 1) * (segs + 1), nt = rings * segs * 2;
    fmesh *me = fig_add_mesh(m, nv, nt, mat, true, false);
    if (!me)
        return NULL;
    int k = 0;
    for (int i = 0; i <= rings; i++) {
        float phi = -PI_F / 2 + PI_F * i / rings;
        for (int j = 0; j <= segs; j++, k++) {
            float th = 2 * PI_F * j / segs;
            v3 u = V3(cosf(phi) * sinf(th), sinf(phi), cosf(phi) * cosf(th));
            me->pos[k] = vadd(c, qrot(q, vscale3(u, r)));
            me->nrm[k] = vnorm(qrot(q, V3(u.x / r.x, u.y / r.y, u.z / r.z)));
            me->uv[k * 2] = (float)j / segs;
            me->uv[k * 2 + 1] = 1 - (float)i / rings;
        }
    }
    int t = 0;
    for (int i = 0; i < rings; i++)
        for (int j = 0; j < segs; j++) {
            int a = i * (segs + 1) + j, b = a + 1, d = a + segs + 1, e = d + 1;
            me->tri[t++] = a, me->tri[t++] = b, me->tri[t++] = e;
            me->tri[t++] = a, me->tri[t++] = e, me->tri[t++] = d;
        }
    fig_mesh_rigid(me, bone);
    return me;
}

/* A tube along a path of n points with a radius at each, closed at both ends
 * when `caps`. `up` keeps the cross-sections from twisting. */
fmesh *prim_tube(fmodel *m, int mat, const v3 *path, const float *rad, int n, int segs, bool caps, v3 up) {
    if (n < 2)
        return NULL;
    int ring = segs + 1;
    int nv = n * ring + (caps ? 2 : 0), nt = (n - 1) * segs * 2 + (caps ? segs * 2 : 0);
    fmesh *me = fig_add_mesh(m, nv, nt, mat, true, true);
    if (!me)
        return NULL;
    v3 prev_side = V3(1, 0, 0);
    for (int i = 0; i < n; i++) {
        v3 dir = i == 0 ? vsub(path[1], path[0]) : i == n - 1 ? vsub(path[n - 1], path[n - 2]) : vsub(path[i + 1], path[i - 1]);
        dir = vnorm(dir);
        v3 side = vcross(dir, up);
        if (vlen(side) < 1e-4f)
            side = prev_side;
        side = vnorm(side);
        prev_side = side;
        v3 nup = vnorm(vcross(side, dir));
        for (int j = 0; j <= segs; j++) {
            float a = 2 * PI_F * j / segs;
            v3 off = vadd(vmul(side, cosf(a)), vmul(nup, sinf(a)));
            int k = i * ring + j;
            me->pos[k] = vmadd(path[i], off, rad[i]);
            me->nrm[k] = off;
            me->uv[k * 2] = (float)j / segs;
            me->uv[k * 2 + 1] = (float)i / (n - 1);
            me->aux[k] = (float)i / (n - 1);
        }
    }
    int t = 0;
    for (int i = 0; i < n - 1; i++)
        for (int j = 0; j < segs; j++) {
            int a = i * ring + j, b = a + 1, d = a + ring, e = d + 1;
            me->tri[t++] = a, me->tri[t++] = e, me->tri[t++] = b;
            me->tri[t++] = a, me->tri[t++] = d, me->tri[t++] = e;
        }
    if (caps) {
        int c0 = n * ring, c1 = c0 + 1;
        me->pos[c0] = path[0];
        me->nrm[c0] = vnorm(vsub(path[0], path[1]));
        me->pos[c1] = path[n - 1];
        me->nrm[c1] = vnorm(vsub(path[n - 1], path[n - 2]));
        for (int j = 0; j < segs; j++) { /* facing away from the tube at each end */
            me->tri[t++] = c0, me->tri[t++] = j, me->tri[t++] = j + 1;
            int b = (n - 1) * ring;
            me->tri[t++] = c1, me->tri[t++] = b + j + 1, me->tri[t++] = b + j;
        }
    }
    return me;
}

/* Every vertex shared between two bones, by how far along `from`->`to` it is. */
void prim_skin_along(fmesh *me, int bone_a, int bone_b, v3 from, v3 to, float blend) {
    v3 ax = vsub(to, from);
    float l2 = vdot(ax, ax);
    for (int i = 0; i < me->nv; i++) {
        float t = l2 > 0 ? vdot(vsub(me->pos[i], from), ax) / l2 : 0;
        float w = smoothf(0.5f - blend, 0.5f + blend, t);
        me->bi[i][0] = (uint8_t)bone_a, me->bw[i][0] = 1 - w;
        me->bi[i][1] = (uint8_t)bone_b, me->bw[i][1] = w;
        me->bw[i][2] = me->bw[i][3] = 0;
    }
}
