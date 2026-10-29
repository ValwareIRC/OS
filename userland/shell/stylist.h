#pragma once
/* The stylist: the AI looks at its own work before the person does.
 *
 * A design document is laid out and drawn offscreen, the lint (fm/lint.c)
 * says what looks broken, and, with a model that can see, pictures of the
 * desktop go back to it for an art director's critique and a corrected
 * document. Starter themes (/usr/share/os/themes) give it finished,
 * tested desktops to personalise instead of a blank page. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../lib/ai.h"
#include "../lib/json.h"
#include "fm/lint.h"

#define STYLIST_THEMES "/usr/share/os/themes"

typedef struct stylist_look {
    bool ok;               /* it parsed and makes a usable desktop */
    char problems[600];    /* why not, when !ok */
    fm_lint lint;
    char *jpeg_b64[2];     /* pictures (when asked): the desktop; its menu open over a window */
    char shows[2][160];    /* what each picture shows, for the prompt */
    int npictures;
} stylist_look;

/* Lays a design out at the screen's size, offscreen, as the desktop would
 * show it; lints it, and draws pictures of it when asked. Uses (and
 * restores) the shell's frame, design and input while it works. */
void stylist_look_at(const char *doc_text, bool pictures, stylist_look *out);
void stylist_look_free(stylist_look *l);
/* Saves the pictures of the last look as JPEG files (for tests). */
bool stylist_write_jpeg(const char *path, const char *b64);

/* What an app window shows while drawing offscreen: a calm stand-in. */
void stylist_placeholder_view(const char *app, rect r);

/* A reply that starts from a theme (<facet base="paper">, only the changed
 * parts) made whole. Returns the <facet> document alone (malloc'd), or
 * NULL if the reply has none. */
char *stylist_expand_base(const char *reply);
/* Set while the preview reads a design as it is written (no log lines). */
extern bool stylist_quiet;
/* The starter themes, for the design prompt. */
void stylist_theme_menu(sbuf *b);

/* In the design job: lint, look, critique, repair. Returns the best whole
 * document (malloc'd), or NULL to keep the reply as it is. */
char *stylist_refine(const struct ai_config *cfg, const char *system, const char *request, const char *reply);

/* Progress for the designing screen: set in the design job, read by the
 * screen. */
void stylist_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool stylist_read_status(char *out, size_t n);
void stylist_clear_status(void);

/* After the companion edits the desktop with [[ui:]] tags: lints the
 * result and returns what the edit broke (findings that weren't there
 * before), malloc'd, or NULL if nothing. `before` is the document text
 * from just before the edit. */
char *stylist_check_edit(const char *before, struct fm_doc *after);

/* The design as it is being written, for the preview to show as it grows.
 * Set in the design job: `whole` false for the reply so far, true for a
 * complete document (the first draft, and each better round). The reader
 * gets a malloc'd copy only when it changed since *seen. */
void stylist_show(const char *text, size_t len, bool whole);
char *stylist_read_show(uint64_t *seen, bool *whole);
void stylist_clear_show(void);
