/* Hair, generated: a cap over the scalp so no skin shows through, and locks
 * (the clumps anime hair is drawn as) laid along the head and then falling,
 * each a ribbon that is wide at the root, arched across so the light falls
 * off at its edges, and pointed at the tip. Every lock is its own part, so
 * the outline pass draws the lines between them, as an artist would.
 *
 * Styles are recipes: which locks, rooted where, how long, swept which way. */
#include <stdlib.h>
#include <string.h>

#include "hair.h"

hair_params hair_defaults(void) {
    return (hair_params){.style = HAIR_BOB, .bangs = BANGS_SIDE, .length = 0.02f, .volume = 0.5f, .curl = -0.4f,
                         .part = 0.35f, .ahoge = 1, .color = 0x6e4552, .tip = 0xb27a86};
}

/* ---- building a mesh a piece at a time ---- */

typedef struct {
    v3 *pos;
    float *uv, *aux;
    int *tri;
    uint8_t *part;
    int nv, nt, capv, capt;
} mbuild;

static bool mb_room(mbuild *b, int nv, int nt) {
    if (b->nv + nv > b->capv) {
        int c = b->capv ? b->capv * 2 : 4096;
        while (c < b->nv + nv)
            c *= 2;
        v3 *p = realloc(b->pos, sizeof *p * c);
        float *uv = realloc(b->uv, sizeof *uv * 2 * c), *ax = realloc(b->aux, sizeof *ax * c);
        if (p)
            b->pos = p;
        if (uv)
            b->uv = uv;
        if (ax)
            b->aux = ax;
        if (!p || !uv || !ax)
            return false;
        b->capv = c;
    }
    if (b->nt + nt > b->capt) {
        int c = b->capt ? b->capt * 2 : 8192;
        while (c < b->nt + nt)
            c *= 2;
        int *t = realloc(b->tri, sizeof *t * 3 * c);
        uint8_t *pt = realloc(b->part, c);
        if (t)
            b->tri = t;
        if (pt)
            b->part = pt;
        if (!t || !pt)
            return false;
        b->capt = c;
    }
    return true;
}

static int mb_vert(mbuild *b, v3 p, float u, float v, float aux) {
    int i = b->nv++;
    b->pos[i] = p, b->uv[i * 2] = u, b->uv[i * 2 + 1] = v, b->aux[i] = aux;
    return i;
}

static void mb_tri(mbuild *b, int x, int y, int z, int part) {
    int t = b->nt++;
    b->tri[t * 3] = x, b->tri[t * 3 + 1] = y, b->tri[t * 3 + 2] = z;
    b->part[t] = (uint8_t)(part > 255 ? 255 : part);
}

/* ---- the cap ---- */

/* How far down the head the hair grows, by how far round from the front
 * (0) to the back (1): the forehead, the temples, in front of the ears, the
 * nape. */
static float hairline(float a) {
    static const float k[][2] = {{0, 0.42f}, {0.3f, 0.26f}, {0.42f, -0.02f}, {0.55f, -0.2f}, {0.75f, -0.5f}, {1, -0.72f}};
    for (int i = 0; i < 5; i++)
        if (a <= k[i + 1][0]) {
            float t = (a - k[i][0]) / (k[i + 1][0] - k[i][0]);
            return lerpf(k[i][1], k[i + 1][1], t);
        }
    return k[5][1];
}

/* how far the hair's surface stands off the scalp */
static float hair_shell(const hair_params *hp, float phi) {
    return 0.006f + hp->volume * 0.024f * (0.45f + 0.55f * smoothf(-0.6f, 1.2f, phi));
}

static void build_cap(mbuild *b, const head_info *hi, const hair_params *hp) {
    const int segs = 44, rings = 12;
    if (!mb_room(b, (segs + 1) * (rings + 1), segs * rings * 2))
        return;
    int base = b->nv;
    for (int j = 0; j <= segs; j++) {
        float th = -PI_F + 2 * PI_F * j / segs;
        float ph0 = hairline(fabsf(th) / PI_F);
        for (int i = 0; i <= rings; i++) {
            float phi = lerpf(ph0, PI_F / 2 - 0.002f, powf((float)i / rings, 0.85f));
            float off = hair_shell(hp, phi) * lerpf(0.3f, 1.0f, smoothf(ph0, ph0 + 0.35f, phi));
            v3 p = vadd(hi->center, vmadd(head_point_at(&hi->hp, th, phi), head_normal_at(&hi->hp, th, phi), off));
            mb_vert(b, p, (float)j / segs, 0, 0);
        }
    }
    for (int j = 0; j < segs; j++)
        for (int i = 0; i < rings; i++) {
            int a = base + j * (rings + 1) + i, bb = a + rings + 1, e = bb + 1, d = a + 1;
            mb_tri(b, a, bb, e, 0);
            mb_tri(b, a, e, d, 0);
        }
}

