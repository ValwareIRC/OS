/* Drawing the figure in software: skinning, projection, triangle filling with
 * a depth buffer, toon shading after MToon (lit and shade colours with a
 * step, rim light, the anime "ring" on hair), shadows the figure casts on
 * itself (baked into the vertices, and optionally a live shadow map),
 * outlines from depth and part edges (as anime line art does),
 * supersampling down to smooth edges, and the split between what is in
 * front of the portal (her frame) and what is behind it, so she can lean
 * out of it.
 *
 * Triangles only record which of them is nearest at each sample (a
 * "visibility buffer"); each visible sample then works out its own normal
 * and texture coordinates and is shaded once. Layers of hair hidden under
 * other layers cost almost nothing, and where the four samples of an output
 * pixel all show the same part, it is shaded once for all four. The work is
 * split into tasks of a few rows, taken by whichever thread is free. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fig.h"

fig_stats_t fig_stats;
int fig_debug;
int fig_threads = 4;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

bool fig_buf_init(fbuf *b, int w, int h) {
    memset(b, 0, sizeof *b);
    b->w = w, b->h = h;
    size_t n = (size_t)w * h;
    b->z = malloc(n * sizeof *b->z);
    b->iz = malloc(n * sizeof *b->iz);
    b->col = malloc(n * sizeof *b->col);
    b->mat = malloc(n);
    b->front = malloc(n);
    b->id = malloc(n * sizeof *b->id);
    b->aux = malloc(n * sizeof *b->aux);
    b->vis = malloc(n * sizeof *b->vis);
    if (!b->z || !b->iz || !b->col || !b->mat || !b->front || !b->id || !b->aux || !b->vis) {
        fig_buf_free(b);
        return false;
    }
    fig_buf_clear(b);
    return true;
}

void fig_buf_free(fbuf *b) {
    free(b->z), free(b->iz), free(b->col), free(b->mat), free(b->front), free(b->id), free(b->aux), free(b->vis);
    memset(b, 0, sizeof *b);
}

void fig_buf_clear(fbuf *b) {
    size_t n = (size_t)b->w * b->h;
    memset(b->iz, 0, n * sizeof *b->iz);
    memset(b->vis, 0, n * sizeof *b->vis);
    memset(b->front, 0, n);
}

/* ---- vertices: skinned, in view space, projected ---- */

typedef struct {
    float sx, sy;  /* screen, internal pixels */
    float d;       /* view depth (positive, in front of the camera) */
    v3 n;          /* view-space normal */
    float u, v, aux;
    float sun;     /* baked: how much of the key light reaches it */
    float side;    /* signed distance to the portal plane */
} pvert;

typedef struct {
    int a, b, c;      /* into the shared vertex array */
    int mat;
    uint16_t id;      /* part id + 1 */
    bool back;        /* seen from behind (two-sided meshes) */
    float y0, y1;     /* its rows */
    float ia;         /* 1 / twice its screen area */
    float wa, wb, wc; /* 1 / each corner's depth */
} ptri;

static struct {
    v3 *wp, *wn;   /* skinned, model space */
    v3 *lp;        /* in the shadow map's space */
    int capw;
    pvert *v;
    int nv, capv;
    ptri *t;
    int nt, capt;
    int *st;       /* shadow casters: 3 vertex indices each */
    int nst, capst;
    int *bin_start, *bin_idx; /* each task's triangles */
    int capbs, capbi;
} W;

static bool grow(void **p, int *cap, int need, size_t sz) {
    if (need <= *cap)
        return true;
    int c = *cap ? *cap : 4096;
    while (c < need)
        c *= 2;
    void *q = realloc(*p, (size_t)c * sz);
    if (!q)
        return false;
    *p = q, *cap = c;
    return true;
}

static bool grow_world(int total) {
    if (total <= W.capw)
        return true;
    int c = W.capw ? W.capw : 4096;
    while (c < total)
        c *= 2;
    free(W.wp), free(W.wn), free(W.lp);
    W.wp = malloc(sizeof *W.wp * c);
    W.wn = malloc(sizeof *W.wn * c);
    W.lp = malloc(sizeof *W.lp * c);
    W.capw = W.wp && W.wn && W.lp ? c : 0;
    return W.capw != 0;
}

typedef struct {
    float fx, fy;   /* projection scale */
    float W2, H2;   /* half the internal size */
    bool ortho;
} proj;

static void skin_mesh(const fmesh *me, const fpose *p, int base) {
    for (int i = 0; i < me->nv; i++) {
        v3 bp = me->pos[i];
        for (int k = 0; k < me->nmorph; k++)
            if (me->mw[k] != 0 && me->morph[k])
                bp = vmadd(bp, me->morph[k][i], me->mw[k]);
        v3 wp = V3(0, 0, 0), wn = V3(0, 0, 0);
        for (int k = 0; k < 4; k++) {
            float w = me->bw[i][k];
            if (w <= 0)
                continue;
            const m4 *s = &p->skin[me->bi[i][k]];
            wp = vmadd(wp, m4point(s, bp), w);
            wn = vmadd(wn, m4dir(s, me->nrm[i]), w);
        }
        W.wp[base + i] = wp;
        W.wn[base + i] = wn;
    }
}

static void project_mesh(const fmesh *me, const fcam *cam, const proj *pr, int base) {
    m4 view = cam->view;
    for (int i = 0; i < me->nv; i++) {
        v3 wp = W.wp[base + i];
        v3 vp = m4point(&view, wp);
        v3 vn = vnorm(m4dir(&view, W.wn[base + i]));
        pvert *o = &W.v[base + i];
        float d = -vp.z;
        o->d = d;
        if (pr->ortho) {
            o->sx = pr->W2 + vp.x * pr->fx;
            o->sy = pr->H2 - vp.y * pr->fy;
        } else {
            float id = d > 1e-5f ? 1.0f / d : 1e5f;
            o->sx = pr->W2 + vp.x * pr->fx * id;
            o->sy = pr->H2 - vp.y * pr->fy * id;
        }
        o->n = vn;
        o->u = me->uv ? me->uv[i * 2] : 0;
        o->v = me->uv ? me->uv[i * 2 + 1] : 0;
        o->aux = me->aux ? me->aux[i] : 0;
        o->sun = me->sun ? me->sun[i] : 1;
        o->side = cam->portal ? vdot(vsub(wp, cam->portal_p), cam->portal_n) : -1;
    }
}

