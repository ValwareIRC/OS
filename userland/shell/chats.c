/* Their conversations with the companion, as in every AI app: the one open, and the ones before it. There was
 * only ever one, kept forever, and nothing to clear it with: to take a video, Valerie had to ask the companion to
 * find a way, and there was none.
 *
 * The one open is STATE_DIR/companion.txt, as it always was (main.c reads it at the start, the window writes it
 * after anything new); the ones before it are STATE_DIR/chats/ID.txt, in the same form. Once the open one has
 * been kept, its ID is in STATE_DIR/chat-current, and it is written there too, so the list shows it as it is.
 * Notes to the companion (hidden) are never kept, here or there. What it remembers about them (memory.txt,
 * portrait.txt) is the same in every chat. */
#include "chats.h"
#include "agent.h"
#include "app.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHATS_DIR STATE_DIR "/chats"
#define CURRENT_FILE STATE_DIR "/chat-current"
#define OPEN_FILE STATE_DIR "/companion.txt"

static char current[40]; /* the open one's ID: "" while it has never been kept */
static bool loaded;

static void load_current(void) {
    if (loaded)
        return;
    loaded = true;
    FILE *f = fopen(CURRENT_FILE, "r");
    if (f) {
        if (!fgets(current, sizeof current, f))
            current[0] = 0;
        fclose(f);
    }
    current[strcspn(current, "\r\n")] = 0;
    for (const char *p = current; *p; p++) /* (an ID is a file name: digits and dashes only) */
        if (!((*p >= '0' && *p <= '9') || *p == '-'))
            current[0] = 0;
}

static void save_current(void) {
    if (!current[0]) {
        unlink(CURRENT_FILE);
        return;
    }
    FILE *f = fopen(CURRENT_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", current);
        fclose(f);
    }
}

static bool id_ok(const char *id) {
    if (!id || !id[0] || strlen(id) >= sizeof current)
        return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '-'))
            return false;
    return true;
}

static void path_of(const char *id, char *out, size_t n) { snprintf(out, n, CHATS_DIR "/%s.txt", id); }

/* Anything said in it they'd see (not only notes to the companion). */
static bool has_words(const struct conversation *c) {
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->msgs[i].role, "hidden") && c->msgs[i].text && c->msgs[i].text[strspn(c->msgs[i].text, " \t\r\n")])
            return true;
    return false;
}

/* The open one under its ID (a new one gets one now: the time it was kept, unique). */
static void keep_open(void) {
    load_current();
    if (!has_words(&A.companion))
        return;
    mkdir(CHATS_DIR, 0700);
    if (!current[0]) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char base[24], p[256];
        strftime(base, sizeof base, "%Y%m%d-%H%M%S", &tm);
        snprintf(current, sizeof current, "%s", base);
        path_of(current, p, sizeof p);
        for (int k = 2; access(p, F_OK) == 0 && k < 100; k++) {
            snprintf(current, sizeof current, "%s-%d", base, k);
            path_of(current, p, sizeof p);
        }
        save_current();
    }
    char p[256];
    path_of(current, p, sizeof p);
    convo_save(&A.companion, p);
}

void chats_saved(void) {
    load_current();
    if (current[0])
        keep_open();
}

bool chats_can_switch(void) { return !A.companion.pending && !agent_busy(); }

static void start_empty(void) {
    convo_clear(&A.companion);
    unlink(OPEN_FILE);
    current[0] = 0;
    save_current();
    A.chat.len = 0;
    A.chat.buf[0] = 0;
}

void chats_new(void) {
    keep_open();
    start_empty();
    LOG("chats: a new one\n");
}

bool chats_open(const char *id) {
    load_current();
    if (!id_ok(id))
        return false;
    if (!strcmp(id, current))
        return true;
    char p[256];
    path_of(id, p, sizeof p);
    if (access(p, R_OK) != 0)
        return false;
    keep_open();
    convo_clear(&A.companion);
    convo_load(&A.companion, p);
    snprintf(current, sizeof current, "%s", id);
    save_current();
    convo_save(&A.companion, OPEN_FILE);
    struct stat st;
    char when[40] = "earlier";
    if (stat(p, &st) == 0)
        chats_when(st.st_mtime, when, sizeof when);
    char note[400];
    /* (it took the screen to be as a conversation had left it, once: what was open in it isn't now) */
    snprintf(note, sizeof note,
             "(They opened this earlier conversation again; its last words were from %s. It goes on from here. "
             "Whatever was open on the screen then isn't now: what is open is only what you are told about the "
             "screen.)",
             when);
    convo_add(&A.companion, "hidden", note);
    LOG("chats: opened %s\n", id);
    return true;
}

