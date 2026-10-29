#pragma once
#include <stdbool.h>
#include "gfx.h"

/* TrueType text through stb_truetype, with a glyph cache per face and size.
 * Faces are referred to by family name ("Inter", "Lora", ...); the AI picks
 * families for each user's design from what is installed. Each family may
 * have several faces (Inter-Bold.ttf, Inter-Italic.ttf, ...); weights and
 * italics it lacks are synthesised from the nearest face. */

typedef struct font font;

void fonts_init(const char *dir);
font *font_get(const char *family, float px); /* falls back to Inter */
/* weight 100..900 (400 regular, 700 bold); tracking is letter-spacing in px. */
font *font_get_styled(const char *family, float px, int weight, bool italic, float tracking);
/* The same family and size with another weight, slant or tracking
 * (weight 0 keeps the base font's). */
font *font_with(font *base, int weight, bool italic, float tracking);
float font_px(font *f);
const char *const *font_families(int *count);

int text_width(font *f, const char *s);
int text_width_n(font *f, const char *s, int n);
int font_line_height(font *f);
int font_ascent(font *f);
/* Draws one line; returns the advance width. y is the top of the line. */
int draw_text(canvas *c, font *f, int x, int y, const char *s, color col);
int draw_text_n(canvas *c, font *f, int x, int y, const char *s, int n, color col);
/* Word-wrapped block; returns the height used. Draws nothing if c is NULL. */
int draw_text_wrapped(canvas *c, font *f, rect box, const char *s, color col, float line_spacing);
/* Centred single line inside a rect (vertically and horizontally). */
void draw_text_centered(canvas *c, font *f, rect r, const char *s, color col);

/* ---- text.c: setting text in boxes ---- */

enum text_align { TEXT_LEFT, TEXT_CENTER, TEXT_RIGHT };
/* Word-wrapped block with each line aligned in the box. With max_lines > 0
 * the text stops there and the last line ends in an ellipsis if more was
 * cut. Returns the height used; draws nothing if c is NULL. */
int draw_text_box(canvas *c, font *f, rect box, const char *s, color col, float line_spacing, int align,
                  int max_lines);
typedef struct text_shadow {
    float dx, dy, blur;
    color col;
} text_shadow;
int draw_text_box_ex(canvas *c, font *f, rect box, const char *s, color col, float line_spacing, int align,
                     int max_lines, const text_shadow *shadow);
/* Lines draw_text_box would use (before any max_lines). */
int text_line_count(font *f, int width, const char *s);
/* Copies s into out, shortened with "…" to fit width px. Returns its width. */
int text_ellipsize(font *f, const char *s, int width, char *out, int outlen);
/* A soft shadow of a line of text: drawn at (x, y) blurred by `blur` px. */
void draw_text_shadow(canvas *c, font *f, int x, int y, const char *s, int n, color col, float blur);
