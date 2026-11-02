#pragma once
#include <stdbool.h>

/* The companion's look (body.c): it brings it up in conversation once their
 * space is made; pictures it found online to choose from take the pointer and
 * keys while they're up: take_input at the start of the frame, draw at its end. */
void body_take_input(void);
void body_draw(void);
bool body_asking(void);
/* .vrm files in their home and its usual folders (paths into out). */
int body_find_vrms(char out[][300], int max);
/* Opens the conversation about how it looks (first: right after onboarding). */
void body_talk_about_it(bool first);
/* The companion's own look from somewhere: how is "picture" (a file or a
 * link), "model" (a .glb/.gltf file or link) or "find" (words: it searches
 * pictures online and shows them to choose from). */
void body_use(const char *how, const char *what);
