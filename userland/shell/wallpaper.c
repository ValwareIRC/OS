#include "wallpaper.h"
#include <math.h>
#include <stdlib.h>

/* Procedural wallpapers. The AI chooses the kind, four colours, a seed and
 * an intensity; these painters turn that into a unique picture. */

static uint32_t rng_state;
static float frand(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)(rng_state >> 8) / 16777216.0f;
}

static float hash2(int x, int y, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffffff) / 16777216.0f;
}

static float smooth(float t) { return t * t * (3 - 2 * t); }

static float vnoise(float x, float y, int seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float tx = smooth(x - xi), ty = smooth(y - yi);
    float a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed);
    float c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
    return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
}

static float fbm(float x, float y, int seed) {
    float v = 0, amp = 0.5f;
    for (int o = 0; o < 4; o++) {
        v += amp * vnoise(x, y, seed + o * 17);
        x *= 2.03f, y *= 2.03f, amp *= 0.5f;
    }
    return v;
}

static color mix4(const color *c, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float s = t * 3;
    int i = (int)s;
    if (i >= 3)
        return c[3];
    return color_mix(c[i], c[i + 1], s - i);
}

static inline void put(canvas *cv, int x, int y, color c) { cv->px[(size_t)y * (size_t)cv->w + (size_t)x] = c | 0xff000000u; }

static void grain(canvas *cv, float amount, int seed) {
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float n = (hash2(x, y, seed) - 0.5f) * amount * 255;
            uint32_t p = cv->px[(size_t)y * (size_t)cv->w + (size_t)x];
            int r = (int)C_R(p) + (int)n, g = (int)C_G(p) + (int)n, b = (int)C_B(p) + (int)n;
            r = r < 0 ? 0 : r > 255 ? 255 : r, g = g < 0 ? 0 : g > 255 ? 255 : g, b = b < 0 ? 0 : b > 255 ? 255 : b;
            put(cv, x, y, RGBA(r, g, b, 255));
        }
}

static void paint_gradient(canvas *cv, const design *d) {
    float ang = (float)(d->wp_seed % 360) * (float)M_PI / 180.0f, ca = cosf(ang), sa = sinf(ang);
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w - 0.5f, v = (float)y / cv->h - 0.5f;
            float t = 0.5f + (u * ca + v * sa) * 0.9f + (fbm(u * 2.5f, v * 2.5f, d->wp_seed) - 0.5f) * 0.25f * d->wp_intensity;
            put(cv, x, y, mix4(d->wp, t));
        }
    grain(cv, 0.025f, d->wp_seed);
}

static void paint_aurora(canvas *cv, const design *d) {
    float k = d->wp_intensity;
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w, v = (float)y / cv->h;
            color base = color_mix(d->wp[0], color_mix(d->wp[0], RGBA(0, 0, 0, 255), 0.35f), v);
            float acc = 0;
            color glow = d->wp[1];
            for (int band = 0; band < 3; band++) {
                float center = 0.25f + band * 0.16f + (fbm(u * 1.6f + band, band * 3.1f, d->wp_seed) - 0.5f) * 0.45f;
                float width = 0.06f + 0.05f * fbm(u * 3 + band * 7, 1.7f, d->wp_seed + 3);
                float dist = (v - center) / width;
                float a = expf(-dist * dist) * (0.55f + 0.45f * fbm(u * 6, v * 2 + band, d->wp_seed + 9));
                if (a > acc) {
                    acc = a;
                    glow = d->wp[1 + band % 3];
                }
            }
            put(cv, x, y, color_mix(base, glow, acc * k));
        }
    grain(cv, 0.02f, d->wp_seed);
}