void chats_delete(const char *id) {
    load_current();
    bool open = !id || !id[0] || !strcmp(id, current);
    if (open && current[0]) {
        char p[256];
        path_of(current, p, sizeof p);
        unlink(p);
    } else if (!open && id_ok(id)) {
        char p[256];
        path_of(id, p, sizeof p);
        unlink(p);
    }
    if (open)
        start_empty();
    LOG("chats: deleted %s\n", open ? "the one open" : id);
}

void chats_delete_all(void) {
    DIR *d = opendir(CHATS_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".txt")) {
            char p[512];
            snprintf(p, sizeof p, CHATS_DIR "/%s", e->d_name);
            unlink(p);
        }
    }
    if (d)
        closedir(d);
    load_current();
    start_empty();
    LOG("chats: all deleted\n");
}

/* Its first words, theirs (or its, if they said nothing yet), on one line. */
static void title_of_file(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    static char line[4096];
    char first_other[96] = "";
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        char *t = tab + 1;
        t[strcspn(t, "\r\n")] = 0;
        for (char *q; (q = strstr(t, "\\n"));) /* (the file's line breaks: spaces in a title) */
            q[0] = ' ', memmove(q + 1, q + 2, strlen(q + 2) + 1);
        t += strspn(t, " ");
        if (!*t)
            continue;
        if (!strcmp(line, "user")) {
            snprintf(out, n, "%s", t);
            break;
        }
        if (!first_other[0])
            snprintf(first_other, sizeof first_other, "%s", t);
    }
    fclose(f);
    if (!out[0])
        snprintf(out, n, "%s", first_other[0] ? first_other : "A conversation");
}

static void title_of_open(char *out, size_t n) {
    const struct conversation *c = &A.companion;
    out[0] = 0;
    for (int i = 0; i < c->n && !out[0]; i++)
        if (!strcmp(c->msgs[i].role, "user") && c->msgs[i].text[strspn(c->msgs[i].text, " \t\r\n")])
            snprintf(out, n, "%s", c->msgs[i].text + strspn(c->msgs[i].text, " \t\r\n"));
    for (char *p = out; *p; p++)
        if (*p == '\n' || *p == '\t')
            *p = ' ';
    if (!out[0])
        snprintf(out, n, "This chat");
}

static int newest_first(const void *a, const void *b) {
    const struct chat_entry *x = a, *y = b;
    return x->when < y->when ? 1 : x->when > y->when ? -1 : 0;
}

int chats_list(struct chat_entry *out, int max) {
    load_current();
    int n = 0;
    if (max > 0 && has_words(&A.companion)) { /* the one open, first: as it is now */
        struct chat_entry *e = &out[n++];
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", current);
        title_of_open(e->title, sizeof e->title);
        e->when = time(NULL);
        e->open = true;
    }
    DIR *d = opendir(CHATS_DIR);
    struct dirent *de;
    while (d && (de = readdir(d)) && n < max) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".txt"))
            continue;
        char id[40];
        snprintf(id, sizeof id, "%.*s", (int)(len - 4), de->d_name);
        if (!id_ok(id) || (current[0] && !strcmp(id, current)))
            continue;
        char p[256];
        path_of(id, p, sizeof p);
        struct stat st;
        if (stat(p, &st) != 0)
            continue;
        struct chat_entry *e = &out[n++];
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", id);
        title_of_file(p, e->title, sizeof e->title);
        e->when = st.st_mtime;
    }
    if (d)
        closedir(d);
    qsort(out, (size_t)n, sizeof *out, newest_first);
    return n;
}

void chats_when(time_t t, char *out, int n) {
    time_t now = time(NULL);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    long days = (long)((now - t) / 86400);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        strftime(out, (size_t)n, "Today %H:%M", &a);
    else if (days < 2 && (b.tm_yday - a.tm_yday == 1 || (b.tm_yday == 0 && a.tm_mon == 11 && a.tm_mday == 31)))
        snprintf(out, (size_t)n, "Yesterday");
    else if (days < 7)
        strftime(out, (size_t)n, "%A", &a);
    else if (a.tm_year == b.tm_year)
        strftime(out, (size_t)n, "%e %b", &a);
    else
        strftime(out, (size_t)n, "%e %b %Y", &a);
    if (out[0] == ' ')
        memmove(out, out + 1, strlen(out));
}
