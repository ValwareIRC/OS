/* Meshing a distance field with surface nets: a vertex in every grid cell
 * the surface passes through (at the average of where it crosses the cell's
 * edges, then moved onto the surface itself), and a quad across every grid
 * edge the surface crosses. Only blocks near the surface are looked at, so
 * a fine grid over a whole body stays quick. */
#include <stdlib.h>
#include <string.h>

#include "sdf.h"

void smesh_free(smesh *m) {
    free(m->pos), free(m->nrm), free(m->tri);
    memset(m, 0, sizeof *m);
}

static bool add_vert(smesh *m, v3 p) {
    if (m->nv >= m->capv) {
        int c = m->capv ? m->capv * 2 : 8192;
        v3 *p2 = realloc(m->pos, sizeof *p2 * c), *n2 = realloc(m->nrm, sizeof *n2 * c);
        if (p2)
            m->pos = p2;
        if (n2)
            m->nrm = n2;
        if (!p2 || !n2)
            return false;
        m->capv = c;
    }
    m->pos[m->nv++] = p;
    return true;
}

static bool add_tri(smesh *m, int a, int b, int c) {
    if (m->nt >= m->capt) {
        int cap = m->capt ? m->capt * 2 : 16384;
        int *t = realloc(m->tri, sizeof *t * 3 * cap);
        if (!t)
            return false;
        m->tri = t, m->capt = cap;
    }
    m->tri[m->nt * 3] = a, m->tri[m->nt * 3 + 1] = b, m->tri[m->nt * 3 + 2] = c;
    m->nt++;
    return true;
}

v3 sdf_normal(sdf_fn f, void *ctx, v3 p, float e) {
    float dx = f(ctx, V3(p.x + e, p.y, p.z)) - f(ctx, V3(p.x - e, p.y, p.z));
    float dy = f(ctx, V3(p.x, p.y + e, p.z)) - f(ctx, V3(p.x, p.y - e, p.z));
    float dz = f(ctx, V3(p.x, p.y, p.z + e)) - f(ctx, V3(p.x, p.y, p.z - e));
    return vnorm(V3(dx, dy, dz));
}

#define BLOCK 4

