#pragma once
/* Small vector, quaternion and matrix maths for the figure (the companion's
 * body). Y is up; the figure faces +Z, towards the camera; its left is +X
 * (VRM's conventions, so bones and clips line up with VRM models). Matrices
 * are column-major: m[col * 4 + row]. */
#include <math.h>
#include <stdbool.h>

#define PI_F 3.14159265358979f

typedef struct {
    float x, y, z;
} v3;

typedef struct {
    float x, y, z, w;
} quat;

typedef struct {
    float m[16];
} m4;

static inline v3 V3(float x, float y, float z) { return (v3){x, y, z}; }
static inline v3 vadd(v3 a, v3 b) { return (v3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline v3 vsub(v3 a, v3 b) { return (v3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline v3 vmul(v3 a, float s) { return (v3){a.x * s, a.y * s, a.z * s}; }
static inline v3 vmadd(v3 a, v3 b, float s) { return (v3){a.x + b.x * s, a.y + b.y * s, a.z + b.z * s}; }
static inline v3 vscale3(v3 a, v3 s) { return (v3){a.x * s.x, a.y * s.y, a.z * s.z}; }
static inline float vdot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 vcross(v3 a, v3 b) { return (v3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static inline float vlen(v3 a) { return sqrtf(vdot(a, a)); }
static inline v3 vnorm(v3 a) {
    float l = vlen(a);
    return l > 1e-12f ? vmul(a, 1.0f / l) : (v3){0, 1, 0};
}
static inline v3 vlerp(v3 a, v3 b, float t) { return vadd(a, vmul(vsub(b, a), t)); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothf(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}
/* Eases towards a goal at a rate that doesn't depend on the frame rate. */
static inline float easef(float cur, float goal, float rate, float dt) { return cur + (goal - cur) * (1 - expf(-rate * dt)); }

static inline quat qid(void) { return (quat){0, 0, 0, 1}; }
static inline quat qaxis(v3 axis, float angle) {
    v3 a = vnorm(axis);
    float s = sinf(angle * 0.5f);
    return (quat){a.x * s, a.y * s, a.z * s, cosf(angle * 0.5f)};
}
static inline quat qmul(quat a, quat b) {
    return (quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static inline quat qconj(quat q) { return (quat){-q.x, -q.y, -q.z, q.w}; }
static inline quat qnorm(quat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-12f ? (quat){q.x / l, q.y / l, q.z / l, q.w / l} : qid();
}
static inline v3 qrot(quat q, v3 v) {
    v3 u = {q.x, q.y, q.z};
    v3 t = vmul(vcross(u, v), 2);
    return vadd(vadd(v, vmul(t, q.w)), vcross(u, t));
}
/* Rotation about X, then Y, then Z (radians). */
static inline quat qeuler(float x, float y, float z) {
    return qmul(qaxis(V3(0, 0, 1), z), qmul(qaxis(V3(0, 1, 0), y), qaxis(V3(1, 0, 0), x)));
}
static inline quat qslerp(quat a, quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = (quat){-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f)
        return qnorm((quat){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    float th = acosf(d), s = sinf(th);
    float wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
    return (quat){a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
/* The rotation that turns direction a into direction b. */
static inline quat qbetween(v3 a, v3 b) {
    a = vnorm(a), b = vnorm(b);
    float d = vdot(a, b);
    if (d > 0.99999f)
        return qid();
    if (d < -0.99999f) {
        v3 ax = vcross(V3(1, 0, 0), a);
        if (vlen(ax) < 1e-4f)
            ax = vcross(V3(0, 1, 0), a);
        return qaxis(ax, PI_F);
    }
    v3 c = vcross(a, b);
    return qnorm((quat){c.x, c.y, c.z, 1 + d});
}

static inline m4 m4id(void) { return (m4){{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; }
static inline m4 m4mul(m4 a, m4 b) {
    m4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            r.m[c * 4 + rr] = a.m[0 * 4 + rr] * b.m[c * 4 + 0] + a.m[1 * 4 + rr] * b.m[c * 4 + 1] +
                              a.m[2 * 4 + rr] * b.m[c * 4 + 2] + a.m[3 * 4 + rr] * b.m[c * 4 + 3];
    return r;
}
/* Translation, rotation, uniform scale. */
static inline m4 m4trs(v3 t, quat q, float s) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    m4 r = {{(1 - 2 * (y * y + z * z)) * s, (2 * (x * y + z * w)) * s, (2 * (x * z - y * w)) * s, 0,
             (2 * (x * y - z * w)) * s, (1 - 2 * (x * x + z * z)) * s, (2 * (y * z + x * w)) * s, 0,
             (2 * (x * z + y * w)) * s, (2 * (y * z - x * w)) * s, (1 - 2 * (x * x + y * y)) * s, 0, t.x, t.y, t.z, 1}};
    return r;
}
static inline v3 m4point(const m4 *m, v3 p) {
    return (v3){m->m[0] * p.x + m->m[4] * p.y + m->m[8] * p.z + m->m[12],
                m->m[1] * p.x + m->m[5] * p.y + m->m[9] * p.z + m->m[13],
                m->m[2] * p.x + m->m[6] * p.y + m->m[10] * p.z + m->m[14]};
}
static inline v3 m4dir(const m4 *m, v3 p) {
    return (v3){m->m[0] * p.x + m->m[4] * p.y + m->m[8] * p.z, m->m[1] * p.x + m->m[5] * p.y + m->m[9] * p.z,
                m->m[2] * p.x + m->m[6] * p.y + m->m[10] * p.z};
}
/* Looking from eye towards target: a view matrix (camera looks down -Z). */
static inline m4 m4lookat(v3 eye, v3 target, v3 up) {
    v3 f = vnorm(vsub(target, eye)), s = vnorm(vcross(f, up)), u = vcross(s, f);
    m4 r = {{s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -vdot(s, eye), -vdot(u, eye), vdot(f, eye), 1}};
    return r;
}

/* Inline min, max, floor and ceil: the libm ones are function calls (for
 * NaN and rounding-mode rules the drawing doesn't need), on every row of
 * every triangle. */
static inline float fminq(float a, float b) { return a < b ? a : b; }
static inline float fmaxq(float a, float b) { return a > b ? a : b; }
static inline int ifloor(float x) {
    int i = (int)x;
    return i - (x < (float)i);
}
static inline int iceil(float x) {
    int i = (int)x;
    return i + (x > (float)i);
}