static void paint_waves(canvas *cv, const design *d) {
    fill_vgradient(cv, (rect){0, 0, cv->w, cv->h}, d->wp[3], d->wp[0]);
    rng_state = (uint32_t)d->wp_seed * 7919u + 1;
    int layers = 5;
    for (int l = 0; l < layers; l++) {
        float base = cv->h * (0.45f + l * 0.11f);
        float amp = cv->h * (0.03f + 0.04f * frand()) * d->wp_intensity;
        float freq = 1.5f + frand() * 2.5f, phase = frand() * 6.28f;
        color col = color_mix(d->wp[1 + l % 3], d->wp[0], (float)l / (layers + 1));
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w;
            float top = base + amp * sinf(u * freq * 6.28f + phase) + amp * 0.4f * sinf(u * freq * 13.1f + phase * 2);
            for (int y = (int)top; y < cv->h; y++)
                if (y >= 0) {
                    float edge = top - floorf(top);
                    color c = y == (int)top ? color_mix(cv->px[(size_t)y * (size_t)cv->w + (size_t)x], col, 1 - edge) : col;
                    put(cv, x, y, c);
                }
        }
    }
    grain(cv, 0.02f, d->wp_seed);
}

static void paint_bokeh(canvas *cv, const design *d) {
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w, v = (float)y / cv->h;
            put(cv, x, y, color_mix(d->wp[0], d->wp[3], fbm(u * 1.5f, v * 1.5f, d->wp_seed) * 0.8f));
        }
    rng_state = (uint32_t)d->wp_seed * 104729u + 3;
    int n = 40 + (int)(60 * d->wp_intensity);
    for (int i = 0; i < n; i++) {
        float r = cv->h * (0.02f + 0.08f * frand() * frand());
        float x = frand() * cv->w, y = frand() * cv->h;
        color c = color_alpha(d->wp[1 + i % 3], 0.12f + 0.25f * frand());
        fill_circle(cv, x, y, r, c);
        fill_circle(cv, x, y, r * 0.8f, color_alpha(c, 0.5f));
    }
    blur_region(cv, (rect){0, 0, cv->w, cv->h}, 6);
    grain(cv, 0.02f, d->wp_seed);
}

static void paint_dunes(canvas *cv, const design *d) {
    fill_vgradient(cv, (rect){0, 0, cv->w, cv->h * 2 / 3}, d->wp[3], d->wp[2]);
    fill_rect(cv, (rect){0, cv->h * 2 / 3, cv->w, cv->h - cv->h * 2 / 3}, d->wp[2]);
    rng_state = (uint32_t)d->wp_seed * 15485863u + 5;
    for (int l = 0; l < 4; l++) {
        float base = cv->h * (0.55f + l * 0.12f), amp = cv->h * 0.08f * d->wp_intensity;
        float ph = frand() * 6.28f, fr = 0.8f + frand();
        color top = color_mix(d->wp[1], d->wp[0], (float)l / 4), bottom = color_mix(top, d->wp[0], 0.5f);
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w;
            float crest = base - amp * (0.6f * sinf(u * fr * 6.28f + ph) + 0.4f * fbm(u * 3, (float)l, d->wp_seed));
            for (int y = (int)crest; y < cv->h; y++)
                if (y >= 0)
                    put(cv, x, y, color_mix(top, bottom, (y - crest) / (cv->h - crest + 1)));
        }
    }
    grain(cv, 0.03f, d->wp_seed);
}

static void paint_sun(canvas *cv, const design *d) {
    float horizon = cv->h * 0.66f;
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float v = (float)y / horizon;
            color sky = y < horizon ? mix4(d->wp, 1 - v * 0.9f) : color_mix(d->wp[1], d->wp[0], (y - horizon) / (cv->h - horizon));
            put(cv, x, y, sky);
        }
    rng_state = (uint32_t)d->wp_seed + 11;
    float sx = cv->w * (0.3f + 0.4f * frand()), sr = cv->h * (0.12f + 0.06f * d->wp_intensity);
    for (int g = 6; g >= 1; g--)
        fill_circle(cv, sx, horizon, sr * (1 + g * 0.35f), color_alpha(d->wp[2], 0.05f));
    fill_circle(cv, sx, horizon, sr, color_alpha(color_mix(d->wp[2], RGBA(255, 250, 240, 255), 0.4f), 0.95f));
    fill_rect(cv, (rect){0, (int)horizon, cv->w, cv->h - (int)horizon}, color_alpha(d->wp[0], 0.85f));
    for (int i = 0; i < 9; i++) { /* reflections on water */
        float y = horizon + 8 + i * (cv->h - horizon) / 10.0f;
        float w = sr * (1.6f - i * 0.12f);
        fill_rect(cv, (rect){(int)(sx - w / 2), (int)y, (int)w, 3}, color_alpha(d->wp[2], 0.35f - i * 0.03f));
    }
    grain(cv, 0.03f, d->wp_seed);
}

