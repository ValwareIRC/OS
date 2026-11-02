/* The person's home: /home/<their account>. The desktop and everything it starts have it as $HOME (set by
 * init); init itself, and anything without $HOME, reads the account's name from /etc/os/account. Linked into
 * the desktop and into init (with store.c). */
#define _GNU_SOURCE
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "valerie": the account's name as kept (/etc/os/account), or "user" before they have told it */
static char name[64], h[256];

void home_forget(void) { name[0] = h[0] = 0; }

const char *account_name(void) {
    if (!name[0]) {
        FILE *f = fopen("/etc/os/account", "r");
        if (!f || !fgets(name, sizeof name, f) || !account_name_ok(strtok(name, "\r\n \t")))
            snprintf(name, sizeof name, "user");
        if (f)
            fclose(f);
    }
    return name;
}

/* a name for an account: a lowercase letter, then lowercase letters, digits, - or _, at most 31 */
bool account_name_ok(const char *s) {
    if (!s || !(*s >= 'a' && *s <= 'z'))
        return false;
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '-' || s[n] == '_') || n > 30)
            return false;
    return true;
}

const char *home_dir(void) {
    if (!h[0]) {
        const char *e = getenv("HOME");
        if (e && !strncmp(e, "/home/", 6) && e[6])
            snprintf(h, sizeof h, "%s", e);
        else
            snprintf(h, sizeof h, "/home/%s", account_name());
        size_t n = strlen(h);
        while (n > 1 && h[n - 1] == '/')
            h[--n] = 0;
    }
    return h;
}

const char *home_path(const char *rest) {
    static char b[8][1024];
    static int k;
    char *o = b[k++ & 7];
    snprintf(o, sizeof b[0], "%s%s", home_dir(), rest ? rest : "");
    return o;
}
