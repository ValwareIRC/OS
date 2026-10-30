/* Clothes, fitted to the body: tops, shorts, trousers, socks and shoes are
 * the body's own shape grown by the cloth's thickness and cut to where the
 * garment covers (a hem, a neckline, how far the sleeves go), so they fit
 * exactly and move with the body. Skirts hang from the waist as a cone
 * wide enough at every height to clear the hips and legs, flared and
 * pleated. Everything is built in the body's T-pose. */
#include <stdlib.h>
#include <string.h>

#include "outfit.h"
#include "prim.h"

outfit_params outfit_defaults(void) {
    return (outfit_params){.top = TOP_SWEATER, .bottom = BOTTOM_PLEATED, .legs = LEGS_THIGHHIGHS, .shoes = SHOES_LOAFERS,
                           .neckline = NECK_ROUND, .top_col = 0xf3eef8, .top_col2 = 0x8a7ad8, .bottom_col = 0x3b3f78,
                           .bottom_col2 = 0x9aa6e8, .legs_col = 0x2a2838, .shoes_col = 0x4a3036, .sole_col = 0x2a1e22,
                           .top_pattern = PAT_NONE, .bottom_pattern = PAT_PLAID, .sleeve = 1.0f, .hem = 0.55f,
                           .skirt_len = 0.62f, .flare = 0.55f};
}

/* ---- garments cut from the grown body ---- */

typedef struct {
    const body_info *bi;
    float thick;
    float y_top, y_bot;  /* covers y_bot < y < y_top */
    float sleeve_x;      /* the arms are covered out to |x| = this (0: not at all) */
    int neck;            /* a neckline at the top (-1: none) */
} garment;

static float neck_cut(const garment *g, v3 p) {
    const body_info *bi = g->bi;
    float s = bi->s;
    float front = smoothf(-0.02f * s, 0.05f * s, p.z), near = 1 - smoothf(0.06f * s, 0.14f * s, fabsf(p.x));
    float depth;
    switch (g->neck) {
    case NECK_SCOOP: depth = (0.03f + 0.07f * front) * s; break;
    case NECK_V: depth = (0.02f + 0.12f * front * clampf(1 - fabsf(p.x) / (0.07f * s), 0, 1)) * s; break;
    case NECK_HIGH: depth = -0.035f * s; break;
    default: depth = (0.004f + 0.016f * front) * s; break;
    }
    return p.y - (bi->neck_y + 0.012f * s - depth * near + (1 - near) * 0.3f * s); /* away from the neck, no cut */
}

/* where the garment covers: negative inside */
static float garment_region(const garment *g, v3 p) {
    const body_info *bi = g->bi;
    float r = fmaxq(p.y - g->y_top, g->y_bot - p.y);
    /* the arms: cut where the sleeves end */
    float ax = fabsf(p.x) - bi->shoulder_x;
    if (ax > -0.03f * bi->s && p.y > bi->shoulder_y - 0.08f * bi->s)
        r = fmaxq(r, fabsf(p.x) - (g->sleeve_x > 0 ? g->sleeve_x : bi->shoulder_x - 0.012f * bi->s));
    if (g->neck >= 0)
        r = fmaxq(r, neck_cut(g, p));
    return r;
}

static float garment_sdf(void *ctx, v3 p) {
    const garment *g = ctx;
    float d = body_sdf((void *)g->bi, p) - g->thick;
    /* a softly rounded edge where it's cut: clean hems and collars */
    return -smin(-d, -garment_region(g, p), 0.003f * g->bi->s);
}

/* the body's skin under a garment, well away from its edges, is never seen */
static void mark_covered(const body_info *bi, const fmodel *m, const garment *g, uint8_t *cov) {
    const fmesh *me = &m->mesh[bi->mesh];
    float margin = -0.009f * bi->s;
    for (int i = 0; i < me->nv; i++)
        if (!cov[i] && garment_region(g, me->pos[i]) < margin)
            cov[i] = 1;
}

