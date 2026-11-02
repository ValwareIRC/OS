/* Drawing a model in software, physically based.
 *
 * Triangles are rasterised (several samples a pixel) into a visibility buffer: each sample keeps only which
 * triangle is nearest. Each pixel is then shaded once for every triangle its samples show (almost always one),
 * at the centre of those samples, and the results are weighed by how many samples each covered: smooth edges at
 * the cost of shading each pixel about once.
 *
 * Shading follows the glTF metal/roughness model: GGX microfacets with Smith visibility and Schlick's Fresnel
 * for the key light (with a shadow map), and image-based light from a studio environment (a gradient sky,
 * a floor, and soft boxes, so metal has something to reflect): mirror reflections are looked up directly,
 * rougher ones in maps blurred ahead for six roughnesses with the GGX lobe itself, diffuse light in an
 * irradiance map, and the split-sum approximation for how much of each comes back. Clearcoat adds a second,
 * glossy layer; transmission lets what is behind the model (the desktop) show through, as glass does.
 * Colours end with the Khronos PBR Neutral tone mapper (base colours stay what the artist chose) and sRGB. */
#include "m3math.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

void m3_view_default(m3_view *v) {
    memset(v, 0, sizeof *v);
    v->yaw = 25, v->pitch = 18;
    v->fov = 30;
    v->fit = true;
    v->zoom = 1;
    v->light_dir = vnorm(v3(-0.55f, 0.75f, 0.45f));
    v->light_color[0] = 1, v->light_color[1] = 0.97f, v->light_color[2] = 0.92f;
    v->light_intensity = 2.2f;
    v->env_intensity = 1;
    v->env_top[0] = 0.30f, v->env_top[1] = 0.31f, v->env_top[2] = 0.34f;
    v->env_horizon[0] = 0.10f, v->env_horizon[1] = 0.10f, v->env_horizon[2] = 0.11f;
    v->env_bottom[0] = 0.035f, v->env_bottom[1] = 0.032f, v->env_bottom[2] = 0.03f;
    v->shadows = true;
    v->exposure = 1;
    v->supersample = 2;
    v->threads = 4;
}

/* ---- colour ---- */

static float srgb_lut[256];
static uint8_t enc_lut[4097];
static pthread_once_t luts_once = PTHREAD_ONCE_INIT;

static void make_luts(void) {
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0f;
        srgb_lut[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    for (int i = 0; i <= 4096; i++) {
        float c = i / 4096.0f;
        float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f;
        enc_lut[i] = (uint8_t)clampf(s * 255 + 0.5f, 0, 255);
    }
}

static inline uint8_t encode(float c) { return enc_lut[(int)(clampf(c, 0, 1) * 4096 + 0.5f)]; }

/* Khronos PBR Neutral: highlights roll off without shifting the colours under them */
static inline m3v tonemap(m3v c) {
    const float start = 0.8f - 0.04f, desat = 0.15f;
    float x = fminf(c.x, fminf(c.y, c.z));
    float off = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    c = vsub(c, v3(off, off, off));
    float peak = fmaxf(c.x, fmaxf(c.y, c.z));
    if (peak < start)
        return c;
    float d = 1 - start, np = 1 - d * d / (peak + d - start);
    c = vmul(c, np / peak);
    float g = 1 - 1 / (desat * (peak - np) + 1);
    return vlerp(c, v3(np, np, np), g);
}

/* ---- textures ---- */

typedef struct {
    float r, g, b, a;
} rgba;

static inline rgba texel(const uint32_t *px, int w, int x, int y, bool srgb) {
    uint32_t v = px[y * w + x];
    rgba o;
    if (srgb)
        o.r = srgb_lut[v & 255], o.g = srgb_lut[(v >> 8) & 255], o.b = srgb_lut[(v >> 16) & 255];
    else
        o.r = (v & 255) / 255.0f, o.g = ((v >> 8) & 255) / 255.0f, o.b = ((v >> 16) & 255) / 255.0f;
    o.a = (v >> 24) / 255.0f;
    return o;
}

static rgba tex_sample(const m3_tex *t, float u, float v, float lod, bool srgb) {
    int l = (int)clampf(lod, 0, (float)(t->levels - 1));
    int w = t->w >> l, h = t->h >> l;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    const uint32_t *px = t->mip[l];
    float fx = (u - floorf(u)) * w - 0.5f, fy = (v - floorf(v)) * h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - x0, ay = fy - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = (x0 % w + w) % w, x1 = (x1 % w + w) % w, y0 = (y0 % h + h) % h, y1 = (y1 % h + h) % h;
    rgba a = texel(px, w, x0, y0, srgb), b = texel(px, w, x1, y0, srgb), c = texel(px, w, x0, y1, srgb),
         d = texel(px, w, x1, y1, srgb);
    rgba o;
    o.r = mixf(mixf(a.r, b.r, ax), mixf(c.r, d.r, ax), ay);
    o.g = mixf(mixf(a.g, b.g, ax), mixf(c.g, d.g, ax), ay);
    o.b = mixf(mixf(a.b, b.b, ax), mixf(c.b, d.b, ax), ay);
    o.a = mixf(mixf(a.a, b.a, ax), mixf(c.a, d.a, ax), ay);
    return o;
}

/* ---- the studio: environment light ---- */

typedef struct {
    m3v top, horizon, bottom;
    float rot;
    m3v key;     /* where the key light (and its soft box) is */
    float intensity;
} studio;

static inline float smooth(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}

/* a soft box: a rounded rectangle of light seen in direction c, half-sizes in radians, soft edges */
static inline float softbox(m3v d, m3v c, float hw, float hh, float soft) {
    m3v up = fabsf(c.y) > 0.95f ? v3(0, 0, 1) : v3(0, 1, 0);
    m3v x = vnorm(vcross(up, c)), y = vcross(c, x);
    float f = vdot(d, c);
    if (f <= 0)
        return 0;
    float ax = atan2f(vdot(d, x), f), ay = atan2f(vdot(d, y), f);
    return (1 - smooth(hw - soft, hw + soft, fabsf(ax))) * (1 - smooth(hh - soft, hh + soft, fabsf(ay)));
}

static m3v env_eval(const studio *s, m3v d) {
    if (s->rot != 0) {
        float c = cosf(s->rot), k = sinf(s->rot);
        d = v3(c * d.x + k * d.z, d.y, -k * d.x + c * d.z);
    }
    float y = d.y;
    m3v sky = y > 0 ? vlerp(s->horizon, s->top, smooth(0, 0.6f, y)) : vlerp(s->horizon, s->bottom, smooth(0, 0.25f, -y));
    /* the key soft box where the light comes from, a strip light each side behind, a broad one above */
    m3v key = s->key;
    float k = softbox(d, key, 0.42f, 0.30f, 0.10f) * 16.0f;
    m3v back = vnorm(v3(-key.x, 0.25f, -key.z));
    m3v side1 = vnorm(vadd(back, vmul(vnorm(vcross(v3(0, 1, 0), back)), 0.9f)));
    m3v side2 = vnorm(vsub(back, vmul(vnorm(vcross(v3(0, 1, 0), back)), 0.9f)));
    float r = softbox(d, side1, 0.07f, 0.55f, 0.03f) * 10.0f + softbox(d, side2, 0.07f, 0.55f, 0.03f) * 7.0f;
    float t = softbox(d, v3(0, 1, 0), 0.6f, 0.6f, 0.2f) * 2.5f;
    /* a low fill card in front, so faces towards the viewer never go black */
    float fill = softbox(d, vnorm(v3(key.x * -0.3f, 0.05f, 1)), 0.9f, 0.25f, 0.3f) * 1.2f;
    t += fill;
    m3v warm = v3(1.0f, 0.96f, 0.9f), cool = v3(0.92f, 0.96f, 1.0f);
    m3v c = vadd(sky, vadd(vmul(warm, k), vadd(vmul(cool, r), vmul(v3(1, 1, 1), t))));
    return vmul(c, s->intensity);
}

#define NLEVELS 6
static const float level_rough[NLEVELS] = {0.0f, 0.12f, 0.25f, 0.45f, 0.7f, 1.0f};

typedef struct {
    bool ready;
    studio s;
    int w[NLEVELS], h[NLEVELS];
    m3v *map[NLEVELS]; /* level 0 unused: mirror reflections are looked up directly */
    m3v *irr;          /* 32 x 16 */
} envmaps;

static envmaps env_cache;
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;

static inline void dir_to_uv(m3v d, float *u, float *v) {
    *u = atan2f(d.x, -d.z) / (2 * (float)M_PI) + 0.5f;
    *v = acosf(clampf(d.y, -1, 1)) / (float)M_PI;
}

static inline m3v uv_to_dir(float u, float v) {
    float ph = (u - 0.5f) * 2 * (float)M_PI, th = v * (float)M_PI;
    return v3(sinf(th) * sinf(ph), cosf(th), -sinf(th) * cosf(ph));
}

static m3v map_sample(const m3v *m, int w, int h, m3v d) {
    float u, v;
    dir_to_uv(d, &u, &v);
    float fx = u * w - 0.5f, fy = v * h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - x0, ay = fy - y0;
    int x1 = ((x0 + 1) % w + w) % w;
    x0 = (x0 % w + w) % w;
    int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 >= h ? h - 1 : y0 + 1 < 0 ? 0 : y0 + 1;
    m3v a = m[ya * w + x0], b = m[ya * w + x1], c = m[yb * w + x0], e = m[yb * w + x1];
    return vlerp(vlerp(a, b, ax), vlerp(c, e, ax), ay);
}

static inline float radical(unsigned i) {
    i = (i << 16) | (i >> 16);
    i = ((i & 0x55555555u) << 1) | ((i & 0xAAAAAAAAu) >> 1);
    i = ((i & 0x33333333u) << 2) | ((i & 0xCCCCCCCCu) >> 2);
    i = ((i & 0x0F0F0F0Fu) << 4) | ((i & 0xF0F0F0F0u) >> 4);
    i = ((i & 0x00FF00FFu) << 8) | ((i & 0xFF00FF00u) >> 8);
    return (float)i * 2.3283064365386963e-10f;
}

static void basis(m3v n, m3v *t, m3v *b) {
    m3v up = fabsf(n.y) < 0.999f ? v3(0, 1, 0) : v3(1, 0, 0);
    *t = vnorm(vcross(up, n));
    *b = vcross(n, *t);
}

/* each texel: the GGX lobe of the level's roughness around it, importance sampled (N = V = R) */
static void prefilter(const studio *s, m3v *out, int w, int h, float rough) {
    const int N = 160;
    float a = rough * rough;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            m3v n = uv_to_dir((x + 0.5f) / w, (y + 0.5f) / h), t, b;
            basis(n, &t, &b);
            m3v sum = v3(0, 0, 0);
            float wsum = 0;
            for (int i = 0; i < N; i++) {
                float e1 = (i + 0.5f) / N, e2 = radical((unsigned)i);
                float ph = 2 * (float)M_PI * e2;
                float ct = sqrtf((1 - e1) / (1 + (a * a - 1) * e1)), st = sqrtf(1 - ct * ct);
                m3v hv = vnorm(vadd(vadd(vmul(t, st * cosf(ph)), vmul(b, st * sinf(ph))), vmul(n, ct)));
                m3v l = vsub(vmul(hv, 2 * vdot(n, hv)), n);
                float nl = vdot(n, l);
                if (nl > 0) {
                    sum = vadd(sum, vmul(env_eval(s, l), nl));
                    wsum += nl;
                }
            }
            out[y * w + x] = wsum > 0 ? vmul(sum, 1 / wsum) : env_eval(s, n);
        }
}