/* ---- running work over rows, on several threads ---- */

#define MAX_THREADS 16

typedef void (*rows_fn)(void *ctx, int y0, int y1);
typedef struct {
    rows_fn fn;
    void *ctx;
    int rows, step, ntasks;
    int next;
} rows_job;

static void *rows_main(void *arg) {
    rows_job *j = arg;
    for (;;) {
        int k = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);
        if (k >= j->ntasks)
            break;
        int y0 = k * j->step, y1 = y0 + j->step > j->rows ? j->rows : y0 + j->step;
        j->fn(j->ctx, y0, y1);
    }
    return NULL;
}

/* Helpers that stay for the life of the process: started once (before a
 * sandbox stops new threads being made), woken for each piece of work. */
static struct {
    pthread_t th[MAX_THREADS];
    int n;
    bool started;
    pthread_mutex_t mu;
    pthread_cond_t go, done;
    rows_job *job;
    unsigned gen;
    int busy;
} P = {.mu = PTHREAD_MUTEX_INITIALIZER, .go = PTHREAD_COND_INITIALIZER, .done = PTHREAD_COND_INITIALIZER};

static void *pool_worker(void *arg) {
    (void)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&P.mu);
        while (P.gen == seen)
            pthread_cond_wait(&P.go, &P.mu);
        seen = P.gen;
        rows_job *j = P.job;
        pthread_mutex_unlock(&P.mu);
        rows_main(j);
        pthread_mutex_lock(&P.mu);
        if (--P.busy == 0)
            pthread_cond_signal(&P.done);
        pthread_mutex_unlock(&P.mu);
    }
    return NULL;
}

void fig_pool_start(void) {
    if (P.started)
        return;
    P.started = true;
    int want = (fig_threads < 1 ? 1 : fig_threads > MAX_THREADS ? MAX_THREADS : fig_threads) - 1;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    for (int i = 0; i < want; i++)
        if (pthread_create(&P.th[P.n], &at, pool_worker, NULL) == 0)
            P.n++;
    pthread_attr_destroy(&at);
}

/* fn over rows 0..rows, in tasks of `step` rows (even, so pixel blocks stay whole) */
static void run_rows(rows_fn fn, void *ctx, int rows, int step) {
    rows_job j = {fn, ctx, rows, step, (rows + step - 1) / step, 0};
    fig_pool_start();
    if (P.n == 0 || j.ntasks < 2) {
        rows_main(&j);
        return;
    }
    pthread_mutex_lock(&P.mu);
    P.job = &j;
    P.busy = P.n;
    P.gen++;
    pthread_cond_broadcast(&P.go);
    pthread_mutex_unlock(&P.mu);
    rows_main(&j); /* this thread works too */
    pthread_mutex_lock(&P.mu);
    while (P.busy > 0)
        pthread_cond_wait(&P.done, &P.mu);
    pthread_mutex_unlock(&P.mu);
}

/* ---- the shadow map: depth seen from the light ---- */

#define SHADOW_MAX 1024

static struct {
    int size;
    float *z;          /* height towards the light; -inf where nothing */
    v3 ax, ay, az;     /* light space: az points at the light */
    float x0, y0, scale; /* light-space x,y to texels */
    float texel;       /* metres per texel */
    bool on;
} S;

/* where along a row a triangle's three weights are all at least -slack:
 * steps k0..k1 from the row's first sample; false if nowhere */
static inline bool row_span(float l0, float l1, float d0x, float d1x, float slack, int n, int *k0, int *k1) {
    float lo = 0, hi = (float)n;
    float ls[3] = {l0 + slack, l1 + slack, 1 - l0 - l1 + slack}, ds[3] = {d0x, d1x, -d0x - d1x};
    for (int k = 0; k < 3; k++) {
        if (ds[k] > 1e-12f)
            lo = fmaxq(lo, -ls[k] / ds[k]);
        else if (ds[k] < -1e-12f)
            hi = fminq(hi, -ls[k] / ds[k]);
        else if (ls[k] < 0)
            return false;
    }
    if (hi < lo)
        return false;
    *k0 = iceil(lo - 1e-4f), *k1 = ifloor(hi + 1e-4f);
    if (*k0 < 0)
        *k0 = 0;
    if (*k1 > n)
        *k1 = n;
    return *k0 <= *k1;
}

static void shadow_rows(void *ctx, int by0, int by1) {
    (void)ctx;
    int size = S.size;
    for (int i = by0 * size; i < by1 * size; i++)
        S.z[i] = -1e30f;
    for (int t = 0; t < W.nst; t++) {
        const v3 *P0 = &W.lp[W.st[t * 3]], *P1 = &W.lp[W.st[t * 3 + 1]], *P2 = &W.lp[W.st[t * 3 + 2]];
        int y0 = ifloor(fminq(P0->y, fminq(P1->y, P2->y))), y1 = iceil(fmaxq(P0->y, fmaxq(P1->y, P2->y)));
        if (y1 < by0 || y0 >= by1)
            continue;
        float area = (P1->x - P0->x) * (P2->y - P0->y) - (P2->x - P0->x) * (P1->y - P0->y);
        if (fabsf(area) < 1e-9f)
            continue;
        float ia = 1.0f / area;
        int x0 = ifloor(fminq(P0->x, fminq(P1->x, P2->x))), x1 = iceil(fmaxq(P0->x, fmaxq(P1->x, P2->x)));
        x0 = x0 < 0 ? 0 : x0, x1 = x1 > size - 1 ? size - 1 : x1;
        y0 = y0 < by0 ? by0 : y0, y1 = y1 > by1 - 1 ? by1 - 1 : y1;
        float d0x = -(P2->y - P1->y) * ia, d1x = -(P0->y - P2->y) * ia;
        for (int y = y0; y <= y1; y++) {
            float px = x0 + 0.5f, py = y + 0.5f;
            float l0 = ((P1->x - px) * (P2->y - py) - (P2->x - px) * (P1->y - py)) * ia;
            float l1 = ((P2->x - px) * (P0->y - py) - (P0->x - px) * (P2->y - py)) * ia;
            int k0, k1;
            /* a hair wider than the triangle: no light leaks between triangles */
            if (!row_span(l0, l1, d0x, d1x, 0.01f, x1 - x0, &k0, &k1))
                continue;
            l0 += d0x * k0, l1 += d1x * k0;
            float *row = &S.z[y * size];
            for (int x = x0 + k0; x <= x0 + k1; x++, l0 += d0x, l1 += d1x) {
                float z = P0->z * l0 + P1->z * l1 + P2->z * (1 - l0 - l1);
                if (z > row[x])
                    row[x] = z;
            }
        }
    }
}

