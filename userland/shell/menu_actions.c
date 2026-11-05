/* What right-clicking can offer, and doing it. The thing clicked offers its own actions (desktop.c: an app, a window,
 * the desktop; files.c: a file; textmenu.c: a text field); this adds what follows from what it is (a picture: open
 * it, copy it, save it, make it the wallpaper; a link: open it, copy it; words: copy them, search for them, look
 * them up, ask the companion about them), the actions offered everywhere (the system's "system" category, and
 * what the person and the companion put everywhere), their own actions, categories and priorities (menus.conf),
 * and how often each has been chosen. What a menu looks like and where things go in it is the design's
 * (menufill.c). */
#define _GNU_SOURCE
#include "menu_actions.h"
#include "app.h"
#include "capture.h"
#include "files.h"
#include "launcher.h"
#include "notes.h"
#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONF STATE_DIR "/menus.conf"
#define USES STATE_DIR "/menu-uses"

bool desktop_set_wallpaper(const char *path);
void desktop_open_link(const char *url);
bool desktop_open_with(const char *app_id, const char *what);
int desktop_apps_for(const char *mime, char (*ids)[96], char (*names)[48], int max);
void companion_ask_about_text(const char *what);
void files_view(const char *path);
void desktop_save_ui(void);

static menu_ctx C;

void menu_context(const menu_ctx *c) {
    if (c)
        C = *c;
    else
        memset(&C, 0, sizeof C);
}

/* ---- the actions that need nothing clicked: any of them may be offered everywhere ---- */

static const struct {
    const char *id, *label, *icon, *desc, *run;
} anywhere[] = {
    {"take-screenshot", "Take a screenshot", "camera", "A picture of the whole screen, saved in Pictures", "screenshot"},
    {"open-terminal", "Open Terminal", "terminal", "A terminal", "open:terminal"},
    {"settings", "Settings", "gear", "Settings", "do:open:settings"},
    {"redesign", "Redesign", "sparkle", "A new look for the desktop, made with you", "do:redesign"},
    {"new-note", "New note", "doc", "An empty note in Notes", "new-note"},
    {"change-wallpaper", "Change the wallpaper", "picture", "The wallpaper settings", "wallpaper-settings"},
    {"open-files", "Open Files", "folder", "Your files", "open:files"},
    {"open-companion", "Open the companion", "spark", "The conversation", "open:companion"},
};
#define NANYWHERE (sizeof anywhere / sizeof anywhere[0])

static mf_action *add(mf_spec *s, char (*run)[MENU_RUN], const char *id, const char *label, const char *icon,
                      const char *desc, const char *what, int flags) {
    int before = s->n;
    mf_action *a = mf_add(s, id, label, flags);
    if (!a)
        return NULL;
    if (s->n > before) { /* (new: offered once, whoever offers it first) */
        snprintf(a->icon, sizeof a->icon, "%s", icon ? icon : "");
        snprintf(a->desc, sizeof a->desc, "%s", desc ? desc : "");
        snprintf(run[s->n - 1], MENU_RUN, "%s", what);
    } else {
        a->flags |= flags & MF_GLOBAL;
    }
    return a;
}

static bool add_anywhere(mf_spec *s, char (*run)[MENU_RUN], const char *id, int flags) {
    for (size_t i = 0; i < NANYWHERE; i++)
        if (!strcasecmp(anywhere[i].id, id))
            return add(s, run, anywhere[i].id, anywhere[i].label, anywhere[i].icon, anywhere[i].desc,
                       anywhere[i].run, flags) != NULL;
    return false;
}

/* ---- menus.conf: theirs, and the companion's (it reads and writes it as a file) ----
 *   category pictures: copy-image save-image set-as-wallpaper      (a category: action ids, other categories)
 *   category shortcuts everywhere: open-terminal take-screenshot   (its actions offered whatever is clicked)
 *   priority copy-image: pictures                                  (its one place, when that is in the menu)
 *   everywhere take-screenshot                                     (one action offered whatever is clicked)
 *   nowhere-else open-terminal                                     (one the system offers everywhere: not)
 *   action downloads everywhere "Downloads": file:~/Downloads      (their own: open:APP file:PATH url:ADDRESS
 *   action weather for desktop app "Ask about the weather": ask:What's the weather like?   cmd:COMMAND ask:WORDS)
 *   keep: copy, cut and paste as a grid, wherever the menu goes    (said once, kept: every design's menus follow it)
 */