/* coordinates for patterns, in metres: round the body and along it */
static void cloth_uv(const body_info *bi, v3 p, float *u, float *v) {
    if (fabsf(p.x) > bi->shoulder_x && p.y > bi->shoulder_y - 0.08f * bi->s) { /* an arm */
        *u = atan2f(p.y - bi->shoulder_y, p.z) * 0.035f * bi->s;
        *v = fabsf(p.x);
    } else {
        *u = atan2f(p.x, p.z) * 0.11f * bi->s;
        *v = p.y;
    }
}

static fmesh *mesh_garment(fmodel *m, const body_info *bi, garment *g, int mat, float cell) {
    float s = bi->s;
    float reach = (g->sleeve_x > 0 ? g->sleeve_x : bi->shoulder_x) + 0.06f * s;
    v3 lo = V3(-reach, fmaxq(-0.02f, g->y_bot - 0.02f), -0.17f * s), hi = V3(reach, fminq(bi->chin_y, g->y_top + 0.02f), 0.21f * s);
    smesh sm;
    if (!sdf_mesh(garment_sdf, g, lo, hi, cell, &sm) || sm.nt == 0) {
        smesh_free(&sm);
        return NULL;
    }
    fmesh *me = fig_add_mesh(m, sm.nv, sm.nt, mat, true, true);
    if (me) {
        memcpy(me->pos, sm.pos, sizeof *sm.pos * sm.nv);
        memcpy(me->nrm, sm.nrm, sizeof *sm.nrm * sm.nv);
        memcpy(me->tri, sm.tri, sizeof *sm.tri * 3 * sm.nt);
        for (int i = 0; i < sm.nv; i++) {
            /* move as the body under it does */
            v3 inner = vmadd(sm.pos[i], sm.nrm[i], -g->thick);
            body_weights(bi, inner, me->bi[i], me->bw[i]);
            float u, v;
            cloth_uv(bi, sm.pos[i], &u, &v);
            me->uv[i * 2] = u, me->uv[i * 2 + 1] = v;
            me->aux[i] = v;
        }
    }
    smesh_free(&sm);
    return me;
}

static int cloth_mat(fmodel *m, rgb col, int pattern, rgb col2) {
    fmat mt = fig_mat(MAT_CLOTH, col);
    if (pattern) {
        mt.pattern = pattern, mt.pat_col = col2, mt.pat_col2 = rgb_mix(col2, 0xffffff, 0.5f);
        mt.pat_scale = pattern == PAT_PLAID ? 24 : pattern == PAT_STRIPES ? 22 : pattern == PAT_DOTS ? 34 : 18;
    }
    return fig_add_mat(m, mt);
}

/* ---- skirts ---- */

/* how far the body reaches in each direction round (0 = the front) at height y, from (0, y, zc) */
static void body_reach(const body_info *bi, float y, float zc, int nth, float *out) {
    for (int j = 0; j < nth; j++)
        out[j] = 0;
    float s = bi->s, step = 0.006f * s;
    for (float x = -0.26f * s; x <= 0.26f * s; x += step)
        for (float z = -0.2f * s; z <= 0.2f * s; z += step) {
            if (body_sdf((void *)bi, V3(x, y, z)) > 0)
                continue;
            for (int j = 0; j < nth; j++) {
                float th = 2 * PI_F * j / nth;
                float h = x * sinf(th) + (z - zc) * cosf(th);
                if (h > out[j])
                    out[j] = h;
            }
        }
}

