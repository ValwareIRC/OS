#pragma once

/* The companion looking things up on the web (web.c). `requests` is one per
 * line, "search<TAB>words" or "read<TAB>https://...". Waits on the network:
 * call it in a child process (a job). Returns malloc'd plain text. */
char *web_lookup(const char *requests);

#include <stdbool.h>
#include <stddef.h>
/* Fetches url (following redirects) into path; false if it failed or was
 * bigger than max_bytes. Waits on the network: call it in a job. */
bool web_download(const char *url, const char *path, size_t max_bytes);
/* Pictures for words: up to max (picture address, thumbnail address, title).
 * Waits on the network: call it in a job. Returns how many. */
int web_images(const char *q, char urls[][512], char thumbs[][512], char titles[][160], int max);
