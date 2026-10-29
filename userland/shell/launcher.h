#pragma once
#include <stdbool.h>
#include "gfx.h"

/* The launcher: one search for everything (apps, files, settings,
 * commands, sums, and the companion) (launcher.c).
 *
 * It appears two ways. A design's own <search> field, when it has the
 * keyboard, grows a list of results under (or over) itself in the design's
 * material. what="launcher" (and the Super key, when the design gave that
 * key to nothing) opens it as a panel in the middle of the screen. */

struct fm_doc;

/* Opens the panel, optionally with something already typed. */
void launcher_open(const char *query);
void launcher_close(void);
bool launcher_active(void);
/* Each desktop frame, after layout and before anything else takes input:
 * while the panel is up it takes the keyboard and the pointer; while a
 * design's <search> is focused it takes the keys that move through results
 * (and the pointer over them). */
void launcher_input(struct fm_doc *ui);
/* Each desktop frame, over everything else. */
void launcher_draw(void);
/* Something under /root changed: look again before the next search. */
void launcher_files_changed(void);
/* Apps were installed or removed: read their .desktop entries again. */
void launcher_apps_changed(void);
/* Starts what a .desktop file describes; false if it can't be read. */
bool launcher_run_desktop_file(const char *path);

/* Things opened recently, for the empty search and Files' sidebar. */
struct recent {
    char kind; /* 'a' app, 'x' program, 'f' file, 'c' command, 's' setting */
    char title[96];
    char target[512];
};
void recent_add(char kind, const char *title, const char *target);
int recent_list(const struct recent **out);
void recent_forget(const char *target);

/* A panel in the design's material (glass, frost, solid or neon). */
void material_panel(rect r, float radius);