static void skirt(fmodel *m, const body_info *bi, const outfit_params *op, int mat, bool pleated, float len) {
    enum { NTH = 64, NR = 16 };
    float s = bi->s;
    float y0 = bi->waist_y + 0.008f * s;
    float knee = bi->knee[0].y;
    float y1 = bi->crotch_y - len * (bi->crotch_y - knee);
    if (y1 > bi->crotch_y - 0.06f * s)
        y1 = bi->crotch_y - 0.06f * s;
    float zc = -0.01f * s;
    float reach[NTH], r[NR][NTH];
    for (int i = 0; i < NR; i++) {
        float t = (float)i / (NR - 1);
        float y = lerpf(y0, y1, t);
        body_reach(bi, y, zc, NTH, reach);
        for (int j = 0; j < NTH; j++) {
            float rr = reach[j] + (0.011f + 0.004f * t) * s;
            if (i > 0 && rr < r[i - 1][j]) /* never narrower further down */
                rr = r[i - 1][j];
            r[i][j] = rr;
        }
    }
    /* smooth round, then flare */
    for (int i = 0; i < NR; i++) {
        float tmp[NTH];
        for (int pass = 0; pass < 3; pass++) {
            for (int j = 0; j < NTH; j++)
                tmp[j] = (r[i][(j + NTH - 1) % NTH] + 2 * r[i][j] + r[i][(j + 1) % NTH]) * 0.25f;
            for (int j = 0; j < NTH; j++)
                r[i][j] = fmaxq(tmp[j], r[i][j] * 0.98f);
        }
        float t = (float)i / (NR - 1);
        for (int j = 0; j < NTH; j++)
            r[i][j] += op->flare * 0.16f * s * t * t * (y0 - y1) / (0.3f * s);
    }
    int ring = NTH + 1;
    fmesh *me = fig_add_mesh(m, NR * ring, (NR - 1) * NTH * 2, mat, true, true);
    if (!me)
        return;
    int lu = bi->bone[BN_LUPPERLEG], ru = bi->bone[BN_RUPPERLEG], hips = bi->bone[BN_HIPS];
    for (int i = 0; i < NR; i++) {
        float t = (float)i / (NR - 1);
        float y = lerpf(y0, y1, t);
        for (int j = 0; j <= NTH; j++) {
            float th = 2 * PI_F * j / NTH;
            float rr = r[i][j % NTH];
            if (pleated) { /* folds, deeper towards the hem */
                float f = th * 24 / (2 * PI_F);
                f -= (float)ifloor(f);
                rr *= 1 + (0.012f + 0.045f * t) * (f < 0.5f ? f * 2 : 2 - f * 2);
            }
            int k = i * ring + j;
            me->pos[k] = V3(sinf(th) * rr, y, zc + cosf(th) * rr);
            me->uv[k * 2] = th * 0.12f * s;
            me->uv[k * 2 + 1] = y;
            me->aux[k] = y;
            /* the hips at the waist; towards the hem, the leg on that side too */
            float side = sinf(th);
            float wl = t * 0.55f * smoothf(-0.2f, 0.6f, side), wr = t * 0.55f * smoothf(-0.2f, 0.6f, -side);
            me->bi[k][0] = (uint8_t)hips, me->bw[k][0] = 1 - wl - wr;
            me->bi[k][1] = (uint8_t)lu, me->bw[k][1] = wl;
            me->bi[k][2] = (uint8_t)ru, me->bw[k][2] = wr;
            me->bw[k][3] = 0;
        }
    }
    int tc = 0;
    for (int i = 0; i < NR - 1; i++)
        for (int j = 0; j < NTH; j++) {
            int a = i * ring + j, b = a + 1, c = a + ring, d = c + 1;
            /* outwards: going round anticlockwise seen from above, then down */
            me->tri[tc++] = a, me->tri[tc++] = c, me->tri[tc++] = b;
            me->tri[tc++] = b, me->tri[tc++] = c, me->tri[tc++] = d;
        }
    fig_mesh_normals(me);
    m->mat[mat].two_sided = true;
}

/* ---- all of it ---- */