static void shadow_build(const flight *L, int size, int ncasters) {
    S.on = false;
    if (size <= 0 || ncasters == 0)
        return;
    if (size > SHADOW_MAX)
        size = SHADOW_MAX;
    if (S.size != size) {
        free(S.z);
        S.z = malloc(sizeof *S.z * size * size);
        S.size = S.z ? size : 0;
        if (!S.z)
            return;
    }
    S.az = vnorm(L->dir);
    v3 up = fabsf(S.az.y) > 0.98f ? V3(0, 0, 1) : V3(0, 1, 0);
    S.ax = vnorm(vcross(up, S.az));
    S.ay = vcross(S.az, S.ax);
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < W.nv; i++) {
        float x = vdot(W.wp[i], S.ax), y = vdot(W.wp[i], S.ay);
        minx = fminq(minx, x), maxx = fmaxq(maxx, x), miny = fminq(miny, y), maxy = fmaxq(maxy, y);
    }
    float ext = fmaxq(maxx - minx, maxy - miny) * 1.02f + 1e-4f;
    S.scale = (size - 2) / ext;
    S.texel = ext / (size - 2);
    S.x0 = minx - (ext - (maxx - minx)) * 0.5f;
    S.y0 = miny - (ext - (maxy - miny)) * 0.5f;
    for (int i = 0; i < W.nv; i++) {
        v3 w = W.wp[i];
        W.lp[i] = V3((vdot(w, S.ax) - S.x0) * S.scale + 1, (vdot(w, S.ay) - S.y0) * S.scale + 1, vdot(w, S.az));
    }
    S.size = size;
    run_rows(shadow_rows, NULL, size, 16);
    S.on = true;
}

/* How much of the light reaches p (model space), 0..1, with soft edges. */
static inline float shadow_at(v3 p, v3 n) {
    if (!S.on)
        return 1;
    p = vmadd(p, n, S.texel * 1.5f); /* off the surface a little: no acne */
    float x = (vdot(p, S.ax) - S.x0) * S.scale + 1 - 0.5f, y = (vdot(p, S.ay) - S.y0) * S.scale + 1 - 0.5f;
    float z = vdot(p, S.az) + S.texel * 0.75f;
    int ix = ifloor(x), iy = ifloor(y);
    float fx = x - ix, fy = y - iy;
    float lit = 0;
    for (int k = 0; k < 4; k++) {
        int sx = ix + (k & 1), sy = iy + (k >> 1);
        float w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
        if (sx < 0 || sy < 0 || sx >= S.size || sy >= S.size || z >= S.z[sy * S.size + sx])
            lit += w;
    }
    return lit;
}

/* ---- shading ---- */

static inline uint32_t tex_sample(const ftex *t, float u, float v) {
    float x = u * t->w - 0.5f, y = v * t->h - 0.5f;
    int x0 = ifloor(x), y0 = ifloor(y);
    float fx = x - x0, fy = y - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : x0 >= t->w ? t->w - 1 : x0;
    x1 = x1 < 0 ? 0 : x1 >= t->w ? t->w - 1 : x1;
    y0 = y0 < 0 ? 0 : y0 >= t->h ? t->h - 1 : y0;
    y1 = y1 < 0 ? 0 : y1 >= t->h ? t->h - 1 : y1;
    uint32_t a = t->px[y0 * t->w + x0], b = t->px[y0 * t->w + x1], c = t->px[y1 * t->w + x0], d = t->px[y1 * t->w + x1];
    float wa = (1 - fx) * (1 - fy), wb = fx * (1 - fy), wc = (1 - fx) * fy, wd = fx * fy;
    float al = ((a >> 24) & 255) * wa + ((b >> 24) & 255) * wb + ((c >> 24) & 255) * wc + ((d >> 24) & 255) * wd;
    float r = ((a >> 16) & 255) * wa + ((b >> 16) & 255) * wb + ((c >> 16) & 255) * wc + ((d >> 16) & 255) * wd;
    float g = ((a >> 8) & 255) * wa + ((b >> 8) & 255) * wb + ((c >> 8) & 255) * wc + ((d >> 8) & 255) * wd;
    float bl = (a & 255) * wa + (b & 255) * wb + (c & 255) * wc + (d & 255) * wd;
    return (uint32_t)((int)(al + 0.5f) << 24 | (int)(r + 0.5f) << 16 | (int)(g + 0.5f) << 8 | (int)(bl + 0.5f));
}

static inline float tex_alpha(const ftex *t, float u, float v) {
    int x = (int)(u * t->w), y = (int)(v * t->h);
    x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x;
    y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y;
    return ((t->px[y * t->w + x] >> 24) & 255) / 255.0f;
}

static inline float hash2(float x, float y) {
    float h = sinf(x * 127.1f + y * 311.7f) * 43758.5453f;
    return h - (float)ifloor(h);
}