bool sdf_mesh(sdf_fn f, void *ctx, v3 lo, v3 hi, float cell, smesh *out) {
    memset(out, 0, sizeof *out);
    int nx = (int)ceilf((hi.x - lo.x) / cell) + 1, ny = (int)ceilf((hi.y - lo.y) / cell) + 1,
        nz = (int)ceilf((hi.z - lo.z) / cell) + 1;
    if (nx < 2 || ny < 2 || nz < 2)
        return false;
    size_t ncorner = (size_t)nx * ny * nz, ncell = (size_t)(nx - 1) * (ny - 1) * (nz - 1);
    float *val = malloc(sizeof *val * ncorner);
    int *vid = malloc(sizeof *vid * ncell);
    if (!val || !vid) {
        free(val), free(vid);
        return false;
    }
    for (size_t i = 0; i < ncorner; i++)
        val[i] = NAN;
    for (size_t i = 0; i < ncell; i++)
        vid[i] = -1;
#define CI(i, j, k) (((size_t)(k) * ny + (j)) * nx + (i))
#define CELL(i, j, k) (((size_t)(k) * (ny - 1) + (j)) * (nx - 1) + (i))
    /* the blocks near the surface: every corner in them */
    float reach = BLOCK * cell * 0.87f * 1.6f + cell;
    for (int bk = 0; bk < nz - 1; bk += BLOCK)
        for (int bj = 0; bj < ny - 1; bj += BLOCK)
            for (int bi = 0; bi < nx - 1; bi += BLOCK) {
                int ei = bi + BLOCK < nx - 1 ? bi + BLOCK : nx - 1, ej = bj + BLOCK < ny - 1 ? bj + BLOCK : ny - 1,
                    ek = bk + BLOCK < nz - 1 ? bk + BLOCK : nz - 1;
                v3 c = V3(lo.x + (bi + ei) * 0.5f * cell, lo.y + (bj + ej) * 0.5f * cell, lo.z + (bk + ek) * 0.5f * cell);
                if (fabsf(f(ctx, c)) > reach)
                    continue;
                for (int k = bk; k <= ek; k++)
                    for (int j = bj; j <= ej; j++)
                        for (int i = bi; i <= ei; i++) {
                            float *v = &val[CI(i, j, k)];
                            if (isnan(*v))
                                *v = f(ctx, V3(lo.x + i * cell, lo.y + j * cell, lo.z + k * cell));
                        }
            }
    /* a vertex in each cell the surface passes through */
    static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k < nz - 1; k++)
        for (int j = 0; j < ny - 1; j++)
            for (int i = 0; i < nx - 1; i++) {
                float v[8];
                bool skip = false;
                int inside = 0;
                for (int c = 0; c < 8; c++) {
                    v[c] = val[CI(i + (c & 1), j + ((c >> 1) & 1), k + (c >> 2))];
                    if (isnan(v[c])) {
                        skip = true;
                        break;
                    }
                    inside += v[c] < 0;
                }
                if (skip || inside == 0 || inside == 8)
                    continue;
                v3 acc = V3(0, 0, 0);
                int n = 0;
                for (int e = 0; e < 12; e++) {
                    float a = v[E[e][0]], b = v[E[e][1]];
                    if ((a < 0) == (b < 0))
                        continue;
                    float t = a / (a - b);
                    int c0 = E[e][0], c1 = E[e][1];
                    v3 p0 = V3(i + (c0 & 1), j + ((c0 >> 1) & 1), k + (c0 >> 2));
                    v3 p1 = V3(i + (c1 & 1), j + ((c1 >> 1) & 1), k + (c1 >> 2));
                    acc = vadd(acc, vlerp(p0, p1, t));
                    n++;
                }
                v3 p = vmul(acc, 1.0f / n);
                p = V3(lo.x + p.x * cell, lo.y + p.y * cell, lo.z + p.z * cell);
                /* onto the surface itself */
                for (int it = 0; it < 3; it++) {
                    float d = f(ctx, p);
                    if (fabsf(d) < cell * 0.002f)
                        break;
                    v3 g = sdf_normal(f, ctx, p, cell * 0.2f);
                    p = vmadd(p, g, -clampf(d, -cell, cell));
                }
                vid[CELL(i, j, k)] = out->nv;
                if (!add_vert(out, p))
                    goto fail;
            }
    /* a quad across each edge the surface crosses, facing outwards */
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                float a = val[CI(i, j, k)];
                if (isnan(a))
                    continue;
                for (int ax = 0; ax < 3; ax++) {
                    int i2 = i + (ax == 0), j2 = j + (ax == 1), k2 = k + (ax == 2);
                    if (i2 >= nx || j2 >= ny || k2 >= nz)
                        continue;
                    float b = val[CI(i2, j2, k2)];
                    if (isnan(b) || (a < 0) == (b < 0))
                        continue;
                    /* the four cells round this edge, anticlockwise seen from its + end */
                    int cs[4][3];
                    static const int off[4][2] = {{-1, -1}, {0, -1}, {0, 0}, {-1, 0}};
                    bool ok = true;
                    for (int q = 0; q < 4; q++) {
                        int ci = i, cj = j, ck = k;
                        /* the other two axes, in cyclic order */
                        if (ax == 0)
                            cj += off[q][0], ck += off[q][1];
                        else if (ax == 1)
                            ck += off[q][0], ci += off[q][1];
                        else
                            ci += off[q][0], cj += off[q][1];
                        if (ci < 0 || cj < 0 || ck < 0 || ci >= nx - 1 || cj >= ny - 1 || ck >= nz - 1) {
                            ok = false;
                            break;
                        }
                        cs[q][0] = ci, cs[q][1] = cj, cs[q][2] = ck;
                    }
                    if (!ok)
                        continue;
                    int v[4];
                    for (int q = 0; q < 4; q++) {
                        v[q] = vid[CELL(cs[q][0], cs[q][1], cs[q][2])];
                        if (v[q] < 0)
                            ok = false;
                    }
                    if (!ok)
                        continue;
                    if (a > 0) { /* outside at the start: the surface faces the other way */
                        int t = v[1];
                        v[1] = v[3], v[3] = t;
                    }
                    /* split along the shorter diagonal */
                    float d02 = vlen(vsub(out->pos[v[0]], out->pos[v[2]])), d13 = vlen(vsub(out->pos[v[1]], out->pos[v[3]]));
                    bool okt = d02 <= d13 ? add_tri(out, v[0], v[1], v[2]) && add_tri(out, v[0], v[2], v[3])
                                          : add_tri(out, v[0], v[1], v[3]) && add_tri(out, v[1], v[2], v[3]);
                    if (!okt)
                        goto fail;
                }
            }
    for (int i = 0; i < out->nv; i++)
        out->nrm[i] = sdf_normal(f, ctx, out->pos[i], cell * 0.25f);
    free(val), free(vid);
    return true;
fail:
    free(val), free(vid);
    smesh_free(out);
    return false;
#undef CI
#undef CELL
}