static void paint_constellation(canvas *cv, const design *d) {
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w, v = (float)y / cv->h;
            put(cv, x, y, color_mix(d->wp[0], d->wp[3], fbm(u * 2, v * 2, d->wp_seed) * 0.6f * d->wp_intensity + v * 0.2f));
        }
    rng_state = (uint32_t)d->wp_seed * 31u + 7;
    float px[60], py[60];
    for (int i = 0; i < 60; i++) {
        px[i] = frand() * cv->w;
        py[i] = frand() * cv->h;
    }
    for (int i = 0; i < 60; i++)
        for (int j = i + 1; j < 60; j++) {
            float dx = px[i] - px[j], dy = py[i] - py[j];
            if (dx * dx + dy * dy < (cv->h * 0.12f) * (cv->h * 0.12f))
                draw_line(cv, px[i], py[i], px[j], py[j], 1, color_alpha(d->wp[1], 0.25f));
        }
    for (int i = 0; i < 400; i++)
        fill_circle(cv, frand() * cv->w, frand() * cv->h, 0.6f + frand(), color_alpha(d->wp[2], 0.3f + 0.6f * frand()));
    for (int i = 0; i < 60; i++)
        fill_circle(cv, px[i], py[i], 2.2f, d->wp[2]);
}

static void paint_paper(canvas *cv, const design *d) {
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w, v = (float)y / cv->h;
            float fib = fbm(u * 40, v * 40, d->wp_seed) * 0.06f + fbm(u * 3, v * 3, d->wp_seed + 1) * 0.12f;
            put(cv, x, y, color_mix(d->wp[0], d->wp[3], fib * d->wp_intensity * 3));
        }
    rng_state = (uint32_t)d->wp_seed * 2654435761u + 13;
    for (int i = 0; i < 7; i++) { /* soft watercolour blooms */
        float x = frand() * cv->w, y = frand() * cv->h, r = cv->h * (0.1f + 0.2f * frand());
        for (int k = 0; k < 5; k++)
            fill_circle(cv, x + (frand() - 0.5f) * r * 0.4f, y + (frand() - 0.5f) * r * 0.4f, r * (1 - k * 0.15f),
                        color_alpha(d->wp[1 + i % 3], 0.05f * d->wp_intensity + 0.02f));
    }
    grain(cv, 0.035f, d->wp_seed);
}

/* ---- living wallpapers: rendered small every frame, then upscaled ---- */

static float fbm3(float x, float y, int seed) {
    return 0.55f * vnoise(x, y, seed) + 0.3f * vnoise(x * 2.1f, y * 2.1f, seed + 7) + 0.15f * vnoise(x * 4.3f, y * 4.3f, seed + 13);
}

static color add_light(color base, color light, float amount) {
    if (amount <= 0)
        return base;
    if (amount > 1.5f)
        amount = 1.5f;
    int r = (int)(C_R(base) + C_R(light) * amount), g = (int)(C_G(base) + C_G(light) * amount),
        b = (int)(C_B(base) + C_B(light) * amount);
    return RGBA(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, 255);
}

static void live_aurora(canvas *cv, const design *d, float t) {
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w, v = (float)y / cv->h;
            color c = color_mix(d->wp[0], d->wp[3], v * 0.8f);
            for (int band = 0; band < 3; band++) {
                float center = 0.28f + band * 0.15f + (fbm3(u * 1.4f + band * 3 + t * 0.05f, band + t * 0.03f, d->wp_seed) - 0.5f) * 0.5f;
                float width = 0.07f + 0.05f * vnoise(u * 3 + band, t * 0.1f, d->wp_seed + 5);
                float dist = (v - center) / width;
                float a = expf(-dist * dist) * (0.5f + 0.5f * fbm3(u * 7 - t * 0.12f, v * 2 + band, d->wp_seed + 9));
                c = add_light(c, d->wp[1 + band % 2], a * 0.75f * d->wp_intensity);
            }
            cv->px[(size_t)y * (size_t)cv->w + (size_t)x] = c;
        }
}