/* ---- locks ---- */

typedef struct {
    float th, phi;   /* the root, on the head */
    float len;       /* at most this long (metres) */
    float ytip;      /* falling locks stop at this height (model space) */
    float w;         /* width at the root */
    float sweep;     /* how far round (radians) it drifts while on the head */
    float release;   /* it leaves the head below this phi, and falls */
    float lift;      /* how far it stands off the head at mid-length */
    float layer;     /* extra offset, for locks lying over other locks */
    float bulge;     /* how arched it is across, as a share of its width */
    float flare;     /* how much it spreads outwards as it falls */
    float curl;      /* the tip: + outwards, - inwards */
    float blunt;     /* 0 pointed .. 1 cut straight across */
    float phi_tip;   /* locks that stay on the head (bangs) end below this phi */
} lockdesc;

#define DENSE 400

/* what every lock is built with */
typedef struct {
    mbuild *b;
    const head_info *hi;
    const hair_params *hp;
    const collider *col;
    int ncol;
    hair_info *out;
} hctx;

/* The lock's centre line: along the head's surface (at a little distance)
 * until it passes `release`, then falling, turned outwards a little and kept
 * off the body; resampled evenly to LOCK_PTS points. O is "away from the
 * head" at each point. */
static int lock_path(const hctx *cx, const lockdesc *L, v3 *P, v3 *O, float *len_out) {
    static v3 dp[DENSE], dn[DENSE];
    const head_info *hi = cx->hi;
    const head_params *hp = &hi->hp;
    const collider *col = cx->col;
    int ncol = cx->ncol;
    int n = 0;
    float th = L->th, phi = L->phi, total = 0;
    v3 prev = V3(0, 0, 0), dir = V3(0, -1, 0);
    bool falling = false;
    float plen = L->phi_tip > -5 ? fmaxf(0.03f, (L->phi - L->phi_tip) * 0.1f) : fmaxf(L->len, 0.03f);
    while (n < DENSE) {
        /* off the scalp by the hair's shell (rising from under the cap at
         * the root, so no root shows), and a little more mid-way */
        float root = smoothf(0.0f, 0.03f, total);
        float lift = hair_shell(cx->hp, phi) * lerpf(0.3f, 1.0f, root) + (0.0025f + L->layer) * root +
                     L->lift * sinf(PI_F * clampf(total / plen, 0, 1) * 0.85f);
        v3 p, o;
        if (!falling) {
            v3 s = head_point_at(hp, th, phi), nn = head_normal_at(hp, th, phi);
            p = vadd(hi->center, vmadd(s, nn, lift));
            o = nn;
        } else {
            v3 radial = V3(prev.x - hi->center.x, 0, prev.z - hi->center.z);
            radial = vlen(radial) > 1e-4f ? vnorm(radial) : V3(0, 0, -1);
            v3 goal = vnorm(vadd(V3(0, -1, 0), vmul(radial, L->flare)));
            dir = vnorm(vlerp(dir, goal, 0.08f));
            p = vmadd(prev, dir, 0.004f);
            for (int c = 0; c < ncol; c++) { /* off the body */
                v3 d = vsub(p, col[c].c);
                float l = vlen(d), r = col[c].r + 0.005f + L->layer;
                if (l < r && l > 1e-6f)
                    p = vmadd(col[c].c, vmul(d, 1.0f / l), r);
            }
            { /* and off the head: its ellipsoid, grown by the hair's shell */
                float H = hair_shell(cx->hp, 0) + 0.003f + L->layer;
                v3 q = vsub(p, hi->center);
                v3 rr = V3(hp->w * 0.5f + H, hp->h * 0.5f + H, hp->d * 0.55f + H);
                v3 sq = V3(q.x / rr.x, q.y / rr.y, q.z / rr.z);
                float e = vlen(sq);
                if (e < 1 && e > 1e-4f && q.y > -hp->h * 0.62f)
                    p = vadd(hi->center, vscale3(vmul(sq, 1.0f / e), rr));
            }
            o = radial;
        }
        if (n > 0) {
            total += vlen(vsub(p, prev));
            if (total >= L->len || (falling && p.y <= L->ytip) || (!falling && phi < L->phi_tip))
                break;
        }
        dp[n] = p, dn[n] = o, n++;
        if (!falling) {
            /* the next point down the head, about 4 mm on */
            float step = 0;
            v3 last = p;
            for (int k = 0; k < 64 && step < 0.004f; k++) {
                phi -= 0.004f;
                th += L->sweep * 0.004f / fmaxf(fabsf(L->phi - L->release), 0.2f);
                v3 q = vadd(hi->center, head_point_at(hp, th, phi));
                step += vlen(vsub(q, last));
                last = q;
            }
            if (phi < L->release || phi < -PI_F / 2 + 0.05f) {
                falling = true;
                dir = n > 1 ? vnorm(vsub(dp[n - 1], dp[n - 2])) : V3(0, -1, 0);
            }
        }
        prev = p;
    }
    if (n < 2)
        return 0;
    /* resample evenly */
    float seg[DENSE];
    float acc = 0;
    seg[0] = 0;
    for (int i = 1; i < n; i++)
        acc += vlen(vsub(dp[i], dp[i - 1])), seg[i] = acc;
    int k = 1;
    for (int i = 0; i < LOCK_PTS; i++) {
        float want = acc * i / (LOCK_PTS - 1);
        while (k < n - 1 && seg[k] < want)
            k++;
        float t = seg[k] > seg[k - 1] ? (want - seg[k - 1]) / (seg[k] - seg[k - 1]) : 0;
        t = clampf(t, 0, 1);
        P[i] = vlerp(dp[k - 1], dp[k], t);
        O[i] = vnorm(vlerp(dn[k - 1], dn[k], t));
    }
    /* the tip curls in or out */
    if (L->curl != 0)
        for (int i = 0; i < LOCK_PTS; i++) {
            float t = (float)i / (LOCK_PTS - 1), c = smoothf(0.6f, 1.0f, t);
            v3 radial = V3(P[i].x - hi->center.x, 0, P[i].z - hi->center.z);
            radial = vlen(radial) > 1e-4f ? vnorm(radial) : V3(0, 0, -1);
            P[i] = vmadd(P[i], radial, L->curl * c * c * 0.03f);
            P[i].y += fabsf(L->curl) * c * c * c * 0.012f;
        }
    *len_out = acc;
    return LOCK_PTS;
}

