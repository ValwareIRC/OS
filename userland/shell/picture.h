#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

/* Pictures of the screen for a model to look at: a small baseline JPEG
 * encoder, a box-filter shrink and base64. */

/* Encodes w*h pixels (0xAARRGGBB, alpha ignored) as a baseline JPEG with
 * 4:2:0 colour. Returns a malloc'd buffer and its size in *len. */
unsigned char *jpeg_encode(const uint32_t *px, int w, int h, int stride, int quality, size_t *len);
/* A copy of src shrunk by an integer factor (2 = half size), averaging. */
canvas *canvas_shrink(const canvas *src, int factor);
/* malloc'd, NUL-terminated base64 of the bytes. */
char *base64_encode(const unsigned char *data, size_t len);