static void live_nebula(canvas *cv, const design *d, float t) {
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w * 2.4f, v = (float)y / cv->h * 1.35f;
            float wx = fbm3(u + t * 0.02f, v, d->wp_seed), wy = fbm3(u, v - t * 0.015f, d->wp_seed + 3);
            float n = fbm3(u + wx * 1.6f, v + wy * 1.6f, d->wp_seed + 11);
            color c = color_mix(d->wp[0], d->wp[3], n * 0.9f);
            c = add_light(c, d->wp[1], powf(n, 3.0f) * 1.3f * d->wp_intensity);
            c = add_light(c, d->wp[2], powf(wy, 4.0f) * 0.9f * d->wp_intensity);
            cv->px[(size_t)y * (size_t)cv->w + (size_t)x] = c;
        }
}

static void live_liquid(canvas *cv, const design *d, float t) {
    float bx[5], by[5], br[5];
    for (int i = 0; i < 5; i++) {
        float ph = (float)(d->wp_seed % 97) * 0.1f + i * 1.7f;
        bx[i] = 0.5f + 0.38f * sinf(t * (0.07f + i * 0.013f) + ph);
        by[i] = 0.5f + 0.34f * cosf(t * (0.05f + i * 0.017f) + ph * 1.3f);
        br[i] = 0.16f + 0.05f * (float)(i % 3);
    }
    float aspect = (float)cv->w / cv->h;
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w, v = (float)y / cv->h, f = 0;
            int nearest = 0;
            float best = 0;
            for (int i = 0; i < 5; i++) {
                float dx = (u - bx[i]) * aspect, dy = v - by[i];
                float k = br[i] * br[i] / (dx * dx + dy * dy + 1e-4f);
                f += k;
                if (k > best)
                    best = k, nearest = i;
            }
            color base = color_mix(d->wp[0], d->wp[3], v);
            /* Soft edges: the field is drawn small and scaled up, so any
             * sharp transition would show as steps. */
            float edge = smooth(fminf(1, fmaxf(0, (f - 0.45f) / 0.9f)));
            color blob = color_mix(d->wp[1 + nearest % 2], d->wp[2], fminf(1, fmaxf(0, (f - 1) * 0.35f)));
            color c = color_mix(base, blob, edge * 0.85f * d->wp_intensity + 0.1f);
            float rim = expf(-(f - 1.1f) * (f - 1.1f) * 4);
            cv->px[(size_t)y * (size_t)cv->w + (size_t)x] = add_light(c, RGBA(255, 255, 255, 255), rim * 0.18f);
        }
}

static void live_flow(canvas *cv, const design *d, float t) {
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w, v = (float)y / cv->h;
            color c = color_mix(d->wp[0], d->wp[3], (u + v) * 0.5f);
            for (int k = 0; k < 6; k++) {
                float yk = 0.25f + k * 0.1f + 0.08f * sinf(u * (3 + k * 0.7f) + t * (0.15f + k * 0.03f) + k) +
                           0.04f * sinf(u * 9 - t * 0.2f + k * 2);
                float dist = fabsf(v - yk);
                float line = expf(-dist * dist * 9000) * 0.9f + expf(-dist * dist * 400) * 0.18f;
                c = add_light(c, d->wp[1 + k % 2], line * d->wp_intensity);
            }
            cv->px[(size_t)y * (size_t)cv->w + (size_t)x] = c;
        }
}