static void irradiance(const studio *s, m3v *out, int w, int h) {
    const int N = 256;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            m3v n = uv_to_dir((x + 0.5f) / w, (y + 0.5f) / h), t, b;
            basis(n, &t, &b);
            m3v sum = v3(0, 0, 0);
            for (int i = 0; i < N; i++) { /* cosine weighted: the average is the irradiance over pi */
                float e1 = (i + 0.5f) / N, e2 = radical((unsigned)i);
                float r = sqrtf(e1), ph = 2 * (float)M_PI * e2;
                m3v l = vadd(vadd(vmul(t, r * cosf(ph)), vmul(b, r * sinf(ph))), vmul(n, sqrtf(1 - e1)));
                sum = vadd(sum, env_eval(s, l));
            }
            out[y * w + x] = vmul(sum, 1.0f / N);
        }
}

static const envmaps *env_for(const studio *s) {
    pthread_mutex_lock(&env_lock);
    if (!env_cache.ready || memcmp(&env_cache.s, s, sizeof *s)) {
        for (int l = 1; l < NLEVELS; l++) {
            env_cache.w[l] = l == 1 ? 128 : 64, env_cache.h[l] = env_cache.w[l] / 2;
            free(env_cache.map[l]);
            env_cache.map[l] = malloc(sizeof(m3v) * (size_t)env_cache.w[l] * env_cache.h[l]);
            prefilter(s, env_cache.map[l], env_cache.w[l], env_cache.h[l], level_rough[l]);
        }
        free(env_cache.irr);
        env_cache.irr = malloc(sizeof(m3v) * 32 * 16);
        irradiance(s, env_cache.irr, 32, 16);
        env_cache.s = *s;
        env_cache.ready = true;
    }
    pthread_mutex_unlock(&env_lock);
    return &env_cache;
}

static m3v env_specular(const envmaps *e, const studio *s, m3v r, float rough) {
    if (rough <= level_rough[1] * 0.5f)
        return env_eval(s, r);
    int l = 1;
    while (l < NLEVELS - 1 && level_rough[l + 1] < rough)
        l++;
    m3v a = map_sample(e->map[l], e->w[l], e->h[l], r);
    if (l == NLEVELS - 1)
        return a;
    m3v b = map_sample(e->map[l + 1], e->w[l + 1], e->h[l + 1], r);
    float t = clampf((rough - level_rough[l]) / (level_rough[l + 1] - level_rough[l]), 0, 1);
    return vlerp(a, b, t);
}

/* ---- the frame being drawn ---- */

typedef struct {
    float x[3], y[3], iw[3]; /* sample-space positions, 1/w */
    uint32_t v[3];           /* into the transformed vertices */
    int mat, node;
    float lod;               /* texture level for a texture 1 texel a unit: add log2(size) */
    bool blend;
} tri_t;

typedef struct {
    m3v *pos, *nrm;
    float *tan, *uv, *col; /* 4, 2, 4 */
    int n, cap;
} verts_t;

typedef struct {
    m3_model *m;
    const m3_view *v;
    int W, H, ss, SW, SH; /* output size, samples a side, sample buffer size */
    tri_t *tris;
    int ntris, captris;
    verts_t vt;
    uint32_t *vis;        /* triangle + 1 per sample */
    float *depth;         /* 1/w per sample (larger: nearer) */
    float *acc;           /* RGBA linear premultiplied per output pixel */
    uint16_t *pick;
    uint32_t *out;
    m3v eye;
    float view[16], proj[16];
    studio st;
    const envmaps *env;
    m3v lightv;           /* towards the key light */
    m3v lightc;
    /* shadow map */
    bool shadow;
    int sm;
    float *smz;
    float lview[16], lproj[16];
    m3v lx, ly, lz, lc;
    float lscale, lzmin, lzmax;
    float *vz;            /* view depth per output pixel (0: nothing) */
    float *ao;            /* ambient occlusion per output pixel */
    float radius;         /* the model's, in world units */
    float *bloom;         /* RGB glow from what is brighter than white */
} frame;

static bool grow(void **p, int *cap, int need, size_t sz) {
    if (need <= *cap)
        return true;
    int nc = *cap ? *cap : 1024;
    while (nc < need)
        nc *= 2;
    void *q = realloc(*p, sz * (size_t)nc);
    if (!q)
        return false;
    *p = q, *cap = nc;
    return true;
}

static const m3_mat default_mat = {"", {0.8f, 0.8f, 0.8f, 1}, -1, 0, 0.5f, -1, -1, 1, -1, 1, {0, 0, 0}, -1,
                                   M3_OPAQUE, 0.5f, false, false, 0, 1.5f, 0, 0};

