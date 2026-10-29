#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the OS remembers across power-offs, kept on the disk (store.c). */
void store_set_device(const char *path); /* default /dev/sda */
bool store_available(void);
/* Puts /etc/os and /root back as they were at the last save. Returns the
 * number of files restored, 0 for a new disk, -1 if there's no usable disk. */
int store_restore(void);
bool store_save(void);
bool store_wipe(void);
void store_set_roots(const char *const *roots); /* for tests: which folders are kept */
