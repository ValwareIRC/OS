#pragma once
#include "fm/fm.h"
#include <stdbool.h>

/* A right-click menu, made (menufill.c): what the thing clicked offers, and what is offered everywhere, put into the
 * design's own <menu> for that kind (or a ring of them, before the design has one). The design decides what a menu
 * is; this only knows the actions, the categories they are in, and the rules: each action once, in its priority
 * category when that is in the menu. */

#define MF_MAX 64
#define MF_CATS 48
enum { MF_DANGER = 1, MF_OFF = 2, MF_ON = 4, MF_QUIET = 8, MF_SEP = 16, MF_GLOBAL = 32 };

typedef struct {
    char id[40];       /* what it does, for a design's do="ID" and take="ID": open, copy-image, close... */
    char label[96];    /* its real name */
    char icon[24];     /* an icon name, for a design that shows icons */
    char desc[160];    /* a line on what it does (a tooltip, read aloud) */
    char keys[24];     /* its shortcut, if it has one */
    char why_off[120]; /* why it can't be done now (with MF_OFF) */
    int flags;         /* MF_*: removes something, can't now, chosen, the small one, everywhere */
    int uses;          /* how often they have chosen it (learning: @frequent, order="use") */
    char letter;       /* picked by this key, in a menu picked by letters (set by mf_build) */
    char groups[200];  /* its categories (set by mf_build) */
} mf_action;

typedef struct {
    char name[32];
    char take[400];   /* action ids and categories, space-separated */
    char prefer[400]; /* the actions whose one priority category this is */
    bool global;      /* (its actions offered whatever was clicked: their makers mark them MF_GLOBAL) */
} mf_category;

typedef struct {
    char kind[16];          /* app window desktop text file image link */
    int x, y;               /* where they clicked */
    int tx, ty, tw, th;     /* where the thing clicked is (tw 0: not known) */
    int n;
    mf_action a[MF_MAX];
    char note[160];
    int ncats;              /* the person's and companion's own categories and priorities (menus.conf), */
    mf_category cats[MF_CATS]; /* beside the system's and the design's */
} mf_spec;

mf_action *mf_add(mf_spec *s, const char *id, const char *label, int flags);
bool mf_has(const mf_spec *s, const char *id);
/* The categories (the system's, a menus.conf's and the design's own) action i of s is in, space-separated. */
void mf_groups_of(const mf_spec *s, int i, char *out, size_t n);
/* Made in d's desktop, open: the design's <menu for="KIND"> filled, or a ring; id "_menu", never kept. */
fm_node *mf_build(fm_doc *d, mf_spec *s);
/* The design's own menu for a kind (<menu for="...">), or NULL. */
fm_node *mf_template(fm_doc *d, const char *kind);
/* What is typed while it is up: the actions that don't match are hidden (filtered="out"), the words shown in its
 * <filter> if it has one. Returns how many match. */
int mf_filter(fm_doc *d, fm_node *menu, const mf_spec *s, const char *typed);
/* How it is picked, as the design says (pick="flick hold letters type"): true if it says `how`. */
bool mf_picks(const fm_node *menu, const char *how);
/* The built-in categories, for a reference: "clipboard: cut copy paste; ..." */
const char *mf_builtin_categories(void);