/* A pattern on cloth, from its coordinates. */
static rgb pattern(const fmat *mt, rgb base, float u, float v) {
    float s = mt->pat_scale > 0 ? mt->pat_scale : 12;
    float x = u * s, y = v * s;
    switch (mt->pattern) {
    case PAT_STRIPES: {
        float f = y - (float)ifloor(y);
        return f < 0.38f ? mt->pat_col : base;
    }
    case PAT_PLAID: {
        float fx = x - (float)ifloor(x), fy = y - (float)ifloor(y);
        rgb c = base;
        if (fx < 0.25f)
            c = rgb_mix(c, mt->pat_col, 0.6f);
        if (fy < 0.25f)
            c = rgb_mix(c, mt->pat_col, 0.6f);
        if (fabsf(fx - 0.62f) < 0.03f || fabsf(fy - 0.62f) < 0.03f)
            c = rgb_mix(c, mt->pat_col2 ? mt->pat_col2 : 0xffffff, 0.7f);
        return c;
    }
    case PAT_DOTS: {
        float cx = x - (float)ifloor(x) - 0.5f, cy = y - (float)ifloor(y) - 0.5f;
        return cx * cx + cy * cy < 0.07f ? mt->pat_col : base;
    }
    case PAT_STARS: {
        float gx = (float)ifloor(x), gy = (float)ifloor(y);
        float ox = 0.25f + 0.5f * hash2(gx, gy), oy = 0.25f + 0.5f * hash2(gy, gx);
        float dx = (x - gx) - ox, dy = (y - gy) - oy;
        float r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
        float star = 0.17f * (0.55f + 0.45f * cosf(a * 5));
        return r < star && hash2(gx + 3, gy) > 0.35f ? mt->pat_col : base;
    }
    case PAT_GRADIENT:
        return rgb_mix(base, mt->pat_col, clampf(v, 0, 1));
    case PAT_CHECK: {
        int cx = ifloor(x), cy = ifloor(y);
        return ((cx + cy) & 1) ? mt->pat_col : base;
    }
    }
    return base;
}

static inline rgb mul_rgb(rgb c, rgb l) {
    return (rgb)((((c >> 16) & 255) * ((l >> 16) & 255) / 255) << 16 | (((c >> 8) & 255) * ((l >> 8) & 255) / 255) << 8 |
                 ((c & 255) * (l & 255) / 255));
}

/* a sine good enough for patterns: a parabola per half period */
static inline float fsin(float x) {
    x *= 0.15915494f; /* turns */
    x -= (float)ifloor(x);
    float y = x < 0.5f ? x * 4 - 1 : 3 - x * 4; /* triangle -1..1 */
    return y * (2 - fabsf(y)) * -1;
}

static inline uint32_t shade(const fmodel *m, const fmat *mt, const flight *L, v3 Lv, v3 n, float u, float v, float aux,
                             float sun) {
    rgb base = mt->base;
    if (mt->tex >= 0 && mt->tex < m->ntex)
        base = tex_sample(&m->tex[mt->tex], u, v) & 0xffffff;
    if (mt->pattern)
        base = pattern(mt, base, u, mt->kind == MAT_CLOTH ? aux : v);
    if (mt->kind == MAT_HAIR) {
        /* drawn strands: a few fine lines along each lock */
        float s = u * 3.0f + 0.12f * fsin(v * 7.0f + u * 4.0f);
        float f = s - (float)ifloor(s);
        float line = smoothf(0.0f, 0.12f, f) * (1 - smoothf(0.16f, 0.3f, f));
        base = rgb_mix(base, rgb_scale(base, 0.88f), line * 0.35f * smoothf(0.1f, 0.4f, v));
    }
    float ndl = vdot(n, Lv);
    float half = mt->toony * 0.5f + 0.02f;
    float lit = smoothf(mt->shift - half, mt->shift + half, ndl);
    lit *= 1 - (1 - sun) * (mt->kind == MAT_FACE ? 0.55f : 0.9f);
    rgb shadec = mt->tex >= 0 ? rgb_mix(rgb_scale(base, 0.88f), mt->shade, 0.35f)
                 : mt->pattern ? rgb_mix(rgb_scale(base, 0.74f), mt->shade, 0.35f) /* the pattern shows in shade too */
                               : mt->shade;
    if (mt->kind == MAT_HAIR && mt->pattern) /* the shade follows the gradient */
        shadec = rgb_mix(mt->shade, rgb_mix(rgb_scale(base, 0.7f), mt->shade, 0.4f), clampf(v, 0, 1));
    rgb c = rgb_mix(shadec, base, lit);
    c = rgb_mix(c, mul_rgb(c, L->color), 0.5f); /* the light's colour, softly */
    c = rgb_mix(c, L->ambient, 0.08f);
    if (mt->kind == MAT_HAIR && mt->spec > 0) {
        /* the ring of light across the top of the head, broken into the locks */
        float jag = 0.06f * fsin(u * 6.2832f) + 0.04f * fsin(v * 40.0f);
        float y = n.y + jag;
        float band = smoothf(0.4f, 0.52f, y) * (1 - smoothf(0.64f, 0.76f, y)) * (1 - smoothf(0.6f, 0.9f, aux));
        c = rgb_mix(c, rgb_mix(base, 0xffffff, 0.6f), band * mt->spec * (0.4f + 0.6f * lit));
    } else if (mt->kind == MAT_SHINY && mt->spec > 0) {
        float h = powf(clampf(vdot(n, vnorm(vadd(Lv, V3(0, 0, 1)))), 0, 1), 24);
        c = rgb_mix(c, 0xffffff, h * mt->spec * sun);
    }
    if (mt->rim > 0) { /* rim light at the silhouette */
        float f = clampf(1 - fabsf(n.z), 0, 1) * (1 - 0.8f * clampf(n.y, 0, 1)); /* edges, not tops */
        float pw = L->rim_power > 0 ? L->rim_power : 3;
        float r = (pw == 3 ? f * f * f : powf(f, pw)) * mt->rim * (0.45f + 0.55f * lit);
        int rr = (int)((c >> 16) & 255) + (int)(((L->rim_col >> 16) & 255) * r);
        int gg = (int)((c >> 8) & 255) + (int)(((L->rim_col >> 8) & 255) * r);
        int bb = (int)(c & 255) + (int)((L->rim_col & 255) * r);
        c = (rgb)((rr > 255 ? 255 : rr) << 16 | (gg > 255 ? 255 : gg) << 8 | (bb > 255 ? 255 : bb));
    }
    if (mt->emit > 0)
        c = rgb_mix(c, base, mt->emit);
    return 0xff000000u | c;
}

