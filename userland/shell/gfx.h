#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Software renderer: 32-bit ARGB canvases, anti-aliased rounded shapes,
 * gradients, soft shadows and alpha blending. Everything the AI designs is
 * drawn with these primitives. */

typedef uint32_t color; /* 0xAARRGGBB */

#define RGBA(r, g, b, a) ((color)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define C_A(c) (((c) >> 24) & 0xff)
#define C_R(c) (((c) >> 16) & 0xff)
#define C_G(c) (((c) >> 8) & 0xff)
#define C_B(c) ((c) & 0xff)

typedef struct canvas {
    int w, h;
    uint32_t *px; /* w*h, row-major */
    /* clip rectangle */
    int cx0, cy0, cx1, cy1;
} canvas;

typedef struct rect {
    int x, y, w, h;
} rect;

canvas *canvas_new(int w, int h);
void canvas_free(canvas *c);
void canvas_clip(canvas *c, rect r);
void canvas_unclip(canvas *c);
void canvas_blit(canvas *dst, const canvas *src, int x, int y);

color color_mix(color a, color b, float t);
color color_alpha(color c, float a); /* multiply alpha */
color color_parse(const char *hex, color fallback); /* "#rrggbb" / "#rrggbbaa" */
float color_luma(color c);
float color_contrast(color a, color b); /* WCAG ratio */

void fill_rect(canvas *c, rect r, color col);
void fill_round(canvas *c, rect r, float radius, color col);
void stroke_round(canvas *c, rect r, float radius, float width, color col);
void fill_circle(canvas *c, float cx, float cy, float radius, color col);
void fill_vgradient(canvas *c, rect r, color top, color bottom);
void fill_round_vgradient(canvas *c, rect r, float radius, color top, color bottom);
void shadow_round(canvas *c, rect r, float radius, float blur, color col);
void blur_region(canvas *c, rect r, int radius); /* frosted glass under panels */
void blur_round(canvas *c, rect r, float corner_radius, int strength);
void draw_line(canvas *c, float x0, float y0, float x1, float y1, float width, color col);

/* ---- light and depth ---- */
/* Soft radial light: full `col` at the centre fading to nothing at `radius`. */
void glow_circle(canvas *c, float cx, float cy, float radius, color col);
/* Neon-style halo around a rounded rectangle. */
void glow_round(canvas *c, rect r, float radius, float spread, color col);
/* A rounded outline whose colour runs from `top` to `bottom` (glass edges). */
void stroke_round_vgradient(canvas *c, rect r, float radius, float width, color top, color bottom);
/* Boost saturation and lift brightness inside a rounded rect (vibrancy). */
void vibrancy(canvas *c, rect r, float radius, float saturate, float lift);
void draw_arc(canvas *c, float cx, float cy, float radius, float a0, float a1, float width, color col);
/* Faster large blurs: work at half resolution. */
void blur_fast(canvas *c, rect r, float corner_radius, int strength);
/* Bilinear upscale of src onto all of dst. */
void canvas_scale(canvas *dst, const canvas *src);

bool pt_in(rect r, int x, int y);
rect rect_inset(rect r, int d);

/* Rounded fill whose colour comes from a callback per pixel (gradients). */
typedef color (*shade_fn)(void *ctx, int x, int y);
void fill_round_shaded(canvas *c, rect r, float radius, shade_fn shade, void *ctx);