static void live_prism(canvas *cv, const design *d, float t) {
    float ox = (d->wp_seed % 2) ? 1.05f : -0.05f, oy = -0.1f;
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float u = (float)x / cv->w, v = (float)y / cv->h;
            float ang = atan2f(v - oy, u - ox), dist = sqrtf((u - ox) * (u - ox) + (v - oy) * (v - oy));
            color c = color_mix(d->wp[0], d->wp[3], dist * 0.7f);
            for (int k = 0; k < 4; k++) {
                float beam = 0.5f + 0.5f * sinf(ang * (7 + k * 3) + t * (0.08f + k * 0.02f) + k * 1.9f);
                beam = powf(beam, 12.0f) * (1.2f - dist);
                c = add_light(c, d->wp[1 + k % 2], beam * 0.6f * d->wp_intensity);
            }
            c = add_light(c, d->wp[2], expf(-dist * dist * 6) * 0.35f * d->wp_intensity);
            cv->px[(size_t)y * (size_t)cv->w + (size_t)x] = c;
        }
}

bool wallpaper_animated(const design *d) {
    return d->motion > 0.05f && (d->wallpaper == WP_AURORA || d->wallpaper == WP_NEBULA || d->wallpaper == WP_LIQUID ||
                                 d->wallpaper == WP_FLOW || d->wallpaper == WP_PRISM);
}

/* Renders one frame of a living wallpaper onto `out` (full size). */
void wallpaper_frame(canvas *out, const design *d, float seconds) {
    static canvas *small;
    int sw = out->w / 5, sh = out->h / 5;
    if (!small || small->w != sw || small->h != sh) {
        canvas_free(small);
        small = canvas_new(sw, sh);
    }
    float t = seconds * (0.3f + 1.7f * d->motion);
    switch (d->wallpaper) {
    case WP_NEBULA: live_nebula(small, d, t); break;
    case WP_LIQUID: live_liquid(small, d, t); break;
    case WP_FLOW: live_flow(small, d, t); break;
    case WP_PRISM: live_prism(small, d, t); break;
    default: live_aurora(small, d, t); break;
    }
    /* A one-pixel blur at low resolution keeps the upscaled image smooth. */
    blur_region(small, (rect){0, 0, sw, sh}, 1);
    /* gentle vignette for depth */
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            float u = (float)x / sw - 0.5f, v = (float)y / sh - 0.5f;
            float k = 1 - (u * u + v * v) * 0.9f;
            uint32_t p = small->px[(size_t)y * (size_t)sw + (size_t)x];
            small->px[(size_t)y * (size_t)sw + (size_t)x] = RGBA((int)(C_R(p) * k), (int)(C_G(p) * k), (int)(C_B(p) * k), 255);
        }
    canvas_scale(out, small);
    if (d->wallpaper == WP_NEBULA) {
        /* Stars, drawn at full resolution so they stay sharp, twinkling. */
        uint32_t st = (uint32_t)d->wp_seed * 2654435761u + 1;
        for (int i = 0; i < 260; i++) {
            st = st * 1664525u + 1013904223u;
            float x = (float)(st >> 8) / 16777216.0f * out->w;
            st = st * 1664525u + 1013904223u;
            float y = (float)(st >> 8) / 16777216.0f * out->h;
            st = st * 1664525u + 1013904223u;
            float base = (float)(st >> 8) / 16777216.0f;
            float tw = 0.55f + 0.45f * sinf(seconds * (0.6f + base * 1.8f) + i);
            float a = (0.15f + 0.6f * base * base) * tw;
            fill_circle(out, x, y, 0.5f + base * 0.9f, color_alpha(color_mix(d->wp[2], RGBA(255, 255, 255, 255), 0.6f), a));
            if (base > 0.93f)
                glow_circle(out, x, y, 9, color_alpha(d->wp[1], 0.25f * tw));
        }
    }
}

void wallpaper_paint(canvas *cv, const design *d) {
    if (wallpaper_animated(d) || d->wallpaper >= WP_NEBULA) {
        wallpaper_frame(cv, d, 0);
        return;
    }
    switch (d->wallpaper) {
    case WP_AURORA: paint_aurora(cv, d); break;
    case WP_WAVES: paint_waves(cv, d); break;
    case WP_BOKEH: paint_bokeh(cv, d); break;
    case WP_DUNES: paint_dunes(cv, d); break;
    case WP_SUN: paint_sun(cv, d); break;
    case WP_CONSTELLATION: paint_constellation(cv, d); break;
    case WP_PAPER: paint_paper(cv, d); break;
    default: paint_gradient(cv, d); break;
    }
}
