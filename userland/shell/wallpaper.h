#pragma once
#include "design.h"
#include "gfx.h"

/* Paints the design's wallpaper (the first frame, for living ones). */
void wallpaper_paint(canvas *cv, const design *d);
/* Living wallpapers move: true if this design's should be redrawn per frame. */
bool wallpaper_animated(const design *d);
void wallpaper_frame(canvas *out, const design *d, float seconds);