static void sweep_lock(mbuild *b, const v3 *P, const v3 *O, float w0, float bulge, float blunt, int part) {
    enum { K = 5 };
    if (!mb_room(b, LOCK_PTS * K, (LOCK_PTS - 1) * (K - 1) * 2))
        return;
    int base = b->nv;
    for (int i = 0; i < LOCK_PTS; i++) {
        float t = (float)i / (LOCK_PTS - 1);
        v3 d = i == 0 ? vsub(P[1], P[0]) : i == LOCK_PTS - 1 ? vsub(P[i], P[i - 1]) : vsub(P[i + 1], P[i - 1]);
        d = vnorm(d);
        v3 s = vcross(d, O[i]);
        if (vlen(s) < 1e-5f)
            s = vcross(d, V3(0, 0, 1));
        s = vnorm(s);
        v3 o = vnorm(vcross(s, d));
        float pointed = powf(1 - powf(t, 2.2f), 0.9f) * (0.75f + 0.25f * smoothf(0, 0.3f, t));
        float w = w0 * lerpf(pointed, 1 - 0.12f * t, blunt);
        for (int k = 0; k < K; k++) {
            float a = -1 + 2.0f * k / (K - 1);
            v3 p = vmadd(vmadd(P[i], s, a * w * 0.5f), o, bulge * w * (1 - a * a) * (0.3f + 0.7f * smoothf(0.05f, 0.45f, t)));
            mb_vert(b, p, (a + 1) * 0.5f, t, t);
        }
    }
    for (int i = 0; i < LOCK_PTS - 1; i++)
        for (int k = 0; k < K - 1; k++) {
            int a = base + i * K + k, c = a + 1, d = a + K, e = d + 1;
            mb_tri(b, a, c, d, part);
            mb_tri(b, c, e, d, part);
        }
}

static void add_lock(const hctx *cx, const lockdesc *L) {
    mbuild *b = cx->b;
    hair_info *out = cx->out;
    v3 P[LOCK_PTS], O[LOCK_PTS];
    float len = 0;
    if (!lock_path(cx, L, P, O, &len))
        return;
    int part = out->nlocks + 1;
    int first = b->nv;
    sweep_lock(b, P, O, L->w, L->bulge, L->blunt, part);
    if (out->nlocks < HAIR_MAX_LOCKS) {
        memcpy(out->lock[out->nlocks].p, P, sizeof P);
        out->lock[out->nlocks].first_vert = first;
        out->lock[out->nlocks].nverts = b->nv - first;
        out->lock[out->nlocks].len = len;
        out->nlocks++;
    }
}