static inline const m3_mat *mat_of(const frame *f, int i) { return i >= 0 && i < f->m->nmats ? &f->m->mats[i] : &default_mat; }

/* ---- camera ---- */

static void look_at(float *m, m3v eye, m3v at, m3v up) {
    m3v z = vnorm(vsub(eye, at)), x = vnorm(vcross(up, z)), y = vcross(z, x);
    m[0] = x.x, m[4] = x.y, m[8] = x.z, m[12] = -vdot(x, eye);
    m[1] = y.x, m[5] = y.y, m[9] = y.z, m[13] = -vdot(y, eye);
    m[2] = z.x, m[6] = z.y, m[10] = z.z, m[14] = -vdot(z, eye);
    m[3] = 0, m[7] = 0, m[11] = 0, m[15] = 1;
}

static void perspective(float *m, float fovy, float aspect, float zn, float zf) {
    float f = 1 / tanf(fovy / 2);
    memset(m, 0, sizeof(float) * 16);
    m[0] = f / aspect, m[5] = f, m[10] = (zf + zn) / (zn - zf), m[11] = -1, m[14] = 2 * zf * zn / (zn - zf);
}

static void ortho(float *m, float hw, float hh, float zn, float zf) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = 1 / hw, m[5] = 1 / hh, m[10] = -2 / (zf - zn), m[14] = -(zf + zn) / (zf - zn), m[15] = 1;
}

/* inverse of a rigid (rotation + translation, maybe scaled) matrix */
static void invert_affine(float *out, const float *m) {
    float n[9];
    mat_normal(m, n); /* inverse transpose of the 3x3; its transpose is the inverse */
    /* column-major out[c * 4 + r] = inverse[r][c] = inverse-transpose[c][r] = n[c * 3 + r] */
    out[0] = n[0], out[1] = n[1], out[2] = n[2];
    out[4] = n[3], out[5] = n[4], out[6] = n[5];
    out[8] = n[6], out[9] = n[7], out[10] = n[8];
    out[3] = out[7] = out[11] = 0, out[15] = 1;
    m3v t = v3(m[12], m[13], m[14]);
    out[12] = -(out[0] * t.x + out[4] * t.y + out[8] * t.z);
    out[13] = -(out[1] * t.x + out[5] * t.y + out[9] * t.z);
    out[14] = -(out[2] * t.x + out[6] * t.y + out[10] * t.z);
}

/* a camera framed for one shape of picture, in another: all of what the artist framed stays in view */
static float framed_fov(float yfov, float framed, float aspect) {
    if (framed <= 0 || aspect >= framed)
        return yfov;
    return 2 * atanf(tanf(yfov / 2) * framed / aspect);
}

static void setup_camera(frame *f) {
    const m3_view *v = f->v;
    m3_model *m = f->m;
    float aspect = (float)f->W / f->H;
    m3v c = vmul(vadd(m->lo, m->hi), 0.5f);
    float radius = vlen(vsub(m->hi, m->lo)) * 0.5f;
    if (radius < 1e-6f)
        radius = 1;
    int camnode = -1;
    if (v->use_camera)
        for (int i = 0; i < m->nnodes && camnode < 0; i++)
            if (m->nodes[i].camera >= 0)
                camnode = i;
    if (camnode >= 0) {
        const m3_camera *cm = &m->cams[m->nodes[camnode].camera];
        float w[16];
        memcpy(w, m->nodes[camnode].world, sizeof w);
        /* no scale in a camera */
        for (int k = 0; k < 3; k++) {
            m3v col = vnorm(v3(w[k * 4], w[k * 4 + 1], w[k * 4 + 2]));
            w[k * 4] = col.x, w[k * 4 + 1] = col.y, w[k * 4 + 2] = col.z;
        }
        invert_affine(f->view, w);
        f->eye = v3(w[12], w[13], w[14]);
        if (cm->ortho)
            ortho(f->proj, cm->ymag * aspect, cm->ymag, cm->znear, cm->zfar);
        else
            perspective(f->proj, framed_fov(cm->yfov, cm->aspect, aspect), aspect, cm->znear, cm->zfar);
        return;
    }
    if (!v->fit)
        c = v->target;
    float fov = (v->fov > 1 ? v->fov : 30) * (float)M_PI / 180;
    float zoom = v->zoom > 0.01f ? v->zoom : 1;
    /* the model's sphere fills the narrower side of the picture */
    float half = fov / 2, halfx = atanf(tanf(half) * aspect), lim = fminf(half, halfx);
    float dist = v->distance > 0 ? v->distance : radius / sinf(lim) / zoom;
    float yaw = v->yaw * (float)M_PI / 180, pitch = clampf(v->pitch, -89, 89) * (float)M_PI / 180;
    m3v dir = v3(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch));
    f->eye = vadd(c, vmul(dir, dist));
    m3v up = v3(-sinf(v->roll * (float)M_PI / 180), cosf(v->roll * (float)M_PI / 180), 0);
    look_at(f->view, f->eye, c, fabsf(dir.y) > 0.99f ? v3(0, 0, -1) : up);
    float zn = fmaxf(dist - radius * 1.5f, dist * 0.01f), zf = dist + radius * 1.5f;
    if (v->ortho)
        ortho(f->proj, radius / zoom * aspect / fminf(aspect, 1), radius / zoom / fminf(aspect, 1), zn, zf);
    else
        perspective(f->proj, fov, aspect, zn, zf);
}

/* ---- vertices and triangles ---- */