static char *conf_text(void) {
    FILE *f = fopen(CONF, "r");
    if (!f)
        return NULL;
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    size_t k = strspn(s, " \t");
    memmove(s, s + k, strlen(s + k) + 1);
}

static bool for_this_kind(const char *kinds, const char *kind) {
    if (!kinds || !*kinds)
        return true;
    char w[200];
    snprintf(w, sizeof w, "%s", kinds);
    for (char *save = NULL, *t = strtok_r(w, " ,", &save); t; t = strtok_r(NULL, " ,", &save))
        if (!strcasecmp(t, kind) || !strcasecmp(t, "everywhere"))
            return true;
    return false;
}

static void read_conf(mf_spec *s, char (*run)[MENU_RUN], bool *not_everywhere, int nnot) {
    char *text = conf_text();
    if (!text)
        return;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char l[700];
        snprintf(l, sizeof l, "%s", line);
        trim(l);
        if (!l[0] || l[0] == '#')
            continue;
        if (!strncasecmp(l, "category ", 9)) {
            char *colon = strchr(l, ':');
            if (!colon || s->ncats >= MF_CATS)
                continue;
            *colon = 0;
            char head[200];
            snprintf(head, sizeof head, "%s", l + 9);
            trim(head);
            mf_category *c = &s->cats[s->ncats++];
            memset(c, 0, sizeof *c);
            char *sp = strchr(head, ' ');
            if (sp) {
                *sp = 0;
                c->global = strcasestr(sp + 1, "everywhere") != NULL;
            }
            snprintf(c->name, sizeof c->name, "%s", head);
            snprintf(c->take, sizeof c->take, "%s", colon + 1);
            trim(c->take);
        } else if (!strncasecmp(l, "priority ", 9)) {
            char *colon = strchr(l, ':');
            if (!colon)
                continue;
            *colon = 0;
            char id[48], cat[48];
            snprintf(id, sizeof id, "%s", l + 9);
            snprintf(cat, sizeof cat, "%s", colon + 1);
            trim(id), trim(cat);
            mf_category *c = NULL;
            for (int k = 0; k < s->ncats; k++)
                if (!strcasecmp(s->cats[k].name, cat))
                    c = &s->cats[k];
            if (!c && s->ncats < MF_CATS) { /* (a priority for one of the system's: kept beside it) */
                c = &s->cats[s->ncats++];
                memset(c, 0, sizeof *c);
                snprintf(c->name, sizeof c->name, "%s", cat);
                snprintf(c->take, sizeof c->take, "%s", cat); /* (itself: the system's, by name... see below) */
                c->take[0] = 0;
            }
            if (c) {
                size_t o = strlen(c->prefer);
                snprintf(c->prefer + o, sizeof c->prefer - o, "%s%s", o ? " " : "", id);
            }
        } else if (!strncasecmp(l, "everywhere ", 11)) {
            char id[48];
            snprintf(id, sizeof id, "%s", l + 11);
            trim(id);
            add_anywhere(s, run, id, MF_GLOBAL);
        } else if (!strncasecmp(l, "nowhere-else ", 13)) {
            char id[48];
            snprintf(id, sizeof id, "%s", l + 13);
            trim(id);
            for (size_t i = 0; i < NANYWHERE && (int)i < nnot; i++)
                if (!strcasecmp(anywhere[i].id, id))
                    not_everywhere[i] = true;
        } else if (!strncasecmp(l, "action ", 7)) {
            /* action ID [everywhere | for KIND...] "Name": WHAT */
            char *q1 = strchr(l, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL, *colon = q2 ? strchr(q2, ':') : NULL;
            if (!q1 || !q2 || !colon)
                continue;
            *q1 = 0, *q2 = 0;
            char head[200], id[48] = "", kinds[150] = "";
            snprintf(head, sizeof head, "%s", l + 7);
            trim(head);
            char *sp = strchr(head, ' ');
            if (sp) {
                *sp = 0;
                snprintf(kinds, sizeof kinds, "%s", sp + 1);
                trim(kinds);
                if (!strncasecmp(kinds, "for ", 4))
                    memmove(kinds, kinds + 4, strlen(kinds + 4) + 1);
            }
            snprintf(id, sizeof id, "%s", head);
            char what[MENU_RUN];
            snprintf(what, sizeof what, "%s", colon + 1);
            trim(what);
            bool everywhere = strcasestr(kinds, "everywhere") != NULL;
            if (!id[0] || !what[0] || !for_this_kind(kinds, s->kind))
                continue;
            char run2[MENU_RUN];
            snprintf(run2, sizeof run2, "own:%s", what);
            add(s, run, id, q1 + 1, "", what, run2, everywhere ? MF_GLOBAL : 0);
        }
    }
    /* a priority for a category of the system's: its own entry had no take (it is the system's, found by name) */
    for (int k = 0; k < s->ncats; k++)
        if (!s->cats[k].take[0]) {
            const char *b = mf_builtin_categories(); /* "name: ids; ..." */
            char pat[48];
            snprintf(pat, sizeof pat, "%s: ", s->cats[k].name);
            const char *p = strstr(b, pat);
            if (!p) {
                snprintf(pat, sizeof pat, "%s (everywhere): ", s->cats[k].name);
                p = strstr(b, pat);
            }
            if (p) {
                p += strlen(pat);
                size_t len = strcspn(p, ";");
                snprintf(s->cats[k].take, sizeof s->cats[k].take, "%.*s", (int)len, p);
            }
        }
}