/* the ahoge: one lock standing up from the crown and arching forwards */
static void add_ahoge(mbuild *b, const head_info *hi, const hair_params *hp, hair_info *out) {
    v3 P[LOCK_PTS], O[LOCK_PTS];
    v3 root = vadd(hi->center, vmadd(head_point_at(&hi->hp, 0.3f, 1.25f), head_normal_at(&hi->hp, 0.3f, 1.25f),
                                     hair_shell(hp, 1.25f) - 0.004f));
    v3 c1 = vadd(root, V3(0.004f, 0.05f, -0.012f)), c2 = vadd(root, V3(0.01f, 0.065f, 0.035f));
    v3 end = vadd(root, V3(0.012f, 0.04f, 0.052f));
    for (int i = 0; i < LOCK_PTS; i++) {
        float t = (float)i / (LOCK_PTS - 1), u = 1 - t;
        P[i] = vadd(vadd(vmul(root, u * u * u), vmul(c1, 3 * u * u * t)), vadd(vmul(c2, 3 * u * t * t), vmul(end, t * t * t)));
        O[i] = V3(1, 0, 0);
    }
    int first = b->nv;
    sweep_lock(b, P, O, 0.026f, 0.2f, 0, out->nlocks + 1);
    if (out->nlocks < HAIR_MAX_LOCKS) {
        memcpy(out->lock[out->nlocks].p, P, sizeof P);
        out->lock[out->nlocks].first_vert = first;
        out->lock[out->nlocks].nverts = b->nv - first;
        out->lock[out->nlocks].len = 0.11f;
        out->nlocks++;
    }
}

static float hash1(int i) {
    unsigned h = (unsigned)i * 2654435761u;
    h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
    return (h & 0xffff) / 65535.0f;
}

/* ---- styles ---- */

static void bangs(const hctx *cx) {
    const hair_params *hp = cx->hp;
    if (hp->bangs == BANGS_NONE)
        return;
    int n = hp->bangs == BANGS_BLUNT ? 11 : 5;
    /* two layers: finer strands underneath, between the main clumps on top */
    for (int layer = 1; layer >= 0; layer--) {
        int m = layer ? n - 1 : n;
        for (int i = 0; i < m; i++) {
            float f = layer ? -1 + 2.0f * (i + 0.5f) / (n - 1) : -1 + 2.0f * i / (n - 1); /* -1 her right .. 1 her left */
            float j = hash1(i * 7 + layer * 31 + 3) - 0.5f;
            lockdesc L = {0};
            L.th = f * 0.95f + j * 0.05f;
            L.phi = 1.2f - 0.22f * f * f;
            L.release = -9;
            L.ytip = -9;
            L.len = 0.4f;
            /* down to the eyes in the middle, longer at the sides */
            L.phi_tip = 0.1f - 0.12f * f * f + j * 0.12f - (layer ? 0.04f : 0) + ((i & 1) ? 0.05f : 0);
            L.w = layer ? 0.03f : ((i & 1) ? 0.05f : 0.062f) + 0.006f * (1 - f * f);
            L.bulge = 0.24f;
            L.lift = 0.006f + hp->volume * 0.006f;
            L.layer = layer ? 0.0f : 0.004f;
            switch (hp->bangs) {
            case BANGS_SIDE: { /* swept away from the parting */
                float d = f - hp->part;
                L.sweep = (d > 0 ? 0.42f : -0.3f) * (0.5f + 0.5f * fabsf(d)) + 0.1f * j;
                L.phi_tip -= d > 0 ? 0.05f * d : 0.02f * d;
                break;
            }
            case BANGS_PARTED: { /* parted, falling to each side like curtains */
                float side = f >= hp->part * 0.3f ? 1.0f : -1.0f;
                L.sweep = side * 0.5f * (1 - 0.45f * fabsf(f));
                L.phi_tip += 0.05f * (1 - fabsf(f));
                break;
            }
            case BANGS_BLUNT: /* hime: cut straight across */
                L.blunt = 0.85f;
                L.phi_tip = 0.2f - 0.04f * f * f;
                L.w = 0.032f;
                L.sweep = f * 0.04f;
                break;
            default: /* full: falling straight, fanned a little, uneven */
                L.sweep = f * 0.1f + j * 0.08f;
                break;
            }
            add_lock(cx, &L);
        }
    }
}

