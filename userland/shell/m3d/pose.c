/* Posing a model: its animations sampled at their moments (linear, step and cubic-spline keys, rotations by
 * slerp), the interface's own nudges added, then every node's world matrix from its parents', and the bounds of
 * what is drawn. */
#include "m3math.h"
#include <string.h>

static void sample(const m3_channel *c, float t, float *out) {
    int n = c->n, k = c->comps;
    const float *in = c->in;
    if (t <= in[0] || n == 1) {
        memcpy(out, c->out + (c->interp == 2 ? k : 0), sizeof(float) * k);
        return;
    }
    if (t >= in[n - 1]) {
        memcpy(out, c->out + (size_t)(n - 1) * k * (c->interp == 2 ? 3 : 1) + (c->interp == 2 ? k : 0), sizeof(float) * k);
        return;
    }
    int lo = 0, hi = n - 1; /* the key before t */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (in[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }
    float dt = in[hi] - in[lo], u = dt > 0 ? (t - in[lo]) / dt : 0;
    if (c->interp == 1) {
        memcpy(out, c->out + (size_t)lo * k, sizeof(float) * k);
    } else if (c->interp == 2) { /* Hermite: each key holds in-tangent, value, out-tangent */
        const float *a = c->out + (size_t)lo * k * 3, *b = c->out + (size_t)hi * k * 3;
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        for (int i = 0; i < k; i++)
            out[i] = h00 * a[k + i] + h10 * dt * a[2 * k + i] + h01 * b[k + i] + h11 * dt * b[i];
        if (c->path == M3_R) {
            float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
            for (int i = 0; l > 0 && i < 4; i++)
                out[i] /= l;
        }
    } else if (c->path == M3_R) {
        quat_slerp(out, c->out + (size_t)lo * 4, c->out + (size_t)hi * 4, u);
    } else {
        for (int i = 0; i < k; i++)
            out[i] = mixf(c->out[(size_t)lo * k + i], c->out[(size_t)hi * k + i], u);
    }
}

static void world_of(m3_model *m, int i, const float *parent, int depth) {
    m3_node *nd = &m->nodes[i];
    float local[16], t[3] = {nd->pt[0] + nd->lift[0], nd->pt[1] + nd->lift[1], nd->pt[2] + nd->lift[2]};
    mat_trs(local, t, nd->pr, nd->ps);
    if (parent)
        mat_mul(nd->world, parent, local);
    else
        memcpy(nd->world, local, sizeof local);
    if (depth > 256)
        return;
    for (int c = 0; c < m->nnodes; c++) /* children: few nodes in an interface model, so a scan is fine */
        if (m->nodes[c].parent == i)
            world_of(m, c, nd->world, depth + 1);
}

void m3_pose(m3_model *m, const int *which, const float *times) {
    for (int i = 0; i < m->nnodes; i++) {
        m3_node *nd = &m->nodes[i];
        memcpy(nd->pt, nd->t, sizeof nd->t), memcpy(nd->pr, nd->r, sizeof nd->r), memcpy(nd->ps, nd->s, sizeof nd->s);
    }
    for (int a = 0; which && a < 8 && which[a] >= 0; a++) {
        if (which[a] >= m->nanims)
            continue;
        const m3_anim *an = &m->anims[which[a]];
        for (int k = 0; k < an->nch; k++) {
            const m3_channel *c = &an->ch[k];
            m3_node *nd = &m->nodes[c->node];
            sample(c, times[a], c->path == M3_T ? nd->pt : c->path == M3_R ? nd->pr : nd->ps);
        }
    }
    for (int r = 0; r < m->nroots; r++)
        world_of(m, m->roots[r], NULL, 0);
    m->lo = v3(1e30f, 1e30f, 1e30f), m->hi = v3(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < m->nnodes; i++) {
        m3_node *nd = &m->nodes[i];
        if (nd->mesh < 0)
            continue;
        const m3_mesh *me = &m->meshes[nd->mesh];
        for (int p = 0; p < me->nprims; p++)
            for (int c = 0; c < 8; c++) {
                const m3_prim *pr = &me->prims[p];
                m3v q = mat_point(nd->world, v3(c & 1 ? pr->hi.x : pr->lo.x, c & 2 ? pr->hi.y : pr->lo.y, c & 4 ? pr->hi.z : pr->lo.z));
                m->lo = v3(fminf(m->lo.x, q.x), fminf(m->lo.y, q.y), fminf(m->lo.z, q.z));
                m->hi = v3(fmaxf(m->hi.x, q.x), fmaxf(m->hi.y, q.y), fmaxf(m->hi.z, q.z));
            }
    }
    if (m->lo.x > m->hi.x)
        m->lo = m->hi = v3(0, 0, 0);
}