static bool transform(frame *f) {
    m3_model *m = f->m;
    float vp[16];
    mat_mul(vp, f->proj, f->view);
    f->ntris = 0, f->vt.n = 0;
    for (int ni = 0; ni < m->nnodes; ni++) {
        const m3_node *nd = &m->nodes[ni];
        if (nd->mesh < 0)
            continue;
        const m3_mesh *me = &m->meshes[nd->mesh];
        float nm[9];
        mat_normal(nd->world, nm);
        float det = nd->world[0] * (nd->world[5] * nd->world[10] - nd->world[9] * nd->world[6]) -
                    nd->world[4] * (nd->world[1] * nd->world[10] - nd->world[9] * nd->world[2]) +
                    nd->world[8] * (nd->world[1] * nd->world[6] - nd->world[5] * nd->world[2]);
        for (int pi = 0; pi < me->nprims; pi++) {
            const m3_prim *p = &me->prims[pi];
            int base = f->vt.n, need = base + p->nverts;
            int c1 = f->vt.cap, c2 = f->vt.cap, c3 = f->vt.cap, c4 = f->vt.cap, c5 = f->vt.cap;
            if (!grow((void **)&f->vt.pos, &c1, need, sizeof(m3v)) || !grow((void **)&f->vt.nrm, &c2, need, sizeof(m3v)) ||
                !grow((void **)&f->vt.tan, &c3, need, sizeof(float) * 4) || !grow((void **)&f->vt.uv, &c4, need, sizeof(float) * 2) ||
                !grow((void **)&f->vt.col, &c5, need, sizeof(float) * 4))
                return false;
            f->vt.cap = c1;
            float *cx = malloc(sizeof(float) * 4 * (size_t)p->nverts); /* clip space */
            if (!cx)
                return false;
            for (int i = 0; i < p->nverts; i++) {
                m3v w = mat_point(nd->world, v3(p->pos[i * 3], p->pos[i * 3 + 1], p->pos[i * 3 + 2]));
                f->vt.pos[base + i] = w;
                f->vt.nrm[base + i] = p->nrm ? vnorm(mat3_apply(nm, v3(p->nrm[i * 3], p->nrm[i * 3 + 1], p->nrm[i * 3 + 2]))) : v3(0, 1, 0);
                if (p->tan) {
                    m3v t = vnorm(mat_dir(nd->world, v3(p->tan[i * 4], p->tan[i * 4 + 1], p->tan[i * 4 + 2])));
                    float *o = f->vt.tan + (size_t)(base + i) * 4;
                    o[0] = t.x, o[1] = t.y, o[2] = t.z, o[3] = p->tan[i * 4 + 3] * (det < 0 ? -1 : 1);
                } else {
                    memset(f->vt.tan + (size_t)(base + i) * 4, 0, sizeof(float) * 4);
                }
                float *uv = f->vt.uv + (size_t)(base + i) * 2;
                uv[0] = p->uv ? p->uv[i * 2] : 0, uv[1] = p->uv ? p->uv[i * 2 + 1] : 0;
                float *col = f->vt.col + (size_t)(base + i) * 4;
                if (p->col)
                    memcpy(col, p->col + i * 4, sizeof(float) * 4);
                else
                    col[0] = col[1] = col[2] = col[3] = 1;
                float *o = cx + i * 4;
                o[0] = vp[0] * w.x + vp[4] * w.y + vp[8] * w.z + vp[12];
                o[1] = vp[1] * w.x + vp[5] * w.y + vp[9] * w.z + vp[13];
                o[2] = vp[2] * w.x + vp[6] * w.y + vp[10] * w.z + vp[14];
                o[3] = vp[3] * w.x + vp[7] * w.y + vp[11] * w.z + vp[15];
            }
            f->vt.n = need;
            const m3_mat *mt = mat_of(f, p->mat);
            float texsize = 1;
            if (mt->base_tex >= 0)
                texsize = (float)(f->m->texs[mt->base_tex].w > f->m->texs[mt->base_tex].h ? f->m->texs[mt->base_tex].w : f->m->texs[mt->base_tex].h);
            for (int t = 0; t + 2 < p->nidx; t += 3) {
                uint32_t ia = p->idx[t], ib = p->idx[t + 1], ic = p->idx[t + 2];
                const float *A = cx + ia * 4, *B = cx + ib * 4, *C = cx + ic * 4;
                float nearw = 1e-5f;
                if (A[3] <= nearw || B[3] <= nearw || C[3] <= nearw)
                    continue; /* behind the camera: an interface model is in front of it */
                float ax = (A[0] / A[3] * 0.5f + 0.5f) * f->SW, ay = (0.5f - A[1] / A[3] * 0.5f) * f->SH;
                float bx = (B[0] / B[3] * 0.5f + 0.5f) * f->SW, by = (0.5f - B[1] / B[3] * 0.5f) * f->SH;
                float cxx = (C[0] / C[3] * 0.5f + 0.5f) * f->SW, cy = (0.5f - C[1] / C[3] * 0.5f) * f->SH;
                float area = (bx - ax) * (cy - ay) - (cxx - ax) * (by - ay);
                if (area == 0 || !isfinite(area))
                    continue;
                if (area > 0 && !mt->double_sided && mt->alpha_mode != M3_BLEND)
                    continue; /* facing away (counter-clockwise in front, y down: negative area) */
                if (fmaxf(ax, fmaxf(bx, cxx)) < 0 || fminf(ax, fminf(bx, cxx)) > f->SW || fmaxf(ay, fmaxf(by, cy)) < 0 ||
                    fminf(ay, fminf(by, cy)) > f->SH)
                    continue;
                if (!grow((void **)&f->tris, &f->captris, f->ntris + 1, sizeof(tri_t)))
                    break;
                tri_t *tr = &f->tris[f->ntris++];
                tr->x[0] = ax, tr->y[0] = ay, tr->x[1] = bx, tr->y[1] = by, tr->x[2] = cxx, tr->y[2] = cy;
                tr->iw[0] = 1 / A[3], tr->iw[1] = 1 / B[3], tr->iw[2] = 1 / C[3];
                tr->v[0] = base + ia, tr->v[1] = base + ib, tr->v[2] = base + ic;
                tr->mat = p->mat, tr->node = ni;
                tr->blend = mt->alpha_mode == M3_BLEND || mt->transmission > 0.01f;
                /* texture level: texels per output pixel across the triangle */
                const float *ua = f->vt.uv + (size_t)tr->v[0] * 2, *ub = f->vt.uv + (size_t)tr->v[1] * 2, *uc = f->vt.uv + (size_t)tr->v[2] * 2;
                float uvarea = fabsf((ub[0] - ua[0]) * (uc[1] - ua[1]) - (uc[0] - ua[0]) * (ub[1] - ua[1])) * texsize * texsize;
                float px = fabsf(area) / (float)(f->ss * f->ss);
                tr->lod = px > 1e-9f && uvarea > 0 ? 0.5f * log2f(uvarea / px) - log2f(texsize) : 0;
            }
            free(cx);
        }
    }
    return true;
}

/* ---- threads ---- */

typedef void (*rows_fn)(frame *f, int y0, int y1, void *arg);

typedef struct {
    frame *f;
    rows_fn fn;
    void *arg;
    int rows, step, *next;
    pthread_mutex_t *lock;
} job;

static void *worker(void *p) {
    job *j = p;
    for (;;) {
        pthread_mutex_lock(j->lock);
        int y = *j->next;
        *j->next += j->step;
        pthread_mutex_unlock(j->lock);
        if (y >= j->rows)
            break;
        j->fn(j->f, y, y + j->step < j->rows ? y + j->step : j->rows, j->arg);
    }
    return NULL;
}

static void run_rows(frame *f, int threads, int rows, int step, rows_fn fn, void *arg) {
    if (threads < 1)
        threads = 1;
    if (threads > 16)
        threads = 16;
    int next = 0;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    job j = {f, fn, arg, rows, step, &next, &lock};
    pthread_t t[16];
    int started = 0;
    for (int i = 1; i < threads; i++)
        if (pthread_create(&t[started], NULL, worker, &j) == 0)
            started++;
    worker(&j);
    for (int i = 0; i < started; i++)
        pthread_join(t[i], NULL);
}

/* ---- rasterising ---- */

static inline bool bary(const tri_t *t, float px, float py, float *b0, float *b1, float *b2) {
    float d = (t->y[1] - t->y[2]) * (t->x[0] - t->x[2]) + (t->x[2] - t->x[1]) * (t->y[0] - t->y[2]);
    if (d == 0)
        return false;
    *b0 = ((t->y[1] - t->y[2]) * (px - t->x[2]) + (t->x[2] - t->x[1]) * (py - t->y[2])) / d;
    *b1 = ((t->y[2] - t->y[0]) * (px - t->x[2]) + (t->x[0] - t->x[2]) * (py - t->y[2])) / d;
    *b2 = 1 - *b0 - *b1;
    return true;
}

/* perspective-correct weights of a point in a triangle */
static inline void persp(const tri_t *t, float b0, float b1, float b2, float *w) {
    float a = b0 * t->iw[0], b = b1 * t->iw[1], c = b2 * t->iw[2], s = a + b + c;
    s = s != 0 ? 1 / s : 0;
    w[0] = a * s, w[1] = b * s, w[2] = c * s;
}

static float alpha_at(const frame *f, const tri_t *t, const float *w) {
    const m3_mat *mt = mat_of(f, t->mat);
    float a = mt->base[3];
    const float *c0 = f->vt.col + (size_t)t->v[0] * 4, *c1 = f->vt.col + (size_t)t->v[1] * 4, *c2 = f->vt.col + (size_t)t->v[2] * 4;
    a *= c0[3] * w[0] + c1[3] * w[1] + c2[3] * w[2];
    if (mt->base_tex >= 0) {
        const float *u0 = f->vt.uv + (size_t)t->v[0] * 2, *u1 = f->vt.uv + (size_t)t->v[1] * 2, *u2 = f->vt.uv + (size_t)t->v[2] * 2;
        const m3_tex *tx = &f->m->texs[mt->base_tex];
        float lod = t->lod + log2f((float)(tx->w > tx->h ? tx->w : tx->h));
        a *= tex_sample(tx, u0[0] * w[0] + u1[0] * w[1] + u2[0] * w[2], u0[1] * w[0] + u1[1] * w[1] + u2[1] * w[2], lod, false).a;
    }
    return a;
}

