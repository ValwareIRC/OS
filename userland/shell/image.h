#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"

/* Pictures from files (PNG, JPEG, GIF, BMP, TGA, PNM) through stb_image,
 * for thumbnails in Files and its picture viewer. */

typedef struct image {
    int w, h;
    uint32_t *px; /* 0xAARRGGBB, straight alpha */
    int src_w, src_h; /* the picture's own size, before any shrinking */
} image;

/* Loads a picture, shrunk (never enlarged) so neither side exceeds max_side;
 * 0 keeps it whole. NULL if it can't be read. */
image *image_load(const char *path, int max_side);
/* A copy shrunk (or kept) to exactly w x h, averaging the pixels it covers. */
image *image_resized(const image *im, int w, int h);
void image_free(image *im);
/* Draws it 1:1 at x, y, blended over what is there, inside the clip. */
void image_draw(canvas *c, const image *im, int x, int y, float opacity);
/* Does the name look like a picture this can show? */
bool image_name_ok(const char *name);
