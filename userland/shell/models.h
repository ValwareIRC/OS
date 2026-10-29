#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "controls.h"
#include "gfx.h"

/* AI models that run on this computer (models.c): the one that comes with
 * the system, and the ones that can be downloaded, with what this computer
 * can manage: memory, processors, free disk. */

typedef struct model_info {
    const char *id, *name, *by, *blurb, *url;
    uint64_t bytes;
    bool builtin; /* comes in the boot image */
} model_info;

int models_count(void);
const model_info *models_get(int i);

/* Where the chosen model's file is (the built-in one when none is chosen
 * or the chosen one is gone), and its catalog entry. */
const char *models_selected_path(void);
const model_info *models_selected(void);
/* How long a conversation (tokens) the selected model can keep in this computer's memory. */
int models_context(void);
/* Choose a model (by catalog index); the mind restarts with it if running. */
void models_select(int i);

/* Once per frame: downloads and the online check move along. */
void models_poll(void);

/* The list, with what fits, what is downloaded, and buttons to download,
 * use and remove (onboarding and Settings both draw it). `on_use` is called
 * when a model is chosen. Returns the height used. */
int models_list(rect c, int y, const palette *p, font *f, font *small, font *head, void (*on_use)(int i));
/* One line about this computer: "7 GB of memory, 4 processors, 29 GB free". */
void models_specs_text(char *out, int n);