static void raster_rows(frame *f, int y0, int y1, void *arg) {
    (void)arg;
    for (int ti = 0; ti < f->ntris; ti++) {
        const tri_t *t = &f->tris[ti];
        if (t->blend)
            continue;
        float minx = fminf(t->x[0], fminf(t->x[1], t->x[2])), maxx = fmaxf(t->x[0], fmaxf(t->x[1], t->x[2]));
        float miny = fminf(t->y[0], fminf(t->y[1], t->y[2])), maxy = fmaxf(t->y[0], fmaxf(t->y[1], t->y[2]));
        int ys = (int)fmaxf(ceilf(miny - 0.5f), (float)y0), ye = (int)fminf(floorf(maxy - 0.5f), (float)(y1 - 1));
        int xs = (int)fmaxf(ceilf(minx - 0.5f), 0), xe = (int)fminf(floorf(maxx - 0.5f), (float)(f->SW - 1));
        if (ys > ye || xs > xe)
            continue;
        bool mask = mat_of(f, t->mat)->alpha_mode == M3_MASK;
        float cut = mat_of(f, t->mat)->alpha_cutoff;
        /* edge functions, stepped across each row */
        float d = (t->y[1] - t->y[2]) * (t->x[0] - t->x[2]) + (t->x[2] - t->x[1]) * (t->y[0] - t->y[2]);
        float id = 1 / d;
        for (int y = ys; y <= ye; y++) {
            float py = y + 0.5f;
            for (int x = xs; x <= xe; x++) {
                float px = x + 0.5f;
                float b0 = ((t->y[1] - t->y[2]) * (px - t->x[2]) + (t->x[2] - t->x[1]) * (py - t->y[2])) * id;
                float b1 = ((t->y[2] - t->y[0]) * (px - t->x[2]) + (t->x[0] - t->x[2]) * (py - t->y[2])) * id;
                float b2 = 1 - b0 - b1;
                if (b0 < 0 || b1 < 0 || b2 < 0)
                    continue;
                float iw = b0 * t->iw[0] + b1 * t->iw[1] + b2 * t->iw[2];
                size_t i = (size_t)y * f->SW + x;
                if (iw <= f->depth[i])
                    continue;
                if (mask) {
                    float w[3];
                    persp(t, b0, b1, b2, w);
                    if (alpha_at(f, t, w) < cut)
                        continue;
                }
                f->depth[i] = iw;
                f->vis[i] = (uint32_t)ti + 1;
            }
        }
    }
}

/* ---- shadows: the key light's depth over the model ---- */

static void shadow_setup(frame *f) {
    m3_model *m = f->m;
    f->lc = vmul(vadd(m->lo, m->hi), 0.5f);
    float r = vlen(vsub(m->hi, m->lo)) * 0.5f;
    if (r < 1e-6f)
        r = 1;
    f->lz = f->lightv;
    m3v up = fabsf(f->lz.y) > 0.95f ? v3(0, 0, 1) : v3(0, 1, 0);
    f->lx = vnorm(vcross(up, f->lz)), f->ly = vcross(f->lz, f->lx);
    f->lscale = 1 / r;
    f->sm = 1024;
    f->smz = malloc(sizeof(float) * (size_t)f->sm * f->sm);
    if (!f->smz) {
        f->shadow = false;
        return;
    }
    for (int i = 0; i < f->sm * f->sm; i++)
        f->smz[i] = -1e30f;
}

static inline void to_light(const frame *f, m3v p, float *x, float *y, float *z) {
    m3v d = vsub(p, f->lc);
    *x = (vdot(d, f->lx) * f->lscale * 0.5f + 0.5f) * f->sm;
    *y = (0.5f - vdot(d, f->ly) * f->lscale * 0.5f) * f->sm;
    *z = vdot(d, f->lz); /* larger: nearer the light */
}

static void shadow_rows(frame *f, int y0, int y1, void *arg) {
    (void)arg;
    m3_model *m = f->m;
    for (int ni = 0; ni < m->nnodes; ni++) {
        const m3_node *nd = &m->nodes[ni];
        if (nd->mesh < 0)
            continue;
        const m3_mesh *me = &m->meshes[nd->mesh];
        for (int pi = 0; pi < me->nprims; pi++) {
            const m3_prim *p = &me->prims[pi];
            const m3_mat *mt = mat_of(f, p->mat);
            if (mt->alpha_mode == M3_BLEND && mt->base[3] < 0.5f)
                continue;
            for (int t = 0; t + 2 < p->nidx; t += 3) {
                float X[3], Y[3], Z[3];
                for (int k = 0; k < 3; k++) {
                    const float *q = p->pos + p->idx[t + k] * 3;
                    to_light(f, mat_point(nd->world, v3(q[0], q[1], q[2])), &X[k], &Y[k], &Z[k]);
                }
                float d = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2]);
                if (d == 0)
                    continue;
                float id = 1 / d;
                int ys = (int)fmaxf(ceilf(fminf(Y[0], fminf(Y[1], Y[2])) - 0.5f), (float)y0);
                int ye = (int)fminf(floorf(fmaxf(Y[0], fmaxf(Y[1], Y[2])) - 0.5f), (float)(y1 - 1));
                int xs = (int)fmaxf(ceilf(fminf(X[0], fminf(X[1], X[2])) - 0.5f), 0);
                int xe = (int)fminf(floorf(fmaxf(X[0], fmaxf(X[1], X[2])) - 0.5f), (float)(f->sm - 1));
                for (int y = ys; y <= ye; y++)
                    for (int x = xs; x <= xe; x++) {
                        float px = x + 0.5f, py = y + 0.5f;
                        float b0 = ((Y[1] - Y[2]) * (px - X[2]) + (X[2] - X[1]) * (py - Y[2])) * id;
                        float b1 = ((Y[2] - Y[0]) * (px - X[2]) + (X[0] - X[2]) * (py - Y[2])) * id;
                        float b2 = 1 - b0 - b1;
                        if (b0 < 0 || b1 < 0 || b2 < 0)
                            continue;
                        float z = b0 * Z[0] + b1 * Z[1] + b2 * Z[2];
                        float *s = &f->smz[y * f->sm + x];
                        if (z > *s)
                            *s = z;
                    }
            }
        }
    }
}

/* how much of the key light reaches p (normal n): 5x5 filtered, biased along the normal */
static float lit(const frame *f, m3v p, m3v n) {
    if (!f->shadow)
        return 1;
    float texel = 2 / (f->lscale * f->sm);
    float ndl = vdot(n, f->lightv);
    p = vadd(p, vmul(n, texel * (1.5f + 2.0f * (1 - clampf(ndl, 0, 1)))));
    float x, y, z;
    to_light(f, p, &x, &y, &z);
    float sum = 0;
    int cnt = 0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            int sx = (int)(x + i * 0.75f), sy = (int)(y + j * 0.75f);
            cnt++;
            if (sx < 0 || sy < 0 || sx >= f->sm || sy >= f->sm) {
                sum += 1;
                continue;
            }
            sum += f->smz[sy * f->sm + sx] <= z + texel * 0.5f ? 1 : 0;
        }
    return sum / cnt;
}

/* ---- shading ---- */

typedef struct {
    m3v color;
    float alpha;      /* coverage of the surface itself (1 opaque; less for glass and blended) */
} shaded;

static inline float d_ggx(float nh, float a) {
    float a2 = a * a, d = nh * nh * (a2 - 1) + 1;
    return a2 / ((float)M_PI * d * d + 1e-12f);
}

static inline float v_smith(float nv, float nl, float a) {
    float a2 = a * a;
    float gv = nl * sqrtf(nv * nv * (1 - a2) + a2), gl = nv * sqrtf(nl * nl * (1 - a2) + a2);
    return 0.5f / (gv + gl + 1e-12f);
}

/* the split-sum's scale and bias for F0 (Karis' fit) */
static inline void env_brdf(float nv, float rough, float *A, float *B) {
    float r0 = rough * -1 + 1, r1 = rough * -0.0275f + 0.0425f, r2 = rough * -0.572f + 1.04f, r3 = rough * 0.022f - 0.04f;
    float a004 = fminf(r0 * r0, exp2f(-9.28f * nv)) * r0 + r1;
    *A = a004 * -1.04f + r2, *B = a004 * 1.04f + r3;
}

