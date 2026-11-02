#pragma once
/* The desktop's few root errands (priv.c): the desktop runs as the person; init does these for it through a
 * private channel only the desktop holds (userland/init/session.inc). Only from the main process: nothing it
 * forks or starts ever holds the channel.
 *
 * Without the channel (an older init, a test) the desktop does them itself, as it did before. */
#include <stdbool.h>

bool priv_available(void);
/* Asks for an errand ("save", "restart", "power-off", "install ID", "uninstall ID", "graphics", "wipe",
 * "factory-reset"): its number, or -1. */
int priv_send(const char *errand);
/* The answer to errand id, when it has come: true with *ok and *text (malloc'd, may be empty). */
bool priv_take(int id, bool *ok, char **text);
/* Asks and waits (up to timeout_ms): true if done and ok. For quick errands only (power). */
bool priv_call(const char *errand, int timeout_ms);
/* Whether the disk is kept (init says so), and how many files came back from it at boot (-1: no disk). */
bool priv_store_kept(void);
int priv_store_restored(void);
