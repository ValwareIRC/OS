#pragma once

/* The companion looking things up on the web (web.c). `requests` is one per
 * line, "search<TAB>words" or "read<TAB>https://...". Waits on the network:
 * call it in a child process (a job). Returns malloc'd plain text. */
char *web_lookup(const char *requests);