static void side_locks(const hctx *cx, float ytip) {
    const hair_params *hp = cx->hp;
    for (int s = -1; s <= 1; s += 2)
        for (int k = 0; k < 3; k++) {
            float j = hash1(k * 11 + s * 5 + 40) - 0.5f;
            lockdesc L = {0};
            L.th = s * (1.0f + 0.2f * k);
            L.phi = 0.9f - 0.08f * k;
            L.release = 0.15f - 0.12f * k;
            L.len = 0.6f;
            L.ytip = ytip + 0.012f * k + j * 0.01f;
            L.phi_tip = -9;
            L.w = 0.036f - 0.004f * k;
            L.bulge = 0.24f;
            L.lift = 0.003f;
            L.layer = 0.006f - 0.002f * k;
            L.flare = 0.1f;
            L.curl = hp->curl * 0.6f;
            L.sweep = s * 0.08f;
            L.blunt = hp->style == HAIR_HIME ? 0.8f : 0;
            add_lock(cx, &L);
        }
}

static void back_hair(const hctx *cx, float ytip, int n) {
    const hair_params *hp = cx->hp;
    /* an inner layer, a little longer, and an outer one over its gaps */
    for (int layer = 0; layer < 2; layer++)
        for (int i = 0; i < n; i++) {
            float f = (i + 0.5f * layer) / (n - 1 + 0.5f * layer); /* 0..1 from her left round the back to her right */
            float th = 1.3f + f * (2 * PI_F - 2.6f);
            float j = hash1(i * 13 + layer * 101) - 0.5f;
            lockdesc L = {0};
            L.th = th;
            L.phi = 1.15f - 0.25f * layer + j * 0.1f;
            L.release = -0.05f + j * 0.08f;
            L.len = 2;
            L.ytip = ytip + (layer ? 0.008f : -0.006f) + j * 0.02f;
            L.phi_tip = -9;
            L.w = 0.056f + hp->volume * 0.012f;
            L.bulge = 0.22f;
            L.lift = 0.003f;
            L.layer = layer * 0.004f;
            L.flare = 0.1f + hp->volume * 0.12f;
            L.curl = hp->curl;
            L.blunt = hp->style == HAIR_HIME ? 0.85f : 0;
            L.sweep = j * 0.08f;
            add_lock(cx, &L);
        }
}

void hair_build(fmodel *m, const head_info *hi, const hair_params *hp, int bone, const collider *col, int ncol,
                hair_info *out) {
    memset(out, 0, sizeof *out);
    fmat mt = fig_mat(MAT_HAIR, hp->color);
    mt.two_sided = true;
    if (hp->tip != hp->color)
        mt.pattern = PAT_GRADIENT, mt.pat_col = hp->tip;
    out->mat = fig_add_mat(m, mt);
    mbuild b = {0};
    build_cap(&b, hi, hp);
    hctx cx = {&b, hi, hp, col, ncol, out};
    float chin = hi->center.y + head_point(&hi->hp, 0, -1, 0).y;
    float ytip = chin - hp->length;
    switch (hp->style) {
    case HAIR_SHORT:
    case HAIR_PIXIE:
        back_hair(&cx, hi->center.y - hi->hp.h * 0.3f, 14);
        side_locks(&cx, hi->center.y - hi->hp.h * 0.25f);
        break;
    case HAIR_LONG:
    case HAIR_HIME:
    case HAIR_WAVY:
        back_hair(&cx, ytip, 20);
        side_locks(&cx, ytip + 0.04f);
        break;
    default: /* bob */
        back_hair(&cx, ytip, 14);
        side_locks(&cx, ytip + 0.005f);
        break;
    }
    bangs(&cx);
    if (hp->ahoge)
        add_ahoge(&b, hi, hp, out);

    fmesh *me = fig_add_mesh(m, b.nv, b.nt, out->mat, true, true);
    out->mesh = m->nmeshes - 1;
    if (me) {
        memcpy(me->pos, b.pos, sizeof *b.pos * b.nv);
        memcpy(me->uv, b.uv, sizeof *b.uv * 2 * b.nv);
        memcpy(me->aux, b.aux, sizeof *b.aux * b.nv);
        memcpy(me->tri, b.tri, sizeof *b.tri * 3 * b.nt);
        me->tpart = malloc(b.nt);
        if (me->tpart)
            memcpy(me->tpart, b.part, b.nt);
        fig_mesh_normals(me);
        fig_mesh_rigid(me, bone);
    }
    free(b.pos), free(b.uv), free(b.aux), free(b.tri), free(b.part);
}