/* "keep:" lines: what they asked for, for the companion when it makes a design's menus */
const char *menu_conf_keep(void) {
    static char out[2000];
    out[0] = 0;
    char *text = conf_text();
    if (!text)
        return out;
    size_t o = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char l[400];
        snprintf(l, sizeof l, "%s", line);
        trim(l);
        if (!strncasecmp(l, "keep:", 5) && o + strlen(l) + 4 < sizeof out)
            o += (size_t)snprintf(out + o, sizeof out - o, "- %s\n", l + 5 + strspn(l + 5, " "));
    }
    return out;
}

const char *menu_conf_help(void) {
    return STATE_DIR "/menus.conf: the right-click menus' actions and categories, theirs and yours (read and write "
           "it like any file), whatever the design. Lines: \"category NAME: ids and categories\" (\"category NAME "
           "everywhere: ...\": offered whatever is clicked); \"priority ID: CATEGORY\" (the one category where an "
           "action shows, when that one is in the menu: never twice); \"everywhere ID\" (one action offered whatever "
           "is clicked); \"nowhere-else ID\" (one the system offers everywhere: not); \"action ID [everywhere | for "
           "KIND...] \\\"Name\\\": open:APP | file:PATH | url:ADDRESS | cmd:COMMAND | ask:WORDS\" (their own); "
           "\"keep: WORDS\" (what they want of every design's menus, kept across redesigns). The system's categories "
           "(they may be used, regrouped or ignored; a category of the same name here replaces one): ";
}

/* ---- how often each is chosen ---- */

static int uses_of(const char *id) {
    FILE *f = fopen(USES, "r");
    char line[120], name[64];
    int n = 0, found = 0;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "%63s %d", name, &n) == 2 && !strcmp(name, id)) {
            found = n;
            break;
        }
    if (f)
        fclose(f);
    return found;
}

void menu_chosen(const char *id) {
    if (!id || !*id)
        return;
    char names[200][48];
    int counts[200], n = 0;
    FILE *f = fopen(USES, "r");
    char line[120];
    bool had = false;
    while (f && n < 200 && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%47s %d", names[n], &counts[n]) != 2)
            continue;
        if (!strcmp(names[n], id))
            counts[n]++, had = true;
        n++;
    }
    if (f)
        fclose(f);
    if (!had && n < 200) {
        snprintf(names[n], sizeof names[0], "%s", id);
        counts[n++] = 1;
    }
    f = fopen(USES, "w");
    for (int i = 0; f && i < n; i++)
        fprintf(f, "%s %d\n", names[i], counts[i]);
    if (f)
        fclose(f);
}

/* ---- what follows from what was clicked ---- */

