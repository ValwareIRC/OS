#pragma once
#include <stdbool.h>
#include "gfx.h"

/* Files: the person's things under /root, with a sidebar, breadcrumbs,
 * list and grid views, a picture viewer, Trash, and drag and drop
 * (files.c). */

#define HOME_DIR "/root"
#define TRASH_DIR HOME_DIR "/.Trash"
#define TRASH_FILES TRASH_DIR "/files" /* what was deleted */
#define TRASH_INFO TRASH_DIR "/info"   /* where each thing came from */

/* What kind of thing a file is, for its icon and for what opens it. */
enum file_kind { FK_FOLDER, FK_TEXT, FK_CODE, FK_IMAGE, FK_AUDIO, FK_VIDEO, FK_ARCHIVE, FK_DOC, FK_PROGRAM, FK_APP,
                 FK_OTHER, FK_COUNT };

/* The Files app, in its window or as <view app="files"> on the desktop. */
void files_body(rect r);
/* Opens a file with the app that suits it (folders in Files). */
void files_open_path(const char *path);
/* Opens Files at the file's folder with it selected. */
void files_reveal(const char *path);
void files_empty_trash(void);
/* Creates the home folders and Trash on first run. */
void files_setup(void);

int file_kind(const char *path, bool is_dir);
const char *file_kind_name(int kind);
/* The icon for a kind of file, in the design's colours. */
void file_icon(canvas *c, rect r, int kind, const char *name, bool selected);
/* Small interface glyphs (not app icons): chevrons, trash, home... */
enum ui_glyph { G_BACK, G_FORWARD, G_UP, G_LIST, G_GRID, G_NEWFOLDER, G_TRASH, G_HOME, G_DOWNLOAD, G_PICTURE,
                G_CLOCK, G_SPEAKER, G_SEARCH, G_MOON, G_SUN, G_RESTORE, G_SORT, G_DESKTOP, G_CALC, G_DOC, G_MUSIC,
                G_POWER, G_TEXT, G_SPARK, G_GEAR, G_TERMINAL, G_FOLDER, G_EYE };
void ui_glyph(canvas *c, rect r, int glyph, float stroke, color col);
/* Starts a program the way a terminal would (sh -c), detached. */
void spawn_command(const char *cmd, const char *name);

/* Notes (desktop.c) */
void open_in_notes(const char *path);
