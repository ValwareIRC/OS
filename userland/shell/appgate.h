#pragma once
/* What runs on this computer, and what it may do (appgate.c).
 *
 * Every Facet app passes the gate before it runs: its capabilities are worked out from its code (fx/caps.h),
 * compared with what the person allowed it before (kept in /etc/os/app-grants, where no app can reach), and
 * anything new or never decided is put to them in a dialog the system draws: what it can do in plain words,
 * required and optional, what it already had and what is new (an update waits until they decide), the rules'
 * warnings and a second opinion from small models on this computer. Nothing runs before they allow it.
 *
 * Warnings and the second opinion can be turned off, only from Settings > Security, by them: a blunt warning, a
 * phrase to type and a wait first; one app's warnings can be quietened instead. The permissions are always
 * shown and always enforced. */
#include <stdbool.h>
#include "fx/caps.h"

/* Admits the app at path (parsed and checked): true with its grants filled to run with; false if it may not
 * run (refused by them, or reaching for what no app may). May show the dialog (and wait for them). */
bool appgate_admit(const char *path, const fx_app *app, const char *src, fx_grants *out);

/* ---- the security settings (pure parts, tested on the host: tests/security_ui_test.c) ---- */

#define SECURITY_PHRASE "I understand this puts my computer at risk"
#define SECURITY_WAIT_MS 10000

/* warnings and the second opinion: on unless they turned them off */
bool security_warnings_on(void);
bool security_app_quiet(const char *path);
void security_set_app_quiet(const char *path, bool quiet);
/* Turning them off: only with the phrase typed exactly and the wait over since the dialog opened. */
bool security_can_turn_off(const char *typed, unsigned long long opened_ms, unsigned long long now_ms);
bool security_turn_off(const char *typed, unsigned long long opened_ms, unsigned long long now_ms);
void security_turn_on(void);
/* where it all is kept (tests point it elsewhere) */
void security_set_dir(const char *dir);

/* an app's grants, kept in DIR/app-grants (the first line names its file: never another's) */
void security_grants_file(const char *app, char *out, size_t n);
bool security_load_grants(const char *app, fx_grants *g);
bool security_save_grants(const char *app, const fx_grants *g);
void security_forget_grants(const char *app);
int security_list_apps(char apps[][300], int max);

/* The Settings page (only ever reached by clicking it there). */
int security_page(int x, int y, int w, int h); /* the height it used */