static shaded shade(const frame *f, const tri_t *t, float px, float py) {
    float b0, b1, b2, w[3];
    shaded out = {v3(0, 0, 0), 1};
    if (!bary(t, px, py, &b0, &b1, &b2))
        return out;
    b0 = clampf(b0, 0, 1), b1 = clampf(b1, 0, 1), b2 = clampf(1 - b0 - b1, 0, 1);
    persp(t, b0, b1, b2, w);
    const m3_mat *mt = mat_of(f, t->mat);
    const uint32_t *vi = t->v;
    m3v P = vadd(vadd(vmul(f->vt.pos[vi[0]], w[0]), vmul(f->vt.pos[vi[1]], w[1])), vmul(f->vt.pos[vi[2]], w[2]));
    m3v N = vnorm(vadd(vadd(vmul(f->vt.nrm[vi[0]], w[0]), vmul(f->vt.nrm[vi[1]], w[1])), vmul(f->vt.nrm[vi[2]], w[2])));
    m3v V = vnorm(vsub(f->eye, P));
    /* the triangle's own facing: the back of a two-sided surface turns its normal round */
    m3v e1 = vsub(f->vt.pos[vi[1]], f->vt.pos[vi[0]]), e2 = vsub(f->vt.pos[vi[2]], f->vt.pos[vi[0]]);
    m3v G = vnorm(vcross(e1, e2));
    bool back = vdot(G, V) < 0;
    if (back && mt->double_sided)
        N = vmul(N, -1), G = vmul(G, -1);
    float uv[2] = {0, 0};
    for (int k = 0; k < 3; k++)
        uv[0] += f->vt.uv[(size_t)vi[k] * 2] * w[k], uv[1] += f->vt.uv[(size_t)vi[k] * 2 + 1] * w[k];
    float vc[4] = {0, 0, 0, 0};
    for (int k = 0; k < 3; k++)
        for (int c = 0; c < 4; c++)
            vc[c] += f->vt.col[(size_t)vi[k] * 4 + c] * w[k];
    m3v base = v3(mt->base[0] * vc[0], mt->base[1] * vc[1], mt->base[2] * vc[2]);
    float alpha = mt->base[3] * vc[3];
    const m3_tex *T = f->m->texs;
    #define LOD(i) (t->lod + log2f((float)(T[i].w > T[i].h ? T[i].w : T[i].h)))
    if (mt->base_tex >= 0) {
        rgba c = tex_sample(&T[mt->base_tex], uv[0], uv[1], LOD(mt->base_tex), true);
        base = vmulv(base, v3(c.r, c.g, c.b)), alpha *= c.a;
    }
    float metal = mt->metallic, rough = mt->roughness, ao = 1;
    if (mt->mr_tex >= 0) {
        rgba c = tex_sample(&T[mt->mr_tex], uv[0], uv[1], LOD(mt->mr_tex), false);
        rough *= c.g, metal *= c.b;
    }
    if (mt->occ_tex >= 0) {
        rgba c = tex_sample(&T[mt->occ_tex], uv[0], uv[1], LOD(mt->occ_tex), false);
        ao = 1 + mt->occ_strength * (c.r - 1);
    }
    if (mt->normal_tex >= 0) {
        const float *t0 = f->vt.tan + (size_t)vi[0] * 4, *t1 = f->vt.tan + (size_t)vi[1] * 4, *t2 = f->vt.tan + (size_t)vi[2] * 4;
        m3v Tg = v3(t0[0] * w[0] + t1[0] * w[1] + t2[0] * w[2], t0[1] * w[0] + t1[1] * w[1] + t2[1] * w[2],
                    t0[2] * w[0] + t1[2] * w[1] + t2[2] * w[2]);
        if (vlen(Tg) > 1e-6f) {
            Tg = vnorm(vsub(Tg, vmul(N, vdot(N, Tg))));
            m3v Bt = vmul(vcross(N, Tg), t0[3] < 0 ? -1.0f : 1.0f);
            if (back && mt->double_sided)
                Bt = vmul(Bt, -1);
            rgba c = tex_sample(&T[mt->normal_tex], uv[0], uv[1], LOD(mt->normal_tex), false);
            m3v tn = v3((c.r * 2 - 1) * mt->normal_scale, (c.g * 2 - 1) * mt->normal_scale, c.b * 2 - 1);
            N = vnorm(vadd(vadd(vmul(Tg, tn.x), vmul(Bt, tn.y)), vmul(N, tn.z)));
        }
    }
    m3v emit = v3(mt->emissive[0], mt->emissive[1], mt->emissive[2]);
    if (mt->emissive_tex >= 0) {
        rgba c = tex_sample(&T[mt->emissive_tex], uv[0], uv[1], LOD(mt->emissive_tex), true);
        emit = vmulv(emit, v3(c.r, c.g, c.b));
    }
    #undef LOD
    const m3_node *nd = &f->m->nodes[t->node];
    float glow = nd->part >= 0 ? f->m->nodes[nd->part].glow : 0; /* a hovered part: brighter in its own colour */
    if (mt->unlit) {
        out.color = vadd(base, emit), out.alpha = alpha;
        return out;
    }
    rough = clampf(rough, 0.03f, 1);
    metal = clampf(metal, 0, 1);
    float nv = fmaxf(vdot(N, V), 1e-4f);
    if (vdot(N, V) < 0) /* a normal map leaning away from the eye: bring it back to the edge */
        N = vnorm(vadd(N, vmul(V, -vdot(N, V) + 0.01f))), nv = 0.01f;
    float a = rough * rough;
    m3v F0 = vlerp(v3(0.04f, 0.04f, 0.04f), base, metal);
    m3v diffc = vmul(base, 1 - metal);
    /* the key light */
    m3v L = f->lightv, H = vnorm(vadd(L, V));
    float nl = vdot(N, L), sh = 1;
    m3v col = v3(0, 0, 0);
    if (nl > 0 && vdot(G, L) > -0.2f) {
        sh = lit(f, P, G);
        float nh = fmaxf(vdot(N, H), 0), vh = fmaxf(vdot(V, H), 0);
        float fw = powf(1 - vh, 5);
        m3v F = vadd(F0, vmul(vsub(v3(1, 1, 1), F0), fw));
        float spec = d_ggx(nh, a) * v_smith(nv, nl, a);
        m3v kd = vmulv(vsub(v3(1, 1, 1), F), diffc);
        m3v brdf = vadd(vmul(kd, 1 / (float)M_PI), vmul(F, spec));
        col = vadd(col, vmulv(brdf, vmul(f->lightc, nl * sh)));
    }
    /* the studio around it */
    float A, B;
    env_brdf(nv, rough, &A, &B);
    m3v R = vsub(vmul(N, 2 * vdot(N, V)), V);
    m3v pre = env_specular(f->env, &f->st, R, rough);
    m3v irr = map_sample(f->env->irr, 32, 16, N);
    if (f->ao) { /* the screen-space occlusion joins the material's own */
        int ax = (int)(px / f->ss), ay = (int)(py / f->ss);
        if (ax >= 0 && ay >= 0 && ax < f->W && ay < f->H)
            ao *= f->ao[ay * f->W + ax];
    }
    float spec_occ = clampf(powf(nv + ao, expf(-16 * a - 1)) - 1 + ao, 0, 1);
    /* the key light's shadow dims the soft light a little too: things in shadow read as in shadow */
    float amb_sh = 0.65f + 0.35f * sh;
    m3v specE = vmul(vmulv(pre, vadd(vmul(F0, A), v3(B, B, B))), spec_occ * amb_sh);
    m3v diffE = vmul(vmulv(irr, diffc), ao * amb_sh);
    col = vadd(col, vadd(specE, diffE));
    /* glass: what is behind shows through what isn't reflected */
    if (mt->transmission > 0.01f) {
        float fr = (F0.x + F0.y + F0.z) / 3;
        fr = fr + (1 - fr) * powf(1 - nv, 5);
        float tr = mt->transmission * (1 - fr) * (1 - metal);
        /* the tint of the glass over what is behind: kept as colour added with less coverage */
        col = vadd(vmul(col, 1 - tr * 0.85f), vmul(vmulv(base, irr), tr * 0.08f));
        alpha *= 1 - tr * 0.92f;
        alpha = fmaxf(alpha, clampf((specE.x + specE.y + specE.z) / 3, 0, 1));
    }
    /* clearcoat: a thin glossy layer over everything */
    if (mt->clearcoat > 0.001f) {
        float cr = clampf(mt->clearcoat_roughness, 0.03f, 1), cA, cB;
        env_brdf(nv, cr, &cA, &cB);
        float fc = 0.04f + 0.96f * powf(1 - nv, 5);
        m3v cc = vmul(env_specular(f->env, &f->st, R, cr), (0.04f * cA + cB) * amb_sh);
        if (nl > 0) {
            float ca = cr * cr, nh = fmaxf(vdot(N, H), 0);
            cc = vadd(cc, vmul(f->lightc, d_ggx(nh, ca) * v_smith(nv, nl, ca) * 0.04f * nl * sh));
        }
        col = vadd(vmul(col, 1 - mt->clearcoat * fc), vmul(cc, mt->clearcoat));
    }
    if (glow > 0)
        col = vadd(vmul(col, 1 + 0.35f * glow), vmul(base, 0.12f * glow));
    col = vadd(col, emit);
    out.color = col;
    out.alpha = mt->alpha_mode == M3_BLEND ? alpha : mt->transmission > 0.01f ? alpha : 1;
    return out;
}

