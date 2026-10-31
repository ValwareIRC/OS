/* The companion's emblem: how it shows itself wherever the OS draws it (the
 * <ai> element of a design, onboarding, the splash, the companion's window,
 * Settings, the captions). The person chooses it: a gem, an orb, an 8-ball,
 * the Earth, a ringed planet, a heart, the moon, or a face (presence.c), a
 * picture, a drawing the AI made, a 3D model. Every one shows what the
 * companion is doing: idle, thinking, listening, speaking, working, just
 * done, or something went wrong. Colours follow the design, but each look
 * stays itself: the Earth stays the Earth.
 *
 * Built-in looks are drawn here: the gem as a small 3D mesh (a brilliant cut,
 * side-on, back facets seen through the front, glints from a soft studio as
 * it turns), the round ones per pixel on a lit sphere. */
#include "emblem.h"
#include "app.h"
#include "voice.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define EMBLEM_FILE STATE_DIR "/emblem"
#define PI 3.14159265f

static const char *const names[EM_COUNT] = {"gem", "orb", "8-ball", "earth", "planet", "heart", "moon"};

static struct {
    bool loaded;
    int kind;
    int last_state;
    uint64_t done_at, changed_at;
} E = {.kind = EM_GEM, .last_state = ES_IDLE};

const char *emblem_name(int kind) { return kind >= 0 && kind < EM_COUNT ? names[kind] : "gem"; }

int emblem_from_name(const char *s) {
    for (int i = 0; i < EM_COUNT; i++)
        if (!strcasecmp(s, names[i]))
            return i;
    if (!strcasecmp(s, "8ball") || !strcasecmp(s, "eightball") || !strcasecmp(s, "magic 8-ball"))
        return EM_EIGHTBALL;
    if (!strcasecmp(s, "light") || !strcasecmp(s, "sphere"))
        return EM_ORB;
    if (!strcasecmp(s, "diamond") || !strcasecmp(s, "crystal") || !strcasecmp(s, "jewel"))
        return EM_GEM;
    if (!strcasecmp(s, "world") || !strcasecmp(s, "globe"))
        return EM_EARTH;
    return -1;
}

int emblem_kind(void) {
    if (!E.loaded) {
        E.loaded = true;
        FILE *f = fopen(EMBLEM_FILE, "r");
        char w[32] = "";
        if (f) {
            if (fscanf(f, "%31s", w) != 1)
                w[0] = 0;
            fclose(f);
        }
        int k = w[0] ? emblem_from_name(w) : -1;
        E.kind = k >= 0 ? k : EM_GEM;
    }
    return E.kind;
}

void emblem_set(int kind) {
    if (kind < 0 || kind >= EM_COUNT)
        return;
    E.loaded = true;
    E.kind = kind;
    E.changed_at = A.now;
    FILE *f = fopen(EMBLEM_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", names[kind]);
        fclose(f);
    }
    LOG("emblem: %s\n", names[kind]);
}

/* ---- what it is doing ---- */

