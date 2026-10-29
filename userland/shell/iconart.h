#pragma once
#include <stdbool.h>
#include "gfx.h"

/* Icons as pictures: the built-in apps and the kinds of file, drawn from
 * geometry with gradients, highlights and soft shadows (iconart.c), and
 * Linux apps' own icons read from their .desktop files (appicon.c).
 *
 * A subject is a built-in app (enum app_id, 0..APP_COUNT-1) or one of the
 * file kinds below. */
enum icon_subject {
    ICON_FOLDER = 100,
    ICON_DOCUMENT,
    ICON_IMAGE,
    ICON_AUDIO,
    ICON_VIDEO,
    ICON_ARCHIVE,
    ICON_CODE,
    ICON_EXECUTABLE,
};

/* icon-style: how an icon is rendered. "color" is the default; "line" is
 * the old single-stroke symbol; "mono" (or "glyph") is the colour picture
 * recoloured in the text colour; "classic" is glossy and outlined, XP-era.
 * The older filled|glow|duotone tiles are LOOK_LEGACY (icons.c draws them). */
enum icon_look { LOOK_COLOR, LOOK_LINE, LOOK_MONO, LOOK_CLASSIC, LOOK_LEGACY };
enum icon_look icon_look_of(const char *style);
/* icon-shape: 0 no tile (the object alone), 1 circle, 2 squircle, 3 square. */
int icon_tile_of(const char *shape);

/* Draws a subject in r (square). tint colours mono icons; opacity fades it. */
void icon_art(canvas *c, rect r, int subject, enum icon_look look, int tile, color tint, float opacity, bool hover);
/* A built-in app's icon for an icon-style/icon-shape pair; false when the
 * pair is one of the older looks that ui_icon (icons.c) draws. */
bool icon_art_styled(canvas *c, rect r, int app, const char *style, const char *shape, color tint, float opacity,
                     bool hover);
/* True when a Facet icon for this app is the plain line symbol. */
bool icon_is_line(int app, const char *style, const char *shape);
/* The file kind for a name ("photo.png" -> ICON_IMAGE). */
int icon_file_subject(const char *name, bool is_dir, bool is_exec);
/* A file kind by name: "folder", "document", "image", "audio", "video",
 * "archive", "code", "executable" (and a few synonyms); -1 if none. */
int icon_subject_named(const char *name);

/* appicon.c: a Linux app's own icon (its .desktop Icon=, from hicolor or
 * pixmaps; PNG only), scaled and cached. False if it has none we can read. */
bool app_icon_draw(canvas *c, rect r, const char *id, const char *exec, int tile, enum icon_look look, color tint,
                   float opacity, bool hover);
