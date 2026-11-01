#pragma once
/* Facet 2 apps in windows (fxwin.c): an app file run by the interpreter
 * (fx/), drawn by the desktop's own Facet engine in the person's design. */
#include "gfx.h"
#include <stdbool.h>

struct fxwin;
struct fm_doc;

/* Opens the app in this file as a window; its index, or -1 (problems are told as a toast and logged). */
int fxwin_open(const char *path);
/* Its window's body: runs its timers and data, lays it out in r, takes the pointer if `pointer_here` and the
 * keys if `focused`, and draws it. */
void fxwin_draw(struct fxwin *fw, rect r, bool focused, bool pointer_here);
const char *fxwin_title(const struct fxwin *fw);
void fxwin_close(struct fxwin *fw);
/* Frees the apps whose windows went (an app may close itself while it runs: freed after). */
void fxwin_reap(void);
/* what="fx:N" from one of its elements (desktop.c's fm_host_action passes these on). */
bool fxwin_action(struct fm_doc *d, const char *a);
/* Apps in ~/Apps (*.facet): their names, for the app list. */
int fxwin_list(char names[][64], char paths[][300], int max);