/* ---- ambient occlusion ---- */

static void depth_rows(frame *f, int y0, int y1, void *arg) {
    (void)arg;
    int ss = f->ss;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < f->W; x++) {
            float best = 0;
            for (int j = 0; j < ss; j++)
                for (int i = 0; i < ss; i++) {
                    size_t k = (size_t)(y * ss + j) * f->SW + x * ss + i;
                    if (f->vis[k] && f->depth[k] > best)
                        best = f->depth[k];
                }
            f->vz[y * f->W + x] = best > 0 ? 1 / best : 0;
        }
}

static void ao_rows(frame *f, int y0, int y1, void *arg) {
    (void)arg;
    /* the sampling disc: about 4% of the model's size on screen, 16 directions, two distances */
    float persp_px = f->proj[5] * f->H * 0.5f; /* pixels a world unit at depth 1 */
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < f->W; x++) {
            float z = f->vz[y * f->W + x];
            if (z <= 0) {
                f->ao[y * f->W + x] = 1;
                continue;
            }
            float rw = f->radius * 0.09f;              /* world radius of the neighbourhood */
            float rp = rw * persp_px / z;              /* in pixels */
            if (f->v->ortho)
                rp = rw * persp_px;
            if (rp < 1.5f) {
                f->ao[y * f->W + x] = 1;
                continue;
            }
            if (rp > 60)
                rp = 60;
            float occ = 0;
            int n = 0;
            float rot = (float)((x * 7 + y * 13) % 16) * 0.3927f; /* rotate the pattern pixel to pixel */
            for (int k = 0; k < 16; k++) {
                float ang = rot + k * 0.3927f * 2.4f, dist = rp * (0.35f + 0.65f * ((k * 5) % 16) / 15.0f);
                int sx = x + (int)(cosf(ang) * dist), sy = y + (int)(sinf(ang) * dist);
                if (sx < 0 || sy < 0 || sx >= f->W || sy >= f->H)
                    continue;
                float s = f->vz[sy * f->W + sx];
                n++;
                if (s <= 0)
                    continue;
                float dz = z - s; /* the neighbour nearer the eye: it may shade this one */
                if (dz > rw * 0.04f && dz < rw * 2.5f)
                    occ += 1 - smooth(rw * 0.8f, rw * 2.5f, dz);
            }
            float a = n ? 1 - 0.75f * occ / n : 1;
            f->ao[y * f->W + x] = clampf(a, 0.25f, 1);
        }
}

/* a light blur so the pattern doesn't show */
static void ao_blur(frame *f) {
    float *tmp = malloc(sizeof(float) * (size_t)f->W * f->H);
    if (!tmp)
        return;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < f->H; y++)
            for (int x = 0; x < f->W; x++) {
                float sum = 0, wsum = 0, z = f->vz[y * f->W + x];
                for (int k = -3; k <= 3; k++) {
                    int sx = pass ? x : x + k, sy = pass ? y + k : y;
                    if (sx < 0 || sy < 0 || sx >= f->W || sy >= f->H)
                        continue;
                    float sz = f->vz[sy * f->W + sx];
                    if (z > 0 && fabsf(sz - z) > f->radius * 0.1f) /* not across an edge */
                        continue;
                    float w = 1 - fabsf((float)k) / 4;
                    sum += (pass ? f->ao : f->ao)[sy * f->W + sx] * w, wsum += w;
                }
                tmp[y * f->W + x] = wsum > 0 ? sum / wsum : f->ao[y * f->W + x];
            }
        memcpy(f->ao, tmp, sizeof(float) * (size_t)f->W * f->H);
    }
    free(tmp);
}

/* ---- resolving: each pixel's samples, shaded once a triangle ---- */

static void resolve_rows(frame *f, int y0, int y1, void *arg) {
    (void)arg;
    int ss = f->ss, ns = ss * ss;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < f->W; x++) {
            uint32_t ids[9];
            int cnt[9], nu = 0;
            float sx[9], sy[9];
            for (int j = 0; j < ss; j++)
                for (int i = 0; i < ss; i++) {
                    uint32_t id = f->vis[(size_t)(y * ss + j) * f->SW + x * ss + i];
                    if (!id)
                        continue;
                    int k = 0;
                    while (k < nu && ids[k] != id)
                        k++;
                    if (k == nu)
                        ids[nu] = id, cnt[nu] = 0, sx[nu] = sy[nu] = 0, nu++;
                    cnt[k]++, sx[k] += x * ss + i + 0.5f, sy[k] += y * ss + j + 0.5f;
                }
            float *a = f->acc + ((size_t)y * f->W + x) * 4;
            a[0] = a[1] = a[2] = a[3] = 0;
            int best = -1, bc = 0;
            for (int k = 0; k < nu; k++) {
                const tri_t *t = &f->tris[ids[k] - 1];
                shaded s = shade(f, t, sx[k] / cnt[k], sy[k] / cnt[k]);
                float wgt = (float)cnt[k] / ns * s.alpha;
                a[0] += s.color.x * wgt, a[1] += s.color.y * wgt, a[2] += s.color.z * wgt, a[3] += wgt;
                if (cnt[k] > bc)
                    bc = cnt[k], best = t->node;
            }
            if (f->pick)
                f->pick[y * f->W + x] = best >= 0 && bc * 2 >= ns ? (uint16_t)(best + 1) : 0;
        }
}

/* see-through surfaces, back to front over what is already there (one sample a pixel) */
static frame *sort_frame;
static int cmp_far(const void *a, const void *b) {
    const tri_t *t = &sort_frame->tris[*(const int *)a], *u = &sort_frame->tris[*(const int *)b];
    float da = t->iw[0] + t->iw[1] + t->iw[2], db = u->iw[0] + u->iw[1] + u->iw[2];
    return da < db ? -1 : da > db;
}

