#pragma once
/* A Facet 2 app's drawn view as a Facet document in a person's design (markup.c). */
#include "fx_rt.h"
#include "../design.h"

/* The document (malloc'd), and its clickable elements and inputs in the order of their what="fx:N" / fxin="N". */
char *fx_markup(fx_out *view, const fx_app *app, const design *d, fx_out ***acts, int *nacts);
