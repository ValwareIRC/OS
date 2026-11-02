/* The security settings and the grants, kept where no app can reach (appgate.h). Pure file work and rules,
 * so they are tested on the host as they are in the OS (tests/security_ui_test.c). */
#define _GNU_SOURCE
#include "appgate.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static char dir[256] = "/etc/os";

void security_set_dir(const char *d) { snprintf(dir, sizeof dir, "%s", d); }

static void conf_path(char *out, size_t n) { snprintf(out, n, "%s/security.conf", dir); }

/* the whole file (malloc'd, "" if none) */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return strdup("");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n > 0 ? (size_t)n + 1 : 1);
    size_t got = n > 0 ? fread(s, 1, (size_t)n, f) : 0;
    s[got] = 0;
    fclose(f);
    return s;
}

static bool has_line(const char *text, const char *line) {
    size_t l = strlen(line);
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == l && !strncmp(p, line, l))
            return true;
        p = e ? e + 1 : NULL;
    }
    return false;
}

/* the file with line added or taken out */
static void set_line(const char *line, bool present) {
    char path[300];
    conf_path(path, sizeof path);
    char *old = slurp(path);
    if (has_line(old, line) == present) {
        free(old);
        return;
    }
    mkdir(dir, 0755);
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(old);
        return;
    }
    size_t l = strlen(line);
    for (const char *p = old; *p;) {
        const char *e = strchr(p, '\n');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k && !(k == l && !strncmp(p, line, l)))
            fprintf(f, "%.*s\n", (int)k, p);
        p = e ? e + 1 : p + k;
    }
    if (present)
        fprintf(f, "%s\n", line);
    fclose(f);
    chmod(tmp, 0600); /* readable by the system alone */
    rename(tmp, path);
    free(old);
}

bool security_warnings_on(void) {
    char path[300];
    conf_path(path, sizeof path);
    char *s = slurp(path);
    bool off = has_line(s, "warnings off");
    free(s);
    return !off;
}

bool security_app_quiet(const char *app) {
    char path[300], line[600];
    conf_path(path, sizeof path);
    snprintf(line, sizeof line, "quiet %s", app);
    char *s = slurp(path);
    bool q = has_line(s, line);
    free(s);
    return q;
}

void security_set_app_quiet(const char *app, bool quiet) {
    char line[600];
    snprintf(line, sizeof line, "quiet %s", app);
    if (strchr(app, '\n'))
        return;
    set_line(line, quiet);
}

bool security_can_turn_off(const char *typed, unsigned long long opened_ms, unsigned long long now_ms) {
    if (!typed || now_ms < opened_ms || now_ms - opened_ms < SECURITY_WAIT_MS)
        return false;
    /* exactly the phrase (spaces at either end don't count; nothing else is forgiven) */
    while (*typed == ' ')
        typed++;
    size_t n = strlen(typed);
    while (n && typed[n - 1] == ' ')
        n--;
    return n == strlen(SECURITY_PHRASE) && !strncmp(typed, SECURITY_PHRASE, n);
}

bool security_turn_off(const char *typed, unsigned long long opened_ms, unsigned long long now_ms) {
    if (!security_can_turn_off(typed, opened_ms, now_ms))
        return false;
    set_line("warnings off", true);
    return true;
}

void security_turn_on(void) { set_line("warnings off", false); }

/* ---- grants, an app's file each ---- */

/* the app's own file name in app-grants: its path, every character not a letter, digit, '.' or '-' as '_',
 * with a hash so different paths never share one */
void security_grants_file(const char *app, char *out, size_t n) {
    char name[200];
    size_t k = 0;
    for (const char *p = app; *p && k < sizeof name - 1; p++)
        name[k++] = isalnum((unsigned char)*p) || *p == '.' || *p == '-' ? *p : '_';
    name[k] = 0;
    unsigned long h = 5381;
    for (const char *p = app; *p; p++)
        h = h * 33 + (unsigned char)*p;
    snprintf(out, n, "%s/app-grants/%.120s-%08lx", dir, name, h & 0xffffffffUL);
}

bool security_load_grants(const char *app, fx_grants *g) {
    char path[400];
    security_grants_file(app, path, sizeof path);
    memset(g, 0, sizeof *g);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    fclose(f);
    char *s = slurp(path);
    /* the first line names the app's file: a grants file for another path is never used */
    char first[600];
    snprintf(first, sizeof first, "app %s\n", app);
    bool mine = !strncmp(s, first, strlen(first));
    if (mine)
        fx_grants_parse(g, s + strlen(first));
    free(s);
    return mine;
}

bool security_save_grants(const char *app, const fx_grants *g) {
    char path[400], tmp[420], d[300];
    security_grants_file(app, path, sizeof path);
    snprintf(d, sizeof d, "%s/app-grants", dir);
    mkdir(dir, 0755);
    mkdir(d, 0700);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;
    char *t = fx_grants_text(g);
    fprintf(f, "app %s\n%s", app, t);
    free(t);
    fclose(f);
    chmod(tmp, 0600);
    return rename(tmp, path) == 0;
}

void security_forget_grants(const char *app) {
    char path[400];
    security_grants_file(app, path, sizeof path);
    remove(path);
}

/* every app with grants: its path into apps[i] (up to max) */
#include <dirent.h>
int security_list_apps(char apps[][300], int max) {
    char d[300];
    snprintf(d, sizeof d, "%s/app-grants", dir);
    DIR *dd = opendir(d);
    int n = 0;
    struct dirent *e;
    while (dd && (e = readdir(dd)) && n < max) {
        if (e->d_name[0] == '.')
            continue;
        char p[600];
        snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        char line[400];
        if (fgets(line, sizeof line, f) && !strncmp(line, "app ", 4) && strlen(line) < 300) { /* never a cut-short path */
            line[strcspn(line, "\n")] = 0;
            snprintf(apps[n++], 300, "%s", line + 4);
        }
        fclose(f);
    }
    if (dd)
        closedir(dd);
    return n;
}