void outfit_build(fmodel *m, const body_info *bi, const outfit_params *op) {
    float s = bi->s, cell = 0.011f * s;
    fmesh *body = &m->mesh[bi->mesh];
    uint8_t *cov = calloc((size_t)body->nv, 1);
    float arm_len = bi->wrist[0].x - bi->shoulder_x;
    /* legs first: socks under shoes, trousers over socks */
    if (op->legs) {
        int mt = cloth_mat(m, op->legs_col, PAT_NONE, 0);
        float top = op->legs == LEGS_THIGHHIGHS ? bi->crotch_y - 0.11f * s
                    : op->legs == LEGS_KNEESOCKS ? bi->knee[0].y + 0.05f * s
                    : op->legs == LEGS_TIGHTS    ? bi->waist_y - 0.02f * s
                                                 : bi->ankle[0].y + 0.035f * s;
        garment g = {bi, 0.0018f * s, top, -1, 0, -1};
        if (mesh_garment(m, bi, &g, mt, cell) && cov)
            mark_covered(bi, m, &g, cov);
    }
    if (op->shoes) {
        int mt = fig_add_mat(m, fig_mat(MAT_SHINY, op->shoes_col));
        m->mat[mt].spec = 0.35f;
        float top = op->shoes == SHOES_BOOTS ? bi->knee[0].y - 0.05f * s
                    : op->shoes == SHOES_LOAFERS ? bi->ankle[0].y + 0.005f * s
                                                 : bi->ankle[0].y + 0.025f * s;
        garment g = {bi, 0.0065f * s, top, -1, 0, -1};
        if (mesh_garment(m, bi, &g, mt, cell * 0.8f) && cov)
            mark_covered(bi, m, &g, cov);
        int sm = fig_add_mat(m, fig_mat(MAT_CLOTH, op->sole_col));
        garment sole = {bi, 0.0095f * s, 0.012f * s, -1, 0, -1};
        mesh_garment(m, bi, &sole, sm, cell * 0.8f);
    }
    int bottom = op->bottom, top = op->top;
    if (top == TOP_DRESS && !bottom)
        bottom = BOTTOM_LONGSKIRT;
    if (bottom == BOTTOM_SHORTS || bottom == BOTTOM_PANTS) {
        int mt = cloth_mat(m, op->bottom_col, op->bottom_pattern, op->bottom_col2);
        float cut = bottom == BOTTOM_SHORTS ? bi->crotch_y - 0.075f * s : bi->ankle[0].y + 0.018f * s;
        garment g = {bi, 0.0055f * s, bi->waist_y + 0.004f * s, cut, 0, -1};
        if (mesh_garment(m, bi, &g, mt, cell) && cov)
            mark_covered(bi, m, &g, cov);
    }
    if (top) {
        int mt = top == TOP_DRESS ? cloth_mat(m, op->top_col, op->top_pattern, op->top_col2)
                                  : cloth_mat(m, op->top_col, op->top_pattern, op->top_col2);
        float sleeve = top == TOP_TANK ? 0 : top == TOP_TEE || top == TOP_CROP ? fminq(op->sleeve, 0.3f) : op->sleeve;
        float hem = top == TOP_CROP ? bi->waist_y + 0.05f * s
                    : top == TOP_DRESS ? bi->waist_y
                                       : lerpf(bi->waist_y + 0.02f * s, bi->hips_y - 0.035f * s, op->hem);
        garment g = {bi, (top == TOP_SWEATER ? 0.0105f : 0.008f) * s, 10, hem,
                     sleeve > 0.02f ? bi->shoulder_x + sleeve * (arm_len - 0.012f * s) : 0,
                     top == TOP_TANK ? NECK_SCOOP : op->neckline};
        if (mesh_garment(m, bi, &g, mt, cell * 0.75f) && cov)
            mark_covered(bi, m, &g, cov);
        if (top == TOP_DRESS)
            skirt(m, bi, op, mt, false, fmaxq(op->skirt_len, 1.0f));
    }
    if (bottom == BOTTOM_SKIRT || bottom == BOTTOM_PLEATED || bottom == BOTTOM_LONGSKIRT) {
        int mt = cloth_mat(m, op->bottom_col, op->bottom_pattern, op->bottom_col2);
        skirt(m, bi, op, mt, bottom == BOTTOM_PLEATED, bottom == BOTTOM_LONGSKIRT ? 1.5f : op->skirt_len);
    }
    if (cov) {
        body = &m->mesh[bi->mesh];
        int keep = 0;
        for (int t = 0; t < body->nt; t++) {
            int a = body->tri[t * 3], b = body->tri[t * 3 + 1], c = body->tri[t * 3 + 2];
            if (cov[a] && cov[b] && cov[c])
                continue;
            body->tri[keep * 3] = a, body->tri[keep * 3 + 1] = b, body->tri[keep * 3 + 2] = c;
            keep++;
        }
        body->nt = keep;
        free(cov);
    }
}