static float lip_level(void) {
    static const struct {
        uint32_t magic, next;
        struct {
            double t;
            float level, f1, f2;
        } e[512];
    } *lip;
    if (!lip) {
        int fd = open("/run/os-lip", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return 0.5f;
        void *m = mmap(NULL, sizeof *lip, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED)
            return 0.5f;
        lip = m;
    }
    if (lip->magic != 0x4c495031u)
        return 0.5f;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    for (uint32_t k = 1; k <= 64 && k <= lip->next; k++) {
        const typeof(lip->e[0]) *e = &lip->e[(lip->next - k) % 512];
        if (e->t <= now && now - e->t < 0.05)
            return e->level;
    }
    return 0;
}

bool emblem_busy_hint; /* set by others for a moment: working on something (actions, web, designing) */

int emblem_state(bool active, float *level) {
    *level = 0;
    int s = ES_IDLE;
    if (A.companion.error[0] || A.onboard.error[0])
        s = ES_ERROR;
    if (voice_listening())
        s = ES_LISTENING;
    else if (voice_speaking()) {
        s = ES_SPEAKING;
        *level = lip_level();
    } else if (A.companion.pending || A.onboard.pending || active)
        s = ES_THINKING;
    else if (emblem_busy_hint || A.design_job)
        s = ES_WORKING;
    /* a bright moment as thinking or working ends */
    if ((E.last_state == ES_THINKING || E.last_state == ES_WORKING) && s == ES_IDLE)
        E.done_at = A.now;
    E.last_state = s;
    if (s == ES_IDLE && E.done_at && A.now - E.done_at < 700)
        s = ES_DONE;
    return s;
}

/* ---- small vector maths ---- */

typedef struct {
    float x, y, z;
} v3;
static v3 V3(float x, float y, float z) { return (v3){x, y, z}; }
static v3 sub(v3 a, v3 b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static v3 norm(v3 a) {
    float l = sqrtf(dot(a, a));
    return l > 0 ? V3(a.x / l, a.y / l, a.z / l) : a;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static color rgbf(float r, float g, float b, float a) {
    return RGBA((int)clampf(r, 0, 255), (int)clampf(g, 0, 255), (int)clampf(b, 0, 255), (int)clampf(a * 255, 0, 255));
}

/* ---- the gem ---- */

#define GN 16
#define GH 8
#define GV (GN + GH + GH + 1)
static int gem_faces[1 + GH * 7][GH];
static int gem_nv[1 + GH * 7], ngem;
static v3 gem_p[GV];

static void gem_build(void) {
    if (ngem)
        return;
    for (int i = 0; i < GN; i++) {
        float a = (float)i / GN * 2 * PI;
        gem_p[i] = V3(cosf(a), 0, sinf(a)); /* girdle */
    }
    for (int i = 0; i < GH; i++) {
        float a = (float)i / GH * 2 * PI, b = (i + 0.5f) / GH * 2 * PI;
        gem_p[GN + i] = V3(0.53f * cosf(a), 0.40f, 0.53f * sinf(a));            /* table */
        gem_p[GN + GH + i] = V3(0.47f * cosf(b), -0.58f, 0.47f * sinf(b));      /* pavilion */
    }
    gem_p[GV - 1] = V3(0, -1.0f, 0); /* culet */
#define G_(i) (((i) + GN) % GN)
#define T_(i) (GN + ((i) + GH) % GH)
#define M_(i) (GN + GH + ((i) + GH) % GH)
#define ADD3(a, b, c) (gem_faces[ngem][0] = (a), gem_faces[ngem][1] = (b), gem_faces[ngem][2] = (c), gem_nv[ngem++] = 3)
    for (int i = 0; i < GH; i++)
        gem_faces[0][i] = T_(i);
    gem_nv[0] = GH;
    ngem = 1;
    for (int i = 0; i < GH; i++) {
        ADD3(T_(i), G_(2 * i + 1), T_(i + 1));
        ADD3(T_(i), G_(2 * i), G_(2 * i + 1));
        ADD3(T_(i + 1), G_(2 * i + 1), G_(2 * i + 2));
        ADD3(G_(2 * i), G_(2 * i + 1), M_(i));
        ADD3(G_(2 * i + 1), G_(2 * i + 2), M_(i));
        ADD3(G_(2 * i + 2), M_(i + 1), M_(i));
        ADD3(M_(i), M_(i + 1), GV - 1);
    }
}

struct facet {
    int f;
    v3 n, ctr;
    float depth;
};

static int by_depth(const void *a, const void *b) {
    float d = ((const struct facet *)a)->depth - ((const struct facet *)b)->depth;
    return d < 0 ? -1 : d > 0;
}

static color gem_colour(v3 n, v3 ctr, float t, bool back, const color *pal, float light_band, float spec_boost,
                        float *spec_out) {
    const v3 Lk = norm(V3(-0.5f, 0.8f, 0.6f)), Lf = norm(V3(0.8f, 0.2f, 0.55f)), Lr = norm(V3(0.1f, -0.4f, 1));
    float h = fmodf(atan2f(ctr.z, ctr.x) / (2 * PI) + 1.25f + ctr.y * 0.35f + t * 0.02f, 1.0f) * 5;
    int k = (int)h;
    float fr = h - (float)k;
    color A_ = pal[k % 5], B_ = pal[(k + 1) % 5];
    float c[3] = {C_R(A_) + (C_R(B_) - C_R(A_)) * fr, C_G(A_) + (C_G(B_) - C_G(A_)) * fr, C_B(A_) + (C_B(B_) - C_B(A_)) * fr};
    float diffuse = fmaxf(0, dot(n, Lk));
    v3 r = V3(2 * n.z * n.x, 2 * n.z * n.y, 2 * n.z * n.z - 1); /* the mirror direction, looking along -z */
    float spec = powf(fmaxf(0, dot(r, Lk)), 60) * 1.1f + powf(fmaxf(0, dot(r, Lf)), 36) * 0.55f +
                 powf(fmaxf(0, dot(r, Lr)), 90) * 0.7f;
    spec = fminf(1.2f, spec * spec_boost + light_band);
    float hue = fmodf(ctr.x * 3 + ctr.y * 2 + t * 0.4f, 1.0f);
    float fire[3] = {sinf(2 * PI * hue) * 0.5f + 0.5f, sinf(2 * PI * (hue + 0.33f)) * 0.5f + 0.5f,
                     sinf(2 * PI * (hue + 0.66f)) * 0.5f + 0.5f};
    for (int i = 0; i < 3; i++) {
        c[i] += (255 - c[i]) * (back ? 0.42f : 0.22f);
        c[i] = fminf(255, c[i] * (back ? 1.0f : 0.86f + 0.18f * diffuse));
        c[i] = fminf(255, c[i] + 255 * spec * (0.75f + 0.25f * fire[i]));
    }
    *spec_out = spec;
    return rgbf(c[0], c[1], c[2], back ? 1 : 0.40f + 0.45f * fminf(1, spec));
}

static void draw_gem(canvas *cv, float cx, float cy, float r, color a, color b, int state, float level, float t) {
    gem_build();
    /* its own pastel run, leaning towards the design's colours */
    color base[5] = {RGBA(150, 132, 255, 255), RGBA(255, 150, 196, 255), RGBA(255, 206, 130, 255),
                     RGBA(110, 222, 200, 255), RGBA(140, 190, 255, 255)}, pal[5];
    for (int i = 0; i < 5; i++)
        pal[i] = color_mix(base[i], i % 2 ? b : a, 0.30f);
    float spin = state == ES_THINKING ? 1.4f : state == ES_WORKING ? 0.9f : 0.35f;
    static float angle;
    static uint64_t last;
    float dt = last ? fminf(0.1f, (float)(A.now - last) / 1000.0f) : 0;
    last = A.now;
    angle += dt * spin;
    float ay = angle, ax = -0.06f, cyr = cosf(ay), syr = sinf(ay), cxr = cosf(ax), sxr = sinf(ax);
    v3 v[GV];
    for (int i = 0; i < GV; i++) {
        v3 p = gem_p[i];
        float x1 = p.x * cyr + p.z * syr, z1 = -p.x * syr + p.z * cyr;
        v[i] = V3(x1, p.y * cxr - z1 * sxr, p.y * sxr + z1 * cxr);
    }
    struct facet fs[1 + GH * 7];
    for (int f = 0; f < ngem; f++) {
        v3 p0 = v[gem_faces[f][0]], p1 = v[gem_faces[f][1]], p2 = v[gem_faces[f][2]];
        v3 n = norm(cross(sub(p1, p0), sub(p2, p0)));
        v3 c = V3(0, 0, 0);
        for (int k = 0; k < gem_nv[f]; k++)
            c.x += v[gem_faces[f][k]].x, c.y += v[gem_faces[f][k]].y, c.z += v[gem_faces[f][k]].z;
        c.x /= gem_nv[f], c.y /= gem_nv[f], c.z /= gem_nv[f];
        if (n.x * c.x + n.y * (c.y + 0.2f) + n.z * c.z < 0)
            n = V3(-n.x, -n.y, -n.z); /* outward */
        fs[f] = (struct facet){f, n, c, c.z};
    }
    qsort(fs, (size_t)ngem, sizeof fs[0], by_depth);
    float scale = r * 1.18f;
    /* light running up through it while it thinks */
    float band = state == ES_THINKING ? fmodf(t * 1.3f, 2.4f) - 1.2f : 9;
    float boost = state == ES_SPEAKING ? 1 + 2.5f * level : state == ES_DONE ? 3 : 1;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ngem; i++) {
            struct facet *o = &fs[i];
            bool front = o->n.z > 0;
            if (front != (pass == 1))
                continue;
            float xy[2 * GH];
            int nv = gem_nv[o->f];
            for (int k = 0; k < nv; k++) {
                v3 p = v[gem_faces[o->f][k]];
                xy[2 * k] = cx + p.x * scale;
                xy[2 * k + 1] = cy - p.y * scale + scale * 0.12f;
            }
            float lb = band < 5 ? 0.6f * expf(-(o->ctr.y - band) * (o->ctr.y - band) * 18) : 0;
            float spec;
            v3 n = front ? o->n : V3(-o->n.x, -o->n.y, -o->n.z);
            color col = gem_colour(n, o->ctr, t, !front, pal, lb, boost, &spec);
            fill_polygon(cv, xy, nv, col);
            if (front)
                for (int k = 0; k < nv; k++) {
                    int k2 = (k + 1) % nv;
                    draw_line(cv, xy[2 * k], xy[2 * k + 1], xy[2 * k2], xy[2 * k2 + 1], 0.8f,
                              RGBA(255, 255, 255, (int)(255 * clampf(0.22f + 0.5f * fminf(1, spec + o->n.y * 0.3f), 0, 1))));
                }
        }
}

/* ---- lit spheres: the Earth, the moon, a planet, an 8-ball ---- */

static float hash3(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

static float vnoise(float x, float y, float z) {
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy), fz = fz * fz * (3 - 2 * fz);
    float r = 0;
    for (int k = 0; k < 8; k++) {
        int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
        float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
        r += w * hash3(ix + dx, iy + dy, iz + dz);
    }
    return r;
}

static float fbm(v3 p, int oct) {
    float s = 0, amp = 0.5f, f = 1;
    for (int i = 0; i < oct; i++, amp *= 0.5f, f *= 2.03f)
        s += amp * vnoise(p.x * f, p.y * f, p.z * f);
    return s;
}

struct sphere {
    int kind;
    float cx, cy, r, turn, t;
    color a, b;
    int state;
    float level;
};

static color shade_sphere(void *ctx, int x, int y) {
    struct sphere *s = ctx;
    float nx = (x + 0.5f - s->cx) / s->r, ny = (s->cy - (y + 0.5f)) / s->r;
    float d2 = nx * nx + ny * ny;
    if (d2 >= 1)
        return 0;
    float nz = sqrtf(1 - d2);
    v3 n = V3(nx, ny, nz), L = norm(V3(-0.55f, 0.6f, 0.6f));
    float lit = fmaxf(0, dot(n, L)), ambient = 0.18f;
    /* where on the body this pixel is: turned about the vertical axis */
    float c = cosf(s->turn), sn = sinf(s->turn);
    v3 p = V3(nx * c + nz * sn, ny, -nx * sn + nz * c);
    float r, g, b;
    if (s->kind == EM_EARTH) {
        float land = fbm(V3(p.x * 1.4f + 3, p.y * 1.4f, p.z * 1.4f), 5);
        float lat = fabsf(p.y);
        if (land > 0.47f) { /* land: green, drier and browner towards the tropics and the peaks */
            float hi = clampf((land - 0.47f) * 6, 0, 1), dry = clampf(1 - lat * 1.7f, 0, 1);
            r = 64 + 120 * dry * hi + 40 * hi, g = 128 + 30 * dry - 20 * hi, b = 60 + 40 * dry * hi;
        } else { /* sea, paler near the coast */
            float deep = clampf((0.47f - land) * 5, 0, 1);
            r = 36 + 30 * (1 - deep), g = 110 + 60 * (1 - deep) - 30 * deep, b = 190 + 30 * (1 - deep) - 30 * deep;
        }
        float ice = clampf((lat - 0.80f) * 9 + (land - 0.5f) * 2, 0, 1); /* soft polar caps */
        r += (242 - r) * ice, g += (248 - g) * ice, b += (252 - b) * ice;
        float cloud = fbm(V3(p.x * 2.4f + s->t * 0.05f, p.y * 3.2f, p.z * 2.4f + 7), 4);
        float cl = clampf((cloud - 0.55f) * 4, 0, 0.85f);
        r += (255 - r) * cl, g += (255 - g) * cl, b += (255 - b) * cl;
    } else if (s->kind == EM_MOON) {
        /* dark seas (maria) at large scale, then many small craters: a dark floor, a bright rim */
        float maria = fbm(V3(p.x * 1.3f + 11, p.y * 1.3f, p.z * 1.3f), 3);
        float v = 200 - clampf((maria - 0.45f) * 260, 0, 70);
        float cr = vnoise(p.x * 9 + 5, p.y * 9, p.z * 9), cr2 = vnoise(p.x * 19, p.y * 19 + 3, p.z * 19);
        if (cr > 0.72f)
            v -= (cr - 0.72f) * 160;
        else if (cr > 0.66f)
            v += 18;
        if (cr2 > 0.78f)
            v -= (cr2 - 0.78f) * 140;
        v += (fbm(V3(p.x * 6, p.y * 6, p.z * 6), 3) - 0.5f) * 30;
        r = v, g = v, b = v * 1.03f;
    } else if (s->kind == EM_PLANET) {
        float band = sinf(ny * 9 + fbm(V3(p.x * 2, p.y * 2, p.z * 2), 3) * 3);
        color mix = color_mix(s->a, s->b, 0.5f + 0.5f * band);
        mix = color_mix(mix, RGBA(255, 236, 214, 255), 0.25f);
        r = C_R(mix), g = C_G(mix), b = C_B(mix);
    } else { /* the 8-ball: black glass */
        r = g = b = 22;
        ambient = 0.6f;
    }
    float k = ambient + (1 - ambient) * lit;
    r *= k, g *= k, b *= k;
    /* a glossy highlight, and (for the Earth) the air at its edge */
    v3 h = norm(V3(L.x, L.y, L.z + 1));
    float sp = powf(fmaxf(0, dot(n, h)), s->kind == EM_EIGHTBALL ? 70 : 30) * (s->kind == EM_EIGHTBALL ? 1.0f : 0.35f);
    r += 255 * sp, g += 255 * sp, b += 255 * sp;
    if (s->kind == EM_EARTH) {
        float rim = powf(1 - nz, 2.2f) * 0.9f;
        r += (120 - r) * rim * 0.6f, g += (190 - g) * rim * 0.6f, b += (255 - b) * rim;
    }
    return rgbf(r, g, b, 1);
}

static void draw_sphere(canvas *cv, int kind, float cx, float cy, float r, color a, color b, int state, float level,
                        float t) {
    static float turn[EM_COUNT];
    static uint64_t last[EM_COUNT];
    float dt = last[kind] ? fminf(0.1f, (float)(A.now - last[kind]) / 1000.0f) : 0;
    last[kind] = A.now;
    float spin = state == ES_THINKING ? 1.2f : state == ES_WORKING ? 0.7f : 0.18f;
    if (kind == EM_EIGHTBALL)
        spin = 0;
    turn[kind] += dt * spin;
    struct sphere s = {kind, cx, cy, r, turn[kind], t, a, b, state, level};
    /* a ringed planet: the far half of the ring first */
    float tilt = 0.28f, rx = r * 1.75f, ry = r * 0.42f;
    if (kind == EM_PLANET)
        for (int i = 0; i < 48; i++) {
            float a0 = PI + PI * i / 48, a1 = PI + PI * (i + 1) / 48;
            float x0 = rx * cosf(a0), y0 = ry * sinf(a0), x1 = rx * cosf(a1), y1 = ry * sinf(a1);
            draw_line(cv, cx + x0 * cosf(tilt) - y0 * sinf(tilt), cy + x0 * sinf(tilt) + y0 * cosf(tilt),
                      cx + x1 * cosf(tilt) - y1 * sinf(tilt), cy + x1 * sinf(tilt) + y1 * cosf(tilt), r * 0.12f,
                      color_alpha(color_mix(a, RGBA(255, 255, 255, 255), 0.45f), 0.55f));
        }
    int ir = (int)ceilf(r);
    fill_round_shaded(cv, (rect){(int)cx - ir, (int)cy - ir, ir * 2, ir * 2}, (float)ir, shade_sphere, &s);
    if (kind == EM_PLANET)
        for (int i = 0; i < 48; i++) {
            float a0 = PI * i / 48, a1 = PI * (i + 1) / 48;
            float x0 = rx * cosf(a0), y0 = ry * sinf(a0), x1 = rx * cosf(a1), y1 = ry * sinf(a1);
            draw_line(cv, cx + x0 * cosf(tilt) - y0 * sinf(tilt), cy + x0 * sinf(tilt) + y0 * cosf(tilt),
                      cx + x1 * cosf(tilt) - y1 * sinf(tilt), cy + x1 * sinf(tilt) + y1 * cosf(tilt), r * 0.12f,
                      color_alpha(color_mix(a, RGBA(255, 255, 255, 255), 0.45f), 0.85f));
        }
    if (kind == EM_EIGHTBALL) {
        /* its window: the 8, or (while it thinks) the blue answer die, rising */
        float wob = state == ES_THINKING ? sinf(t * 5) * r * 0.08f : sinf(t * 0.8f) * r * 0.03f;
        float wx = cx - r * 0.12f + wob, wy = cy - r * 0.18f, wr = r * 0.42f;
        if (state == ES_THINKING || state == ES_SPEAKING || state == ES_DONE) {
            fill_circle(cv, wx, wy, wr, RGBA(16, 22, 60, 255));
            float glow = state == ES_SPEAKING ? 0.6f + 0.4f * level : 0.7f + 0.3f * sinf(t * 4);
            float tri[6] = {wx, wy + wr * 0.62f, wx - wr * 0.62f, wy - wr * 0.42f, wx + wr * 0.62f, wy - wr * 0.42f};
            fill_polygon(cv, tri, 3, color_alpha(RGBA(70, 110, 255, 255), glow));
            glow_circle(cv, wx, wy, wr * 0.9f, color_alpha(RGBA(90, 140, 255, 255), 0.35f * glow));
        } else {
            fill_circle(cv, wx, wy, wr, RGBA(250, 250, 250, 255));
            font *f = font_get_styled("Outfit", wr * 1.15f, 700, false, 0);
            draw_text_centered(cv, f, (rect){(int)(wx - wr), (int)(wy - wr), (int)(wr * 2), (int)(wr * 2)}, "8",
                               RGBA(20, 20, 24, 255));
        }
    }
}

/* ---- a heart, beating with its voice ---- */

static void draw_heart(canvas *cv, float cx, float cy, float r, color a, color b, int state, float level, float t) {
    (void)b;
    float beat = state == ES_SPEAKING ? 0.06f + 0.10f * level
                 : state == ES_LISTENING ? 0.05f * (0.5f + 0.5f * sinf(t * 6))
                                         : 0.03f * powf(fmaxf(0, sinf(t * (state == ES_THINKING ? 5.0f : 2.2f))), 8);
    float s = r * (1.0f + beat) / 16.0f;
    float xy[2 * 64];
    for (int i = 0; i < 64; i++) {
        float u = 2 * PI * i / 64;
        float x = 16 * powf(sinf(u), 3), y = 13 * cosf(u) - 5 * cosf(2 * u) - 2 * cosf(3 * u) - cosf(4 * u);
        xy[2 * i] = cx + x * s, xy[2 * i + 1] = cy - y * s - r * 0.08f;
    }
    color body = color_mix(RGBA(255, 92, 140, 255), a, 0.25f);
    fill_polygon(cv, xy, 64, body);
    /* soft volume: a darker underside and a glossy top-left */
    glow_circle(cv, cx + r * 0.25f, cy + r * 0.35f, r * 0.75f, RGBA(160, 30, 80, 70));
    glow_circle(cv, cx - r * 0.38f, cy - r * 0.32f, r * 0.42f, RGBA(255, 255, 255, 150));
    glow_circle(cv, cx + r * 0.36f, cy - r * 0.38f, r * 0.22f, RGBA(255, 255, 255, 90));
}

/* ---- the whole emblem, in its state ---- */

void emblem_draw_kind(canvas *cv, int kind, float cx, float cy, float r, color a, color b, int state, float level) {
    float t = (float)(A.now % 1000000) / 1000.0f;
    bool still = A.prof.reduce_motion;
    if (still)
        t = 2.0f, level = 0;
    if (kind == EM_ORB) {
        ui_orb(cv, cx, cy, r, a, b, state == ES_THINKING || state == ES_LISTENING || state == ES_SPEAKING);
        return;
    }
    /* trouble: a small shake, and the colour drains a little */
    if (state == ES_ERROR && !still) {
        cx += sinf(t * 40) * r * 0.03f * fmaxf(0, 1 - fmodf(t, 3.0f));
        a = color_mix(a, RGBA(150, 150, 160, 255), 0.5f), b = color_mix(b, RGBA(150, 150, 160, 255), 0.5f);
    }
    /* listening: it leans in, breathing */
    if (state == ES_LISTENING && !still)
        r *= 1.0f + 0.035f * sinf(t * 6);
    /* its light: the design's colours behind it */
    float breath = 0.5f + 0.5f * sinf(t * (state == ES_IDLE ? 1.2f : 3.5f));
    glow_circle(cv, cx, cy, r * (2.4f + 0.3f * breath), color_alpha(a, 0.14f + 0.06f * breath));
    glow_circle(cv, cx + r * 0.3f, cy - r * 0.2f, r * 1.7f, color_alpha(b, 0.16f));
    if (state == ES_DONE) /* just finished: a flash */
        glow_circle(cv, cx, cy, r * 2.2f, RGBA(255, 255, 255, 120));
    switch (kind) {
    case EM_GEM: draw_gem(cv, cx, cy, r, a, b, state, level, t); break;
    case EM_HEART: draw_heart(cv, cx, cy, r, a, b, state, level, t); break;
    default: draw_sphere(cv, kind, cx, cy, r, a, b, state, level, t); break;
    }
    /* working on something: a light circling it */
    if (state == ES_WORKING && !still)
        for (int k = 0; k < 3; k++) {
            float ang = t * 2.2f + k * 0.35f;
            float ox = cx + cosf(ang) * r * 1.45f, oy = cy + sinf(ang) * r * 0.5f;
            glow_circle(cv, ox, oy, r * (0.22f - k * 0.05f), color_alpha(b, 0.8f - k * 0.25f));
        }
}

void emblem_draw(canvas *cv, float cx, float cy, float r, color a, color b, bool active) {
    float level;
    int state = emblem_state(active, &level);
    emblem_draw_kind(cv, emblem_kind(), cx, cy, r, a, b, state, level);
}