/* ---- triangles: which one is nearest at each sample ---- */

typedef struct {
    fbuf *b;
    const fcam *cam;
    const flight *L;
    const fmodel *m;
    v3 Lv;
    m4 inv_view;
    proj pr;
    int rows;      /* per task */
    double rast_ms, shade_ms;
    pthread_mutex_t lock;
} frame;

static void draw_tri(const frame *fr, int ti, int by0, int by1) {
    fbuf *b = fr->b;
    const ptri *t = &W.t[ti];
    const pvert *A = &W.v[t->a], *B = &W.v[t->b], *C = &W.v[t->c];
    int x0 = ifloor(fminq(A->sx, fminq(B->sx, C->sx))), x1 = iceil(fmaxq(A->sx, fmaxq(B->sx, C->sx)));
    int y0 = ifloor(t->y0), y1 = iceil(t->y1);
    if (x0 < 0) x0 = 0;
    if (x1 > b->w - 1) x1 = b->w - 1;
    if (y0 < by0) y0 = by0;
    if (y1 > by1 - 1) y1 = by1 - 1;
    if (x0 > x1 || y0 > y1)
        return;
    float ia = t->ia, wa = t->wa, wb = t->wb, wc = t->wc;
    const fmat *mt = &fr->m->mat[t->mat];
    const ftex *cut = mt->cutout && mt->tex >= 0 ? &fr->m->tex[mt->tex] : NULL;
    float izmax = 1.0f / fr->cam->znear;
    int32_t vis = ti + 1;
    /* the weights change by a constant step along a row */
    float d0x = -(C->sy - B->sy) * ia, d1x = -(A->sy - C->sy) * ia;
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f, px = x0 + 0.5f;
        float l0 = ((B->sx - px) * (C->sy - py) - (C->sx - px) * (B->sy - py)) * ia;
        float l1 = ((C->sx - px) * (A->sy - py) - (A->sx - px) * (C->sy - py)) * ia;
        int k0, k1;
        if (!row_span(l0, l1, d0x, d1x, 0, x1 - x0, &k0, &k1))
            continue;
        l0 += d0x * k0, l1 += d1x * k0;
        size_t row = (size_t)y * b->w;
        float *iz = &b->iz[row];
        int32_t *vb = &b->vis[row];
        for (int x = x0 + k0; x <= x0 + k1; x++, l0 += d0x, l1 += d1x) {
            float l2 = 1 - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0)
                continue;
            float qs = l0 * wa + l1 * wb + l2 * wc;
            if (qs <= iz[x] || qs > izmax)
                continue;
            if (cut) {
                float d = 1.0f / qs;
                float q0 = l0 * wa * d, q1 = l1 * wb * d, q2 = l2 * wc * d;
                if (tex_alpha(cut, A->u * q0 + B->u * q1 + C->u * q2, A->v * q0 + B->v * q1 + C->v * q2) < 0.5f)
                    continue;
            }
            iz[x] = qs;
            vb[x] = vis;
        }
    }
}

/* what a visible sample needs to be shaded, from its triangle */
typedef struct {
    float d;
    v3 n;
    float u, v, aux, sun, side;
} attrs;

static inline bool tri_attrs(const ptri *t, float px, float py, attrs *o) {
    const pvert *A = &W.v[t->a], *B = &W.v[t->b], *C = &W.v[t->c];
    float l0 = ((B->sx - px) * (C->sy - py) - (C->sx - px) * (B->sy - py)) * t->ia;
    float l1 = ((C->sx - px) * (A->sy - py) - (A->sx - px) * (C->sy - py)) * t->ia;
    float l2 = 1 - l0 - l1;
    float q0 = l0 * t->wa, q1 = l1 * t->wb, q2 = l2 * t->wc, qs = q0 + q1 + q2;
    if (qs <= 1e-6f)
        return false;
    float d = 1.0f / qs;
    q0 *= d, q1 *= d, q2 *= d;
    o->d = d;
    o->n = V3(A->n.x * q0 + B->n.x * q1 + C->n.x * q2, A->n.y * q0 + B->n.y * q1 + C->n.y * q2,
              A->n.z * q0 + B->n.z * q1 + C->n.z * q2);
    if (t->back)
        o->n = vmul(o->n, -1);
    o->u = A->u * q0 + B->u * q1 + C->u * q2;
    o->v = A->v * q0 + B->v * q1 + C->v * q2;
    o->aux = A->aux * q0 + B->aux * q1 + C->aux * q2;
    o->sun = A->sun * q0 + B->sun * q1 + C->sun * q2;
    o->side = A->side * q0 + B->side * q1 + C->side * q2;
    return true;
}

/* how much of the key light reaches a point seen at (px, py), depth d: the
 * baked part times the live shadow map, if there is one */
static inline float sun_at(const frame *fr, const fmat *mt, float px, float py, float d, v3 n, float baked) {
    if (mt->kind == MAT_FACE || mt->kind == MAT_GLOW) /* faces stay clean, as in VRoid */
        return 1;
    if (!S.on)
        return baked;
    const proj *pr = &fr->pr;
    v3 vp = pr->ortho ? V3((px - pr->W2) / pr->fx, -(py - pr->H2) / pr->fy, -d)
                      : V3((px - pr->W2) / pr->fx * d, -(py - pr->H2) / pr->fy * d, -d);
    return baked * shadow_at(m4point(&fr->inv_view, vp), m4dir(&fr->inv_view, n));
}

