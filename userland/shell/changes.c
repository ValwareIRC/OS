#define _GNU_SOURCE
#include "changes.h"
#include "app.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* What changed, kept so it can be put back: see changes.h. */

#define DIR_ STATE_DIR "/changes"
#define LIST DIR_ "/list.txt"
#define KEEP 30

static struct change {
    time_t at;
    char kind;      /* 's' a setting, 'd' the design */
    char key[32];   /* the setting; the design: the file it was kept in */
    char old[96], now[96];
} ch[KEEP]; /* newest first */
static int n;
static bool loaded;
static bool putting_back; /* (a put-back is a change too, but not the one to offer back next) */

static void clean(char *s) {
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

static void load(void) {
    loaded = true;
    n = 0;
    FILE *f = fopen(LIST, "r");
    char line[400];
    while (f && n < KEEP && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        struct change c = {0};
        char *p = line, *field[5];
        for (int i = 0; i < 5; i++) {
            field[i] = p;
            p = strchr(p, '\t');
            if (p)
                *p++ = 0;
            else
                p = (char *)"";
        }
        c.at = (time_t)atoll(field[0]);
        c.kind = field[1][0];
        snprintf(c.key, sizeof c.key, "%s", field[2]);
        snprintf(c.old, sizeof c.old, "%s", field[3]);
        snprintf(c.now, sizeof c.now, "%s", field[4]);
        if (c.at && (c.kind == 's' || c.kind == 'd'))
            ch[n++] = c;
    }
    if (f)
        fclose(f);
}

static void save(void) {
    mkdir(DIR_, 0700);
    FILE *f = fopen(LIST ".new", "w");
    if (!f)
        return;
    for (int i = 0; i < n; i++)
        fprintf(f, "%lld\t%c\t%s\t%s\t%s\n", (long long)ch[i].at, ch[i].kind, ch[i].key, ch[i].old, ch[i].now);
    if (fclose(f) == 0)
        rename(LIST ".new", LIST);
}

static void push(const struct change *c) {
    if (n == KEEP) { /* the oldest goes (with its design file) */
        if (ch[KEEP - 1].kind == 'd') {
            char p[96];
            snprintf(p, sizeof p, DIR_ "/%s", ch[KEEP - 1].key);
            unlink(p);
        }
        n--;
    }
    memmove(ch + 1, ch, sizeof ch[0] * (size_t)n);
    ch[0] = *c;
    n++;
    save();
}

void changes_note_setting(const char *key, const char *old_value, const char *new_value) {
    size_t kl = strlen(key);
    if ((kl > 4 && !strcmp(key + kl - 4, "_key")) || (kl > 6 && !strcmp(key + kl - 6, "_asked")) ||
        !strncmp(key, "family", 6))
        return; /* keys to their accounts are never written anywhere else; what was asked once isn't a choice */
    if (!loaded)
        load();
    time_t t = time(NULL);
    /* a slider dragged, a switch tried twice: one change, from where it was */
    if (n && ch[0].kind == 's' && !strcmp(ch[0].key, key) && t - ch[0].at < 20 && !putting_back) {
        snprintf(ch[0].now, sizeof ch[0].now, "%s", new_value ? new_value : "");
        ch[0].at = t;
        if (!strcmp(ch[0].now, ch[0].old)) { /* back where it was: nothing changed */
            memmove(ch, ch + 1, sizeof ch[0] * (size_t)--n);
        }
        save();
        return;
    }
    struct change c = {.at = t, .kind = 's'};
    snprintf(c.key, sizeof c.key, "%s", key);
    snprintf(c.old, sizeof c.old, "%s", old_value ? old_value : "");
    snprintf(c.now, sizeof c.now, "%s", new_value ? new_value : "");
    clean(c.old), clean(c.now);
    push(&c);
}

void changes_note_design(const char *old_text) {
    if (!old_text || !old_text[0])
        return;
    if (!loaded)
        load();
    time_t t = time(NULL);
    /* many edits in a row (a redesign's steps, a slider in Appearance): one change, from how it looked before */
    if (n && ch[0].kind == 'd' && t - ch[0].at < 60 && !putting_back) {
        ch[0].at = t;
        save();
        return;
    }
    mkdir(DIR_, 0700);
    struct change c = {.at = t, .kind = 'd'};
    snprintf(c.key, sizeof c.key, "design-%lld.xml", (long long)t);
    char p[96];
    snprintf(p, sizeof p, DIR_ "/%s", c.key);
    FILE *f = fopen(p, "w");
    if (!f)
        return;
    bool ok = fputs(old_text, f) >= 0;
    if (fclose(f) != 0 || !ok) {
        unlink(p);
        return;
    }
    snprintf(c.now, sizeof c.now, "%s", putting_back ? "put back" : "");
    push(&c);
}

static const char *ago(time_t at, char *b, size_t n) {
    long s = (long)(time(NULL) - at);
    if (s < 90)
        snprintf(b, n, "just now");
    else if (s < 3600)
        snprintf(b, n, "%ld minutes ago", s / 60);
    else if (s < 86400)
        snprintf(b, n, "%ld hour%s ago", s / 3600, s / 3600 == 1 ? "" : "s");
    else
        snprintf(b, n, "%ld day%s ago", s / 86400, s / 86400 == 1 ? "" : "s");
    return b;
}

void changes_prompt(char *out, size_t len) {
    out[0] = 0;
    if (!loaded)
        load();
    size_t o = 0;
    int shown = 0;
    for (int i = 0; i < n && shown < 8 && o < len; i++) {
        if (time(NULL) - ch[i].at > 3 * 86400)
            break;
        char when[32];
        ago(ch[i].at, when, sizeof when);
        if (!shown)
            o += (size_t)snprintf(out + o, len - o,
                                  "\n\nRecent changes on this computer, newest first (each can be put back):\n");
        if (o >= len)
            break;
        if (ch[i].kind == 'd')
            o += (size_t)snprintf(out + o, len - o, "%d. %s: how the desktop looks changed%s\n", i + 1, when,
                                  ch[i].now[0] ? " (a change put back)" : "");
        else
            o += (size_t)snprintf(out + o, len - o, "%d. %s: the setting %s went from \"%s\" to \"%s\"\n", i + 1, when,
                                  ch[i].key, ch[i].old, ch[i].now);
        shown++;
    }
    if (shown && o < len)
        snprintf(out + o, len - o,
                 "If they seem upset, frustrated or stuck, or say something broke or looks wrong, and one of these "
                 "came just before, offer in a sentence to put it back (say which, plainly). Only on their yes, end "
                 "with [[putback]] for the newest or [[putback: N]] for the N-th. Never put anything back unasked.");
}

bool desktop_put_back_design(const char *path);

bool changes_put_back(int k, char *said, size_t len) {
    if (!loaded)
        load();
    said[0] = 0;
    if (k < 1 || k > n)
        return false;
    struct change c = ch[k - 1];
    bool ok = false;
    putting_back = true;
    if (c.kind == 's') {
        settings_set(c.key, c.old);
        settings_boot(); /* (what the settings do, done again: the screen, the keyboard, the mouse, the sound) */
        snprintf(said, len, "the setting %s, back to \"%s\"", c.key, c.old);
        ok = true;
    } else {
        char p[96];
        snprintf(p, sizeof p, DIR_ "/%s", c.key);
        char when[32];
        ok = desktop_put_back_design(p);
        snprintf(said, len, "how the desktop looked %s", ago(c.at, when, sizeof when));
    }
    putting_back = false;
    LOG("changes: put back %d (%s): %s\n", k, said, ok ? "done" : "FAILED");
    return ok;
}
