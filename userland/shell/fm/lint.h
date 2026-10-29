#pragma once
/* Design lint: looks at a laid-out, drawn design for the things that make a
 * desktop look broken (text you can't read, things off the screen or on
 * top of each other, labels cut short, menus taller than the screen, apps
 * nothing opens) and says, in a line each, what to change. The findings go
 * back to the AI that wrote the design, so it can repair its own work. */
#include "fm.h"

typedef struct fm_lint_opts {
    canvas *work;           /* screen-sized scratch canvas; the host draws icons into it, so it
                               must be the one the host's callbacks draw on while linting */
    const canvas *backdrop; /* the wallpaper as painted, same size; NULL: the wallpaper tone */
    rect screen;
    float min_contrast;     /* 4.5, or 7 when they asked for strong contrast */
    int core_apps;          /* the first n host apps must be reachable by a click */
} fm_lint_opts;

typedef struct fm_lint {
    int count;       /* distinct findings */
    int score;       /* weighted by how bad they look: 0 is clean, lower is better */
    char text[8000]; /* one finding per line, "- ...", worst first */
} fm_lint;

/* Lints a design. It opens each popover in turn and lays the document out
 * again, so give it a scratch copy rather than the live desktop. */
void fm_lint_run(fm_doc *d, const fm_lint_opts *o, fm_lint *out);

/* Draws the design into o->work the way the desktop shows it: the backdrop,
 * the surfaces, one sample window when `window` is set, and the popover
 * `open` opened (NULL: everything closed). Used for the pictures the AI
 * looks at, and by the lint itself. */
void fm_lint_paint(fm_doc *d, const fm_lint_opts *o, fm_node *open, bool window);
/* The popover worth showing open: the one on the Windows key, else the
 * first one something opens. NULL if the design has none. */
fm_node *fm_lint_main_popover(fm_doc *d);

/* Set while the lint or the stylist draws offscreen: the host then draws
 * app views as calm placeholders, with no side effects. */
extern bool fm_lint_offscreen;

/* render.c calls this, when set, for each run of text it is about to draw. */
extern void (*fm_text_probe)(canvas *c, fm_node *t, rect r, color col, float px);