static inline uint32_t shade_attrs(const frame *fr, const fmat *mt, const attrs *a, float px, float py) {
    v3 n = vnorm(a->n);
    float sun = sun_at(fr, mt, px, py, a->d, n, a->sun);
    if (fig_debug == 1)
        return 0xff000000u | (uint32_t)(clampf(sun, 0, 1) * 255) * 0x010101u;
    return shade(fr->m, mt, fr->L, fr->Lv, n, a->u, a->v, a->aux, sun);
}

static void shade_one(const frame *fr, size_t i, int x, int y) {
    fbuf *b = fr->b;
    const ptri *t = &W.t[b->vis[i] - 1];
    attrs a;
    if (!tri_attrs(t, x + 0.5f, y + 0.5f, &a)) {
        b->col[i] = 0;
        return;
    }
    b->aux[i] = a.aux;
    if (fr->cam->portal)
        b->front[i] = a.side > 0;
    b->col[i] = shade_attrs(fr, &fr->m->mat[t->mat], &a, x + 0.5f, y + 0.5f);
}

static void shade_rows(const frame *fr, int y0, int y1) {
    fbuf *b = fr->b;
    int w = b->w;
    /* what's where, for the outlines and for finding whole blocks */
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            int32_t v = b->vis[i];
            if (!v) {
                b->mat[i] = 0, b->col[i] = 0;
                continue;
            }
            const ptri *t = &W.t[v - 1];
            b->mat[i] = (uint8_t)(t->mat + 1);
            b->id[i] = t->id;
            b->z[i] = 1.0f / b->iz[i];
        }
    bool blocks = fr->cam->ss == 2 && !fr->cam->portal;
    if (!blocks) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++)
                if (b->vis[(size_t)y * w + x])
                    shade_one(fr, (size_t)y * w + x, x, y);
        return;
    }
    for (int y = y0; y < y1; y += 2)
        for (int x = 0; x < w; x += 2) {
            size_t i0 = (size_t)y * w + x, i1 = i0 + 1, i2 = i0 + w, i3 = i2 + 1;
            bool whole = x + 1 < w && y + 1 < y1 && b->vis[i0] && b->vis[i1] && b->vis[i2] && b->vis[i3] &&
                         b->id[i0] == b->id[i1] && b->id[i0] == b->id[i2] && b->id[i0] == b->id[i3] &&
                         b->mat[i0] == b->mat[i1] && b->mat[i0] == b->mat[i2] && b->mat[i0] == b->mat[i3];
            const fmat *mt = whole ? &fr->m->mat[b->mat[i0] - 1] : NULL;
            attrs a;
            if (whole && mt->kind != MAT_FACE && tri_attrs(&W.t[b->vis[i0] - 1], x + 1.0f, y + 1.0f, &a)) {
                /* one shading at the block's centre, for all four */
                uint32_t c = shade_attrs(fr, mt, &a, x + 1.0f, y + 1.0f);
                b->col[i0] = b->col[i1] = b->col[i2] = b->col[i3] = c;
                b->aux[i0] = b->aux[i1] = b->aux[i2] = b->aux[i3] = a.aux;
                continue;
            }
            for (int k = 0; k < 4; k++) {
                int xx = x + (k & 1), yy = y + (k >> 1);
                if (xx >= w || yy >= y1)
                    continue;
                size_t i = (size_t)yy * w + xx;
                if (b->vis[i])
                    shade_one(fr, i, xx, yy);
            }
        }
}

static void frame_rows(void *ctx, int y0, int y1) {
    frame *fr = ctx;
    int k = y0 / fr->rows;
    double t0 = now_ms();
    for (int j = W.bin_start[k]; j < W.bin_start[k + 1]; j++)
        draw_tri(fr, W.bin_idx[j], y0, y1);
    double t1 = now_ms();
    shade_rows(fr, y0, y1);
    double t2 = now_ms();
    pthread_mutex_lock(&fr->lock);
    fr->rast_ms += t1 - t0, fr->shade_ms += t2 - t1;
    pthread_mutex_unlock(&fr->lock);
}

/* the inverse of a rigid view matrix */
static m4 inv_rigid(const m4 *v) {
    m4 r = m4id();
    for (int c = 0; c < 3; c++)
        for (int rr = 0; rr < 3; rr++)
            r.m[c * 4 + rr] = v->m[rr * 4 + c];
    v3 t = V3(v->m[12], v->m[13], v->m[14]);
    for (int rr = 0; rr < 3; rr++)
        r.m[12 + rr] = -(r.m[rr] * t.x + r.m[4 + rr] * t.y + r.m[8 + rr] * t.z);
    return r;
}

