#pragma once
#include <stdbool.h>

/* The companion API (docs/companion-api.md): apps on this computer use the
 * companion's AI, identity, face, voice and memory through a socket of
 * their own, $XDG_RUNTIME_DIR/app/<app-id>/os-companion.sock, which
 * Flatpak mounts into the app's sandbox (appapi.c). */
void appapi_init(void);
int appapi_fds(int *out, int max);
void appapi_poll(void);

/* What the desktop companion just did, for the apps following its face
 * (presence.c): a gesture or mood, where it looks (-1.2..1.2). */
void appapi_avatar_event(const char *kind, const char *name);
void appapi_gaze(float x, float y);

/* The "Prism wants to use your AI" prompt: it takes the pointer and keys
 * while it's up. take_input at the start of a desktop frame, draw at its end. */
bool appapi_modal(void);
void appapi_take_input(void);
void appapi_draw(void);

/* Settings > Apps: the apps that asked, and what each may use. */
int appapi_app_count(void);
const char *appapi_app_label(int i);
/* -1 not asked yet, 0 refused, 1 allowed */
int appapi_app_allows(int i, bool memory);
void appapi_app_set(int i, bool memory, bool allow);