static void blend_pass(frame *f) {
    int n = 0;
    for (int i = 0; i < f->ntris; i++)
        n += f->tris[i].blend;
    if (!n)
        return;
    int *order = malloc(sizeof(int) * (size_t)n), k = 0;
    if (!order)
        return;
    for (int i = 0; i < f->ntris; i++)
        if (f->tris[i].blend)
            order[k++] = i;
    static pthread_mutex_t sort_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&sort_lock);
    sort_frame = f;
    qsort(order, (size_t)n, sizeof(int), cmp_far);
    pthread_mutex_unlock(&sort_lock);
    int ss = f->ss;
    for (int o = 0; o < n; o++) {
        const tri_t *t = &f->tris[order[o]];
        const m3_mat *mt = mat_of(f, t->mat);
        float minx = fminf(t->x[0], fminf(t->x[1], t->x[2])) / ss, maxx = fmaxf(t->x[0], fmaxf(t->x[1], t->x[2])) / ss;
        float miny = fminf(t->y[0], fminf(t->y[1], t->y[2])) / ss, maxy = fmaxf(t->y[0], fmaxf(t->y[1], t->y[2])) / ss;
        int xs = (int)fmaxf(floorf(minx), 0), xe = (int)fminf(ceilf(maxx), (float)(f->W - 1));
        int ys = (int)fmaxf(floorf(miny), 0), ye = (int)fminf(ceilf(maxy), (float)(f->H - 1));
        for (int y = ys; y <= ye; y++)
            for (int x = xs; x <= xe; x++) {
                float px = (x + 0.5f) * ss, py = (y + 0.5f) * ss, b0, b1, b2;
                if (!bary(t, px, py, &b0, &b1, &b2) || b0 < 0 || b1 < 0 || b2 < 0)
                    continue;
                float iw = b0 * t->iw[0] + b1 * t->iw[1] + b2 * t->iw[2];
                /* behind the opaque surface at the pixel's middle sample: hidden */
                size_t si = (size_t)(y * ss + ss / 2) * f->SW + x * ss + ss / 2;
                if (f->vis[si] && iw < f->depth[si])
                    continue;
                if (!mt->double_sided && mt->transmission < 0.01f) {
                    float area = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->x[2] - t->x[0]) * (t->y[1] - t->y[0]);
                    if (area > 0)
                        continue;
                }
                shaded s = shade(f, t, px, py);
                float *a = f->acc + ((size_t)y * f->W + x) * 4, al = clampf(s.alpha, 0, 1);
                a[0] = s.color.x * al + a[0] * (1 - al), a[1] = s.color.y * al + a[1] * (1 - al);
                a[2] = s.color.z * al + a[2] * (1 - al), a[3] = al + a[3] * (1 - al);
                if (f->pick && al > 0.3f)
                    f->pick[y * f->W + x] = (uint16_t)(t->node + 1);
            }
    }
    free(order);
}

static void make_bloom(frame *f) {
    int W = f->W, H = f->H;
    float ex = f->v->exposure > 0 ? f->v->exposure : 1;
    float *b = calloc((size_t)W * H * 3, sizeof(float)), *t = malloc(sizeof(float) * (size_t)W * H * 3);
    if (!b || !t) {
        free(b), free(t);
        return;
    }
    bool any = false;
    for (int i = 0; i < W * H; i++) {
        const float *a = f->acc + (size_t)i * 4;
        for (int c = 0; c < 3; c++) {
            float v = (a[c] * ex - 1.6f) * 0.8f; /* well past white: emission, not every highlight */
            if (v > 0)
                b[i * 3 + c] = v, any = true;
        }
    }
    if (!any) {
        free(b), free(t);
        return;
    }
    /* three widening box blurs each way: close to a gaussian, about 2% of the picture across */
    int r = (W < H ? W : H) / 70 + 1;
    for (int it = 0; it < 3; it++)
        for (int pass = 0; pass < 2; pass++) {
            float *src = pass ? t : b, *dst = pass ? b : t;
            int n = pass ? H : W, m = pass ? W : H;
            for (int o = 0; o < m; o++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    int cnt = 0;
                    for (int k = -r; k <= r && k < n; k++)
                        if (k >= 0)
                            sum += src[(pass ? k * W + o : o * W + k) * 3 + c], cnt++;
                    for (int i = 0; i < n; i++) {
                        dst[(pass ? i * W + o : o * W + i) * 3 + c] = sum / (2 * r + 1);
                        int add = i + r + 1, rem = i - r;
                        if (add < n)
                            sum += src[(pass ? add * W + o : o * W + add) * 3 + c];
                        if (rem >= 0)
                            sum -= src[(pass ? rem * W + o : o * W + rem) * 3 + c];
                    }
                    (void)cnt;
                }
        }
    free(t);
    f->bloom = b;
}

static void encode_rows(frame *f, int y0, int y1, void *arg) {
    (void)arg;
    float ex = f->v->exposure > 0 ? f->v->exposure : 1;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < f->W; x++) {
            float *a = f->acc + ((size_t)y * f->W + x) * 4;
            if (f->bloom) { /* glow, also past the model's edge (it covers what is behind it a little) */
                const float *g = f->bloom + ((size_t)y * f->W + x) * 3;
                float gl = (g[0] + g[1] + g[2]) / 3 * 0.6f;
                a[0] += g[0] * 0.6f / ex, a[1] += g[1] * 0.6f / ex, a[2] += g[2] * 0.6f / ex;
                a[3] = fminf(1, a[3] + gl * (1 - a[3]));
            }
            float al = clampf(a[3], 0, 1);
            uint32_t o = 0;
            if (al > 0.002f) {
                m3v c = tonemap(vmul(v3(a[0] / al, a[1] / al, a[2] / al), ex)); /* tone mapped unpremultiplied */
                int A = (int)(al * 255 + 0.5f);
                o = (uint32_t)A << 24 | (uint32_t)(encode(c.x) * A / 255) << 16 | (uint32_t)(encode(c.y) * A / 255) << 8 |
                    (uint32_t)(encode(c.z) * A / 255);
            }
            f->out[(size_t)y * f->W + x] = o;
        }
}

double m3_render(m3_model *m, const m3_view *v, uint32_t *out, uint16_t *pick, int w, int h) {
    double t0 = now_ms();
    pthread_once(&luts_once, make_luts);
    if (w <= 0 || h <= 0 || !m)
        return 0;
    frame f;
    memset(&f, 0, sizeof f);
    f.m = m, f.v = v, f.W = w, f.H = h, f.out = out, f.pick = pick;
    f.ss = v->supersample < 1 ? 1 : v->supersample > 3 ? 3 : v->supersample;
    f.SW = w * f.ss, f.SH = h * f.ss;
    setup_camera(&f);
    f.lightv = vnorm(v->light_dir);
    f.lightc = v3(v->light_color[0] * v->light_intensity, v->light_color[1] * v->light_intensity,
                  v->light_color[2] * v->light_intensity);
    f.st.top = v3(v->env_top[0], v->env_top[1], v->env_top[2]);
    f.st.horizon = v3(v->env_horizon[0], v->env_horizon[1], v->env_horizon[2]);
    f.st.bottom = v3(v->env_bottom[0], v->env_bottom[1], v->env_bottom[2]);
    f.st.rot = v->env_rotation * (float)M_PI / 180;
    f.st.key = f.lightv;
    f.st.intensity = v->env_intensity > 0 ? v->env_intensity : 1;
    f.env = env_for(&f.st);
    size_t ns = (size_t)f.SW * f.SH;
    f.vis = calloc(ns, sizeof *f.vis);
    f.depth = malloc(ns * sizeof *f.depth);
    f.acc = malloc(sizeof(float) * 4 * (size_t)w * h);
    if (!f.vis || !f.depth || !f.acc || !transform(&f)) {
        memset(out, 0, sizeof *out * (size_t)w * h);
        goto done;
    }
    for (size_t i = 0; i < ns; i++)
        f.depth[i] = 0;
    f.shadow = v->shadows;
    if (f.shadow) {
        shadow_setup(&f);
        if (f.shadow)
            run_rows(&f, v->threads, f.sm, 32, shadow_rows, NULL);
    }
    run_rows(&f, v->threads, f.SH, 16, raster_rows, NULL);
    f.radius = vlen(vsub(m->hi, m->lo)) * 0.5f;
    f.vz = malloc(sizeof(float) * (size_t)w * h);
    f.ao = malloc(sizeof(float) * (size_t)w * h);
    if (f.vz && f.ao) {
        run_rows(&f, v->threads, f.H, 16, depth_rows, NULL);
        run_rows(&f, v->threads, f.H, 8, ao_rows, NULL);
        ao_blur(&f);
    } else {
        free(f.ao), f.ao = NULL;
    }
    run_rows(&f, v->threads, f.H, 8, resolve_rows, NULL);
    blend_pass(&f);
    make_bloom(&f);
    run_rows(&f, v->threads, f.H, 16, encode_rows, NULL);
done:
    free(f.vis), free(f.depth), free(f.acc), free(f.tris), free(f.smz), free(f.vz), free(f.ao), free(f.bloom);
    free(f.vt.pos), free(f.vt.nrm), free(f.vt.tan), free(f.vt.uv), free(f.vt.col);
    return now_ms() - t0;
}