void fig_render(fbuf *b, const fcam *cam, const flight *L, const fmodel *m, const fpose *p) {
    double t0 = now_ms();
    int total = 0, tris = 0;
    for (int i = 0; i < m->nmeshes; i++)
        if (!m->mesh[i].hidden)
            total += m->mesh[i].nv, tris += m->mesh[i].nt;
    if (!grow((void **)&W.v, &W.capv, total, sizeof *W.v) || !grow((void **)&W.t, &W.capt, tris, sizeof *W.t) ||
        !grow((void **)&W.st, &W.capst, tris * 3, sizeof *W.st) || !grow_world(total))
        return;
    proj pr = {.W2 = b->w * 0.5f, .H2 = b->h * 0.5f, .ortho = cam->fovy <= 0};
    if (pr.ortho) {
        pr.fy = b->h / cam->ortho_h;
        pr.fx = pr.fy;
    } else {
        float f = 1.0f / tanf(cam->fovy * 0.5f);
        pr.fy = f * b->h * 0.5f;
        pr.fx = f * b->h * 0.5f;
    }
    W.nv = 0, W.nt = 0, W.nst = 0;
    int base_of[FIG_MAX_MESHES];
    for (int i = 0; i < m->nmeshes; i++) {
        const fmesh *me = &m->mesh[i];
        base_of[i] = -1;
        if (me->hidden)
            continue;
        base_of[i] = W.nv;
        skin_mesh(me, p, W.nv);
        W.nv += me->nv;
        if (L->shadows && !m->mat[me->mat].no_shadow) {
            for (int t = 0; t < me->nt * 3; t++)
                W.st[W.nst * 3 + t] = base_of[i] + me->tri[t];
            W.nst += me->nt;
        }
    }
    double t1 = now_ms();
    shadow_build(L, L->shadows ? L->shadow_size : 0, W.nst);
    double t2 = now_ms();
    int next_id = 1;
    for (int i = 0; i < m->nmeshes; i++) {
        const fmesh *me = &m->mesh[i];
        int parts = 1;
        if (me->tpart)
            for (int t = 0; t < me->nt; t++)
                if (me->tpart[t] + 1 > parts)
                    parts = me->tpart[t] + 1;
        int id0 = next_id;
        next_id += parts;
        if (base_of[i] < 0)
            continue;
        int base = base_of[i];
        project_mesh(me, cam, &pr, base);
        bool two = m->mat[me->mat].two_sided;
        for (int t = 0; t < me->nt; t++) {
            int a = base + me->tri[t * 3], bb = base + me->tri[t * 3 + 1], c = base + me->tri[t * 3 + 2];
            const pvert *A = &W.v[a], *B = &W.v[bb], *C = &W.v[c];
            if (A->d < cam->znear || B->d < cam->znear || C->d < cam->znear)
                continue;
            float area = (B->sx - A->sx) * (C->sy - A->sy) - (C->sx - A->sx) * (B->sy - A->sy);
            if (fabsf(area) < 1e-8f)
                continue;
            bool back = area > 0; /* y points down: front faces are clockwise on screen */
            if (back && !two)
                continue;
            float y0 = fminq(A->sy, fminq(B->sy, C->sy)), y1 = fmaxq(A->sy, fmaxq(B->sy, C->sy));
            float x0 = fminq(A->sx, fminq(B->sx, C->sx)), x1 = fmaxq(A->sx, fmaxq(B->sx, C->sx));
            if (y1 < 0 || y0 >= b->h || x1 < 0 || x0 >= b->w)
                continue;
            int id = id0 + (me->tpart ? me->tpart[t] : 0);
            W.t[W.nt++] = (ptri){a, bb, c, me->mat, (uint16_t)(id > 65535 ? 65535 : id), back, y0, y1,
                                 1.0f / area, 1.0f / A->d, 1.0f / B->d, 1.0f / C->d};
        }
    }
    /* sort the triangles into the tasks' rows */
    frame fr = {b, cam, L, m, vnorm(m4dir(&cam->view, vnorm(L->dir))), inv_rigid(&cam->view), pr, 16, 0, 0};
    int ntasks = (b->h + fr.rows - 1) / fr.rows;
    if (!grow((void **)&W.bin_start, &W.capbs, ntasks + 2, sizeof *W.bin_start))
        return;
    memset(W.bin_start, 0, sizeof *W.bin_start * (ntasks + 2));
    int nbin = 0;
    for (int i = 0; i < W.nt; i++) {
        int k0 = ifloor(W.t[i].y0) / fr.rows, k1 = iceil(W.t[i].y1) / fr.rows;
        k0 = k0 < 0 ? 0 : k0, k1 = k1 >= ntasks ? ntasks - 1 : k1;
        for (int k = k0; k <= k1; k++)
            W.bin_start[k + 2]++, nbin++;
    }
    for (int k = 2; k < ntasks + 2; k++)
        W.bin_start[k] += W.bin_start[k - 1];
    if (!grow((void **)&W.bin_idx, &W.capbi, nbin, sizeof *W.bin_idx))
        return;
    for (int i = 0; i < W.nt; i++) {
        int k0 = ifloor(W.t[i].y0) / fr.rows, k1 = iceil(W.t[i].y1) / fr.rows;
        k0 = k0 < 0 ? 0 : k0, k1 = k1 >= ntasks ? ntasks - 1 : k1;
        for (int k = k0; k <= k1; k++)
            W.bin_idx[W.bin_start[k + 1]++] = i;
    }
    double t3 = now_ms();
    pthread_mutex_init(&fr.lock, NULL);
    run_rows(frame_rows, &fr, b->h, fr.rows);
    pthread_mutex_destroy(&fr.lock);
    double t4 = now_ms();
    fig_stats.rast_cpu += fr.rast_ms;
    fig_stats.shade_cpu += fr.shade_ms;
    fig_stats.skin += t1 - t0;
    fig_stats.shadow += t2 - t1;
    fig_stats.project += t3 - t2;
    fig_stats.raster += t4 - t3;
    fig_stats.frames++;
}

/* ---- outlines and the way down to output pixels ---- */

typedef struct {
    const fbuf *b;
    const fcam *cam;
    const fmodel *m;
    uint32_t *out, *front;
    int ss, r, ri;
} resolve_ctx;

