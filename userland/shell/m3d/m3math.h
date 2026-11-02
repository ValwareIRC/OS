#pragma once
/* Small vector, quaternion and matrix helpers for m3d (column-major 4x4, as glTF stores them). */
#include <math.h>
#include "m3d.h"

static inline m3v v3(float x, float y, float z) { return (m3v){x, y, z}; }
static inline m3v vadd(m3v a, m3v b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline m3v vsub(m3v a, m3v b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline m3v vmul(m3v a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static inline m3v vmulv(m3v a, m3v b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float vdot(m3v a, m3v b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline m3v vcross(m3v a, m3v b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float vlen(m3v a) { return sqrtf(vdot(a, a)); }
static inline m3v vnorm(m3v a) {
    float l = vlen(a);
    return l > 1e-20f ? vmul(a, 1 / l) : v3(0, 1, 0);
}
static inline m3v vlerp(m3v a, m3v b, float t) { return vadd(a, vmul(vsub(b, a), t)); }
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

static inline void mat_identity(float *m) {
    for (int i = 0; i < 16; i++)
        m[i] = i % 5 == 0;
}

static inline void mat_mul(float *out, const float *a, const float *b) { /* out = a * b */
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    for (int i = 0; i < 16; i++)
        out[i] = r[i];
}

static inline void mat_trs(float *m, const float *t, const float *q, const float *s) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    m[0] = (1 - 2 * (yy + zz)) * s[0], m[1] = 2 * (xy + wz) * s[0], m[2] = 2 * (xz - wy) * s[0], m[3] = 0;
    m[4] = 2 * (xy - wz) * s[1], m[5] = (1 - 2 * (xx + zz)) * s[1], m[6] = 2 * (yz + wx) * s[1], m[7] = 0;
    m[8] = 2 * (xz + wy) * s[2], m[9] = 2 * (yz - wx) * s[2], m[10] = (1 - 2 * (xx + yy)) * s[2], m[11] = 0;
    m[12] = t[0], m[13] = t[1], m[14] = t[2], m[15] = 1;
}

static inline m3v mat_point(const float *m, m3v p) {
    return v3(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
              m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
}

static inline m3v mat_dir(const float *m, m3v p) {
    return v3(m[0] * p.x + m[4] * p.y + m[8] * p.z, m[1] * p.x + m[5] * p.y + m[9] * p.z, m[2] * p.x + m[6] * p.y + m[10] * p.z);
}

/* the inverse transpose of the upper 3x3, for normals (row-major 3x3 out) */
static inline void mat_normal(const float *m, float *n) {
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float k = fabsf(det) > 1e-30f ? 1 / det : 0;
    /* cofactor matrix / det = inverse transpose */
    n[0] = A * k, n[1] = B * k, n[2] = C * k;
    n[3] = -(b * i - c * h) * k, n[4] = (a * i - c * g) * k, n[5] = -(a * h - b * g) * k;
    n[6] = (b * f - c * e) * k, n[7] = -(a * f - c * d) * k, n[8] = (a * e - b * d) * k;
}

static inline m3v mat3_apply(const float *n, m3v v) {
    return v3(n[0] * v.x + n[1] * v.y + n[2] * v.z, n[3] * v.x + n[4] * v.y + n[5] * v.z, n[6] * v.x + n[7] * v.y + n[8] * v.z);
}

static inline void quat_slerp(float *out, const float *a, const float *b, float t) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float bb[4] = {b[0], b[1], b[2], b[3]};
    if (d < 0)
        d = -d, bb[0] = -bb[0], bb[1] = -bb[1], bb[2] = -bb[2], bb[3] = -bb[3];
    float k0, k1;
    if (d > 0.9995f)
        k0 = 1 - t, k1 = t;
    else {
        float th = acosf(d), s = sinf(th);
        k0 = sinf((1 - t) * th) / s, k1 = sinf(t * th) / s;
    }
    float l = 0;
    for (int i = 0; i < 4; i++)
        out[i] = a[i] * k0 + bb[i] * k1, l += out[i] * out[i];
    l = l > 0 ? 1 / sqrtf(l) : 1;
    for (int i = 0; i < 4; i++)
        out[i] *= l;
}
