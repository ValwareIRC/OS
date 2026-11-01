#pragma once
/* A Facet 2 app's drawn view as a Facet document in a person's design (markup.c), and its widgets (widgets.c). */
#include "fx_rt.h"
#include "../design.h"
#include "../gfx.h"

/* The document (malloc'd), and its clickable elements, inputs and widgets in the order of their what="fx:N" /
 * fxin="N". */
char *fx_markup(fx_out *view, const fx_app *app, const design *d, fx_out ***acts, int *nacts);
/* token(name) as this design's value: a colour as #rrggbb, or a font's name. */
void fx_token_value(const design *d, const char *name, char *out, size_t n);
/* ring, progress, chart: drawn by the shell into their boxes. */
bool fx_is_widget(const char *tag);
/* Draws one into r. `shown` is the value to show (animated towards value by the caller), or < 0 for value itself. */
void fx_widget_draw(canvas *c, rect r, const fx_out *o, const design *d, float shown);