/* outlines into the samples of output rows y0..y1, then those rows down to output pixels */
static void resolve_rows(void *arg, int y0, int y1) {
    resolve_ctx *rc = arg;
    const fbuf *b = rc->b;
    const fmodel *m = rc->m;
    int ss = rc->ss, r = rc->r, ri = rc->ri, W2 = b->w, H2 = b->h;
    static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    int w = rc->cam->w, layers = rc->front ? 2 : 1;
    if (W2 < w * ss)
        w = W2 / ss;
    float inv = 1.0f / (ss * ss);
    float *sum = malloc(sizeof(float) * 4 * w * layers);
    if (!sum)
        return;
    for (int y = y0; y < y1; y++) {
        memset(sum, 0, sizeof(float) * 4 * w * layers);
        for (int sy = 0; sy < ss; sy++) {
            int iy = y * ss + sy;
            if (iy >= H2)
                break;
            for (int ix = 0; ix < w * ss; ix++) {
                size_t i = (size_t)iy * W2 + ix;
                int mi = b->mat[i];
                uint32_t c = mi ? b->col[i] : 0;
                if (mi && m->mat[mi - 1].line) {
                    /* a line where this surface ends: against nothing, against
                     * something well behind it, or where one part (a lock of
                     * hair, the jaw over the neck) lies over another */
                    const fmat *mt = &m->mat[mi - 1];
                    float z = b->z[i], eps = fmaxq(0.012f, z * 0.025f), eps_part = 0.0025f + z * 0.001f;
                    uint16_t id = b->id[i];
                    /* no lines between locks where they come out of the cap */
                    bool inner_ok = !mt->line_outer && (mt->kind != MAT_HAIR || b->aux[i] > 0.18f);
                    bool edge = false;
                    for (int k = 0; k < 4 && !edge; k++) {
                        int nx = ix + dx4[k] * r, ny = iy + dy4[k] * r;
                        if (nx < 0 || ny < 0 || nx >= W2 || ny >= H2) {
                            edge = true;
                            break;
                        }
                        size_t j = (size_t)ny * W2 + nx;
                        if (!b->mat[j] || (!mt->line_outer && b->z[j] > z + eps))
                            edge = true;
                        else if (inner_ok && b->id[j] != id) {
                            /* an inner line: checked at its own, finer width */
                            size_t q = (size_t)(iy + dy4[k] * ri) * W2 + (ix + dx4[k] * ri);
                            if (b->mat[q] && b->id[q] != id && b->z[q] > z + eps_part)
                                edge = true;
                        }
                    }
                    if (edge)
                        c = 0xff000000u | mt->line_col;
                }
                int layer = (rc->front && b->front[i]) ? 1 : 0;
                float *a = &sum[(layer * w + ix / ss) * 4];
                a[0] += (c >> 24) & 255, a[1] += (c >> 16) & 255, a[2] += (c >> 8) & 255, a[3] += c & 255;
            }
        }
        for (int l = 0; l < layers; l++) {
            uint32_t *o = &(l ? rc->front : rc->out)[(size_t)y * rc->cam->w];
            for (int x = 0; x < w; x++) {
                const float *a = &sum[(l * w + x) * 4];
                /* colours are opaque per sample, so averaging gives premultiplied values */
                o[x] = (uint32_t)(a[0] * inv + 0.5f) << 24 | (uint32_t)(a[1] * inv + 0.5f) << 16 |
                       (uint32_t)(a[2] * inv + 0.5f) << 8 | (uint32_t)(a[3] * inv + 0.5f);
            }
        }
    }
    free(sum);
}

void fig_resolve(const fbuf *b, const fcam *cam, const fmodel *m, uint32_t *out, uint32_t *front, float line_px) {
    double t0 = now_ms();
    int ss = cam->ss > 0 ? cam->ss : 1;
    int r = (int)(line_px * ss + 0.5f);
    if (r < 1)
        r = 1;
    int ri = (int)(line_px * ss * 0.7f + 0.5f); /* lines inside the figure are finer */
    if (ri < 1)
        ri = 1;
    resolve_ctx rc = {b, cam, m, out, front, ss, r, ri};
    run_rows(resolve_rows, &rc, cam->h, 8);
    fig_stats.resolve += now_ms() - t0;
}

/* Bakes how much of a key light from `dir` (and a few directions around
 * it, for soft edges) reaches each vertex, in the pose given: the shadow
 * under the jaw, between locks of hair, under the bangs, for free at draw
 * time. Faces are left clean. */
void fig_bake_sun(fmodel *m, const fpose *p, v3 dir, int samples) {
    int total = 0, tris = 0;
    for (int i = 0; i < m->nmeshes; i++)
        total += m->mesh[i].nv, tris += m->mesh[i].nt;
    if (!grow((void **)&W.st, &W.capst, tris * 3, sizeof *W.st) || !grow_world(total))
        return;
    float *acc = calloc((size_t)total, sizeof *acc);
    if (!acc)
        return;
    W.nv = 0, W.nst = 0;
    for (int i = 0; i < m->nmeshes; i++) {
        const fmesh *me = &m->mesh[i];
        skin_mesh(me, p, W.nv);
        if (!m->mat[me->mat].no_shadow) {
            for (int t = 0; t < me->nt * 3; t++)
                W.st[W.nst * 3 + t] = W.nv + me->tri[t];
            W.nst += me->nt;
        }
        W.nv += me->nv;
    }
    if (samples < 1)
        samples = 1;
    v3 d0 = vnorm(dir);
    v3 ax = vnorm(vcross(fabsf(d0.y) > 0.9f ? V3(1, 0, 0) : V3(0, 1, 0), d0)), ay = vcross(d0, ax);
    for (int k = 0; k < samples; k++) {
        float a = k * 2.39996f, r = samples > 1 ? 0.22f * sqrtf((k + 0.5f) / samples) : 0;
        flight L = {.dir = vnorm(vadd(d0, vadd(vmul(ax, cosf(a) * r), vmul(ay, sinf(a) * r)))), .shadows = true};
        shadow_build(&L, 768, W.nst);
        for (int i = 0; i < W.nv; i++)
            acc[i] += shadow_at(W.wp[i], vnorm(W.wn[i]));
    }
    int base = 0;
    for (int i = 0; i < m->nmeshes; i++) {
        fmesh *me = &m->mesh[i];
        if (m->mat[me->mat].kind != MAT_FACE) {
            if (!me->sun)
                me->sun = malloc(sizeof *me->sun * me->nv);
            if (me->sun)
                for (int v = 0; v < me->nv; v++)
                    me->sun[v] = acc[base + v] / samples;
        }
        base += me->nv;
    }
    S.on = false;
    free(acc);
}
