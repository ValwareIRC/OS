#pragma once
#include "menufill.h"
#include <stdbool.h>

/* What right-clicking can offer, and doing it (menu_actions.c): what was clicked (an image, a link, words, a file,
 * an app, a window, the desktop), the actions offered everywhere, their own actions and categories (menus.conf),
 * and how often each is chosen. */

#define MENU_RUN 400

typedef struct {
    char image[600];  /* a picture under the pointer (its file) */
    char link[600];   /* a link under it (a web address or a file) */
    char text[1024];  /* the words selected, or those of the thing clicked (a message) */
    bool text_selected; /* (the words are a selection: search, look up, ask about them) */
    char file[600];   /* a file (Files) */
} menu_ctx;

/* What was clicked, for what follows: ctx_menu_open, then this, then the thing's own actions. */
void menu_context(const menu_ctx *c);
/* Before it is made: what the thing clicked offers by what it is (a picture, a link, words), what is offered
 * everywhere, their own actions, categories and priorities, how often each was chosen. */
void menu_complete(mf_spec *s, char (*run)[MENU_RUN]);
/* What choosing it does, for those this file makes; false if it isn't one of them. */
bool menu_do(const char *run);
/* It was chosen: counted (what is chosen most comes first where a design asks for it). */
void menu_chosen(const char *id);
/* menus.conf, for the companion's reference: where it is and how it is written. */
const char *menu_conf_help(void);
/* Their "keep:" lines (what they want of every design's menus), for the companion when it makes them. */
const char *menu_conf_keep(void);