static const char *mime_of(const char *path) {
    const char *dot = strrchr(path, '.');
    static const struct {
        const char *ext, *mime;
    } m[] = {{".png", "image/png"},   {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
             {".webp", "image/webp"}, {".svg", "image/svg+xml"}, {".pdf", "application/pdf"}, {".html", "text/html"},
             {".htm", "text/html"},   {".txt", "text/plain"}, {".md", "text/markdown"}, {".mp3", "audio/mpeg"},
             {".ogg", "audio/ogg"},   {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".mkv", "video/x-matroska"},
             {".zip", "application/zip"}, {".odt", "application/vnd.oasis.opendocument.text"}};
    for (size_t i = 0; dot && i < sizeof m / sizeof m[0]; i++)
        if (!strcasecmp(dot, m[i].ext))
            return m[i].mime;
    return NULL;
}

static void short_words(const char *t, char *out, size_t n) {
    size_t k = 0;
    for (const char *p = t; *p && k + 4 < n && k < 28; p++)
        out[k++] = *p == '\n' || *p == '\t' ? ' ' : *p;
    out[k] = 0;
    if (strlen(t) > 28)
        snprintf(out + k, n - k, "...");
}

void menu_complete(mf_spec *s, char (*run)[MENU_RUN]) {
    char r[MENU_RUN], label[160];
    /* a picture */
    if (C.image[0]) {
        snprintf(r, sizeof r, "img-view:%s", C.image);
        add(s, run, "open-picture", "Open picture", "picture", "The picture, large", r, 0);
        snprintf(r, sizeof r, "img-copy:%s", C.image);
        add(s, run, "copy-image", "Copy image", "copy", "The picture itself, for pasting into anything that takes "
            "pictures", r, 0);
        const char *home = getenv("HOME");
        if (!home || strncmp(C.image, home, strlen(home))) { /* (not one of theirs already) */
            snprintf(r, sizeof r, "img-save:%s", C.image);
            add(s, run, "save-image", "Save image", "download", "A copy in your Pictures", r, 0);
        }
        snprintf(r, sizeof r, "img-wallpaper:%s", C.image);
        add(s, run, "set-as-wallpaper", "Set as wallpaper", "desktop", "Behind everything, covering the screen", r, 0);
    }
    /* a link */
    if (C.link[0]) {
        snprintf(r, sizeof r, "link-open:%s", C.link);
        add(s, run, "open-link", "Open link", "forward", C.link, r, 0);
        snprintf(r, sizeof r, "copy:%s", C.link);
        add(s, run, "copy-link", "Copy link", "copy", C.link, r, 0);
    }
    /* words */
    if (C.text[0]) {
        char w[40];
        short_words(C.text, w, sizeof w);
        if (!C.text_selected) {
            snprintf(r, sizeof r, "copy:%s", C.text);
            add(s, run, "copy-text", "Copy", "copy", "These words, for pasting", r, 0);
        }
        snprintf(label, sizeof label, "Search for “%s”", w);
        snprintf(r, sizeof r, "search:%s", C.text);
        add(s, run, "search-for-it", label, "search", "In your apps, files, settings and the web", r, 0);
        if (!strchr(C.text, ' ') && strlen(C.text) < 40) {
            snprintf(label, sizeof label, "Look up “%s”", w);
            snprintf(r, sizeof r, "lookup:%s", C.text);
            add(s, run, "look-it-up", label, "doc", "What the word means", r, 0);
        }
        snprintf(label, sizeof label, "Ask %s about it", A.d.ai_name[0] ? A.d.ai_name : "the companion");
        snprintf(r, sizeof r, "ask-about:%s", C.text);
        add(s, run, "ask-about-it", label, "spark", "The chat opens with these words, for you to ask", r, MF_QUIET);
    }
    /* offered everywhere: the system's "system" category, unless they said not; their own, and their categories' */
    bool not_everywhere[NANYWHERE] = {false};
    read_conf(s, run, not_everywhere, (int)NANYWHERE);
    bool replaced_system = false;
    for (int k = 0; k < s->ncats; k++)
        replaced_system |= !strcasecmp(s->cats[k].name, "system");
    if (!replaced_system) {
        static const char *const sys[] = {"take-screenshot", "open-terminal", "settings"};
        for (size_t i = 0; i < sizeof sys / sizeof sys[0]; i++) {
            bool skip = false;
            for (size_t k = 0; k < NANYWHERE; k++)
                skip |= not_everywhere[k] && !strcmp(anywhere[k].id, sys[i]);
            if (!skip)
                add_anywhere(s, run, sys[i], MF_GLOBAL);
        }
    }
    for (int k = 0; k < s->ncats; k++) { /* their categories offered everywhere: all they take, everywhere */
        if (!s->cats[k].global)
            continue;
        char w[420];
        snprintf(w, sizeof w, "%s", s->cats[k].take);
        for (char *save = NULL, *t = strtok_r(w, " ,", &save); t; t = strtok_r(NULL, " ,", &save))
            add_anywhere(s, run, t, MF_GLOBAL);
    }
    for (int i = 0; i < s->n; i++)
        s->a[i].uses = uses_of(s->a[i].id);
}

/* ---- doing it ---- */

static void expand_home(const char *p, char *out, size_t n) {
    const char *home = getenv("HOME");
    if (p[0] == '~' && (p[1] == '/' || !p[1]) && home)
        snprintf(out, n, "%s%s", home, p + 1);
    else
        snprintf(out, n, "%s", p);
}

static void save_image(const char *src) {
    const char *home = getenv("HOME");
    char dir[400], dst[700];
    snprintf(dir, sizeof dir, "%s/Pictures", home ? home : "/root");
    mkdir(dir, 0755);
    const char *base = strrchr(src, '/') ? strrchr(src, '/') + 1 : src;
    snprintf(dst, sizeof dst, "%s/%s", dir, base);
    for (int k = 2; access(dst, F_OK) == 0 && k < 100; k++) {
        const char *dot = strrchr(base, '.');
        int stem = dot ? (int)(dot - base) : (int)strlen(base);
        snprintf(dst, sizeof dst, "%s/%.*s %d%s", dir, stem, base, k, dot ? dot : "");
    }
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    char buf[65536];
    size_t n;
    bool ok = in && out;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    if (ok)
        toast("Saved in Pictures as %s.", strrchr(dst, '/') + 1);
    else
        toast("The picture couldn't be saved.");
}

static void open_with_menu(const char *path) {
    const char *mime = mime_of(path);
    char ids[12][96], names[12][48];
    int n = mime ? desktop_apps_for(mime, ids, names, 12) : 0;
    if (!n && mime && !strncmp(mime, "image/", 6))
        n = desktop_apps_for("image/", ids, names, 12);
    ctx_menu_open(A.in.mx, A.in.my);
    ctx_menu_for("file");
    char act[700], label[120];
    if (mime && (!strncmp(mime, "text/", 5))) {
        snprintf(act, sizeof act, "notes:%s", path);
        ctx_menu_add("Notes", act, false);
    }
    if (mime && !strncmp(mime, "image/", 6)) {
        snprintf(act, sizeof act, "img-view:%s", path);
        ctx_menu_add("Files' viewer", act, false);
    }
    for (int i = 0; i < n; i++) {
        snprintf(act, sizeof act, "with:%s\x1f%s", ids[i], path);
        snprintf(label, sizeof label, "%s", names[i]);
        ctx_menu_add(label, act, false);
    }
    if (!n)
        ctx_menu_note("No app you have opens this kind of file.");
}

bool menu_do(const char *r) {
    char p[700];
    if (!strcmp(r, "screenshot")) {
        capture_screenshot_now();
    } else if (!strcmp(r, "new-note")) {
        notes_new_note();
        fm_host_action(A.ui, NULL, "open:notes");
    } else if (!strcmp(r, "wallpaper-settings")) {
        settings_open_page("Appearance");
    } else if (!strncmp(r, "img-view:", 9)) {
        files_view(r + 9);
    } else if (!strncmp(r, "img-copy:", 9)) {
        if (clipboard_set_image(r + 9))
            toast("The picture is copied.");
        else
            toast("That picture couldn't be copied.");
    } else if (!strncmp(r, "img-save:", 9)) {
        save_image(r + 9);
    } else if (!strncmp(r, "img-wallpaper:", 14)) {
        if (!desktop_set_wallpaper(r + 14))
            toast("That picture couldn't be the wallpaper.");
    } else if (!strncmp(r, "link-open:", 10)) {
        const char *u = r + 10;
        if (strstr(u, "://"))
            desktop_open_link(u);
        else
            files_open_path(u);
    } else if (!strncmp(r, "copy:", 5)) {
        clipboard_set(r + 5);
        toast("Copied.");
    } else if (!strncmp(r, "search:", 7)) {
        launcher_open(r + 7);
    } else if (!strncmp(r, "lookup:", 7)) {
        char q[200];
        snprintf(q, sizeof q, "define %s", r + 7);
        launcher_open(q);
    } else if (!strncmp(r, "ask-about:", 10)) {
        char w[200];
        snprintf(w, sizeof w, "“%.180s”", r + 10);
        companion_ask_about_text(w);
    } else if (!strncmp(r, "notes:", 6)) {
        open_in_notes(r + 6);
    } else if (!strncmp(r, "with:", 5)) {
        const char *sep = strchr(r + 5, '\x1f');
        if (sep) {
            char id[100];
            snprintf(id, sizeof id, "%.*s", (int)(sep - r - 5), r + 5);
            desktop_open_with(id, sep + 1);
        }
    } else if (!strncmp(r, "open-with:", 10)) {
        open_with_menu(r + 10);
    } else if (!strncmp(r, "compress:", 9)) { /* NAME.tar.gz beside it */
        const char *path = r + 9, *slash = strrchr(path, '/');
        if (!slash)
            return true;
        char dir[600], name[300], cmd[1400];
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
        snprintf(name, sizeof name, "%s", slash + 1);
        if (strchr(dir, '\'') || strchr(name, '\'')) {
            toast("That name has a quote in it: it couldn't be compressed from here.");
            return true;
        }
        snprintf(cmd, sizeof cmd, "cd '%s' && tar -czf '%s.tar.gz' '%s'", dir[0] ? dir : "/", name, name);
        spawn_command(cmd, "Compress");
        toast("Compressing %s...", name);
    } else if (!strncmp(r, "props:", 6)) { /* what it is, told in a menu of its own */
        struct stat st;
        const char *path = r + 6;
        if (stat(path, &st) != 0) {
            toast("It isn't there any more.");
            return true;
        }
        char size[40], when[64], note[160];
        double b = (double)st.st_size;
        if (b < 1024) snprintf(size, sizeof size, "%.0f bytes", b);
        else if (b < 1048576) snprintf(size, sizeof size, "%.1f KB", b / 1024);
        else if (b < 1073741824) snprintf(size, sizeof size, "%.1f MB", b / 1048576);
        else snprintf(size, sizeof size, "%.2f GB", b / 1073741824);
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(when, sizeof when, "%e %b %Y, %H:%M", &tm);
        ctx_menu_open(A.in.mx, A.in.my);
        ctx_menu_for("file");
        const char *m = mime_of(path);
        snprintf(note, sizeof note, "%s \u00b7 %s \u00b7 changed %s \u00b7 %s%s%s", S_ISDIR(st.st_mode) ? "folder" : size,
                 m ? m : S_ISDIR(st.st_mode) ? "" : "file", when, (st.st_mode & 0222) ? "can be changed" : "read only",
                 "", "");
        char act[MENU_RUN];
        snprintf(act, sizeof act, "copy:%s", path);
        ctx_menu_action("copy-path", "Copy path", act, 0, NULL, "copy", path);
        ctx_menu_note(note);
    } else if (!strncmp(r, "own:", 4)) { /* their own action (menus.conf) */
        const char *w = r + 4;
        if (!strncmp(w, "open:", 5)) {
            fm_host_action(A.ui, NULL, w);
        } else if (!strncmp(w, "file:", 5)) {
            expand_home(w + 5, p, sizeof p);
            files_open_path(p);
        } else if (!strncmp(w, "url:", 4)) {
            desktop_open_link(w + 4);
        } else if (!strncmp(w, "cmd:", 4)) {
            spawn_command(w + 4, "A command of yours");
        } else if (!strncmp(w, "ask:", 4)) {
            fm_host_action(A.ui, NULL, "open:companion");
            snprintf(A.chat.buf, sizeof A.chat.buf, "%s", w + 4);
            A.chat.len = (int)strlen(A.chat.buf);
            te_reset(&A.chat.ed, A.chat.buf);
        } else {
            toast("“%.60s” isn't something an action of yours can do yet.", w);
        }
    } else {
        return false;
    }
    return true;
}
