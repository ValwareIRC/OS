#define _GNU_SOURCE
/* The launcher: one place to find anything.
 *
 * Type and it looks through the apps (built-in, bundled under /apps, and
 * .desktop entries), the names of their files (a small index kept in the
 * background), the pages of Settings, commands ("dark mode", "volume 50",
 * "restart"), sums (the answer appears as they type), and the companion
 * ("ask Sol ..." or anything ending in "?"). Matching is forgiving but
 * ranks the obvious first: a name that starts with what they typed, then a
 * word inside it that does, then anything containing it, then letters in
 * order. Results come in groups, with icons, and the keyboard does
 * everything: arrows, Enter, Esc. With nothing typed it shows what they
 * opened lately.
 *
 * It is drawn in the design's own colours, type and material, either as a
 * panel in the middle of the screen or as a list growing from the design's
 * own <search> field. */
#include "launcher.h"
#include "app.h"
#include "files.h"
#include "iconart.h"
#include "fm/fm.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define D (A.d)

static font *FB(float px) { return fm_host_font(D.font_body, px); }

/* ---- material ---- */

/* The same panel the desktop draws (desktop.c), so the launcher and Files'
 * menus are made of whatever the design chose: glass, frost, solid, neon. */
void material_panel(rect r, float radius) {
    bool dark = color_luma(D.surface) < 0.3f;
    if (D.shadows)
        shadow_round(A.frame, (rect){r.x, r.y + 14, r.w, r.h}, radius, 46, RGBA(0, 0, 0, dark ? 110 : 60));
    if (D.material == MAT_NEON)
        glow_round(A.frame, r, radius, 10 + 26 * D.glow, color_alpha(D.accent, 0.18f + 0.3f * D.glow));
    float op = D.surface_opacity;
    if (D.material != MAT_SOLID && D.blur >= 1) {
        blur_fast(A.frame, r, radius, (int)(D.blur * 0.9f) + 2);
        vibrancy(A.frame, r, radius, D.material == MAT_FROST ? 1.1f : 1.45f, dark ? -4 : 6);
    } else {
        op = 1;
    }
    /* words are read here: never thinner than this */
    if (op < 0.82f)
        op = 0.82f;
    if (D.material == MAT_FROST)
        op = op + 0.1f > 1 ? 1 : op + 0.1f;
    color top = color_mix(D.surface, RGBA(255, 255, 255, 255), dark ? 0.08f : 0.25f);
    fill_round_vgradient(A.frame, r, radius, color_alpha(top, op), color_alpha(D.surface, op));
    stroke_round_vgradient(A.frame, r, radius, 1, RGBA(255, 255, 255, dark ? 70 : 150), RGBA(255, 255, 255, dark ? 8 : 30));
    if (D.material == MAT_NEON)
        stroke_round(A.frame, r, radius, 1.4f, color_alpha(D.accent, 0.85f));
}

/* ---- recently opened ---- */

#define RECENT_PATH STATE_DIR "/recent.txt"
#define MAX_RECENT 16
static struct recent recents[MAX_RECENT];
static int nrecent = -1;

static void recent_load(void) {
    if (nrecent >= 0)
        return;
    nrecent = 0;
    FILE *f = fopen(RECENT_PATH, "r");
    char line[700];
    while (f && fgets(line, sizeof line, f) && nrecent < MAX_RECENT) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2 || t1 != line + 1)
            continue;
        *t2 = 0;
        struct recent *r = &recents[nrecent++];
        r->kind = line[0];
        snprintf(r->title, sizeof r->title, "%s", t1 + 1);
        snprintf(r->target, sizeof r->target, "%s", t2 + 1);
    }
    if (f)
        fclose(f);
}

static void recent_save(void) {
    FILE *f = fopen(RECENT_PATH, "w");
    if (!f)
        return;
    for (int i = 0; i < nrecent; i++)
        fprintf(f, "%c\t%s\t%s\n", recents[i].kind, recents[i].title, recents[i].target);
    fclose(f);
}

void recent_forget(const char *target) {
    recent_load();
    for (int i = 0; i < nrecent; i++)
        if (!strcmp(recents[i].target, target)) {
            memmove(&recents[i], &recents[i + 1], sizeof recents[0] * (size_t)(nrecent - i - 1));
            nrecent--;
            recent_save();
            return;
        }
}

void recent_add(char kind, const char *title, const char *target) {
    recent_load();
    for (int i = 0; i < nrecent; i++)
        if (!strcmp(recents[i].target, target) && recents[i].kind == kind) {
            memmove(&recents[i], &recents[i + 1], sizeof recents[0] * (size_t)(nrecent - i - 1));
            nrecent--;
            break;
        }
    if (nrecent == MAX_RECENT)
        nrecent--;
    memmove(&recents[1], &recents[0], sizeof recents[0] * (size_t)nrecent);
    recents[0].kind = kind;
    snprintf(recents[0].title, sizeof recents[0].title, "%s", title);
    snprintf(recents[0].target, sizeof recents[0].target, "%s", target);
    for (char *p = recents[0].title; *p; p++)
        if (*p == '\t' || *p == '\n')
            *p = ' ';
    nrecent++;
    recent_save();
}

int recent_list(const struct recent **out) {
    recent_load();
    *out = recents;
    return nrecent;
}

/* How recently this was opened, as a ranking bonus (0 if never). */
static int recency(const char *target) {
    recent_load();
    for (int i = 0; i < nrecent; i++)
        if (!strcmp(recents[i].target, target))
            return 60 + (MAX_RECENT - i) * 3;
    return 0;
}

/* ---- the file index: names under /root, read in the background ---- */

static struct {
    char **path;
    bool *dir;
    int n;
    job *job;
    uint64_t at;
    bool dirty;
} ix;

struct obuf {
    char *p;
    size_t len, cap;
    int n;
};

static void ob_add(struct obuf *o, const char *s) {
    size_t l = strlen(s);
    if (o->len + l + 1 > o->cap) {
        o->cap = (o->len + l + 1) * 2 + 4096;
        o->p = realloc(o->p, o->cap);
    }
    memcpy(o->p + o->len, s, l + 1);
    o->len += l;
}

static void index_walk(struct obuf *o, const char *dir, int depth) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && o->n < 20000) {
        if (e->d_name[0] == '.') /* hidden, and Trash */
            continue;
        char p[1024], line[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) < 0)
            continue;
        bool isdir = S_ISDIR(st.st_mode);
        snprintf(line, sizeof line, "%c%s\n", isdir ? 'd' : 'f', p);
        ob_add(o, line);
        o->n++;
        if (isdir && depth < 10)
            index_walk(o, p, depth + 1);
    }
    if (d)
        closedir(d);
}

static void index_child(void *arg, char **result) {
    (void)arg;
    struct obuf o = {0};
    ob_add(&o, "");
    index_walk(&o, HOME_DIR, 0);
    *result = o.p;
}

static void index_poll(void) {
    char *res = NULL;
    if (ix.job && job_poll(ix.job, &res)) {
        job_free(ix.job);
        ix.job = NULL;
        for (int i = 0; i < ix.n; i++)
            free(ix.path[i]);
        ix.n = 0;
        int cap = 0;
        for (char *s = res, *nl; s && *s; s = nl + 1) {
            nl = strchr(s, '\n');
            if (!nl)
                break;
            *nl = 0;
            if (ix.n == cap) {
                cap = cap ? cap * 2 : 256;
                ix.path = realloc(ix.path, sizeof *ix.path * (size_t)cap);
                ix.dir = realloc(ix.dir, sizeof *ix.dir * (size_t)cap);
            }
            ix.dir[ix.n] = s[0] == 'd';
            ix.path[ix.n++] = strdup(s + 1);
        }
        free(res);
        LOG("launcher: indexed %d names\n", ix.n);
    }
}

static void index_refresh(bool now) {
    if (ix.job)
        return;
    if (!now && ix.at && !ix.dirty && A.now - ix.at < 10000)
        return;
    ix.at = A.now;
    ix.dirty = false;
    ix.job = job_start(index_child, NULL);
}

void launcher_files_changed(void) { ix.dirty = true; }

/* ---- .desktop entries ---- */

struct dentry {
    char name[64], exec[256], comment[128], path[300];
};
static struct dentry dents[64];
static int ndents = -1;
void launcher_apps_changed(void) {
    ndents = -1;
    desktop_apps_changed(); /* Flatpak apps are apps like any other */
}

static bool read_desktop(const char *path, struct dentry *d) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    char line[512];
    bool in_entry = false, hidden = false, app = true;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') {
            in_entry = !strcmp(line, "[Desktop Entry]");
            continue;
        }
        if (!in_entry)
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "Name")) snprintf(d->name, sizeof d->name, "%s", v);
        else if (!strcmp(line, "Exec")) snprintf(d->exec, sizeof d->exec, "%s", v);
        else if (!strcmp(line, "Comment")) snprintf(d->comment, sizeof d->comment, "%s", v);
        else if (!strcmp(line, "NoDisplay") || !strcmp(line, "Hidden")) hidden |= !strcasecmp(v, "true");
        else if (!strcmp(line, "Type")) app = !strcmp(v, "Application");
    }
    fclose(f);
    /* field codes (%f, %U, ...) stand for files they'd be given: none here */
    char *p;
    while ((p = strchr(d->exec, '%')) && p[1]) {
        size_t skip = p[1] == '%' ? 1 : 2;
        memmove(p, p + skip, strlen(p + skip) + 1);
    }
    return d->name[0] && d->exec[0] && !hidden && app;
}

static void scan_dir_entries(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && ndents < 64) {
        size_t l = strlen(e->d_name);
        if (l < 9 || strcmp(e->d_name + l - 8, ".desktop"))
            continue;
        char p[400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (read_desktop(p, &dents[ndents]))
            ndents++;
    }
    if (d)
        closedir(d);
}

static void scan_desktop_entries(void) {
    if (ndents >= 0)
        return;
    ndents = 0;
    scan_dir_entries("/usr/share/applications");
    scan_dir_entries("/usr/local/share/applications");
    scan_dir_entries(HOME_DIR "/.local/share/applications");
    DIR *d = opendir("/apps"); /* bundled apps may bring their own */
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            char p[300];
            snprintf(p, sizeof p, "/apps/%s", e->d_name);
            scan_dir_entries(p);
        }
    if (d)
        closedir(d);
}

bool launcher_run_desktop_file(const char *path) {
    struct dentry d;
    if (!read_desktop(path, &d))
        return false;
    spawn_command(d.exec, d.name);
    recent_add('x', d.name, d.exec);
    return true;
}

/* ---- matching ---- */

static bool word_start(const char *s, int i) {
    if (i == 0)
        return true;
    unsigned char p = (unsigned char)s[i - 1], c = (unsigned char)s[i];
    if (strchr(" -_./(,:", p))
        return true;
    if (islower(p) && isupper(c))
        return true;
    return !isdigit(p) && isdigit(c);
}

/* How well `q` matches `s`: <0 not at all; 1000 exactly; ~900 the start of
 * it; ~800 the start of a word in it; ~600 somewhere inside; below 500
 * letters in order (more for initials and runs). */
static int fuzzy(const char *q, const char *s) {
    int ql = (int)strlen(q), sl = (int)strlen(s);
    if (!ql)
        return 0;
    if (!sl)
        return -1;
    if (!strncasecmp(s, q, (size_t)ql))
        return ql == sl ? 1000 : 900 - (sl - ql < 99 ? sl - ql : 99);
    for (int i = 1; i + ql <= sl; i++)
        if (word_start(s, i) && !strncasecmp(s + i, q, (size_t)ql))
            return 800 - (i < 99 ? i : 99);
    const char *p = strcasestr(s, q);
    if (p)
        return 600 - ((int)(p - s) < 99 ? (int)(p - s) : 99);
    int score = 0, si = 0, prev = -2, starts = 0, want = 0;
    for (int k = 0; k < ql; k++) {
        int c = tolower((unsigned char)q[k]);
        if (c == ' ')
            continue;
        want++;
        int found = -1;
        if (si < sl && tolower((unsigned char)s[si]) == c && prev == si - 1)
            found = si; /* keep a run going */
        for (int i = si; found < 0 && i < sl; i++)
            if (tolower((unsigned char)s[i]) == c && word_start(s, i))
                found = i;
        for (int i = si; found < 0 && i < sl; i++)
            if (tolower((unsigned char)s[i]) == c)
                found = i;
        if (found < 0)
            return -1;
        if (word_start(s, found))
            score += 12, starts++;
        else if (found == prev + 1)
            score += 8;
        else
            score += 1;
        prev = found;
        si = found + 1;
    }
    int r = 200 + score * 3 - sl / 2 + (starts == want ? 120 : 0);
    return r > 499 ? 499 : r < 1 ? 1 : r;
}

/* ---- results ---- */

enum group { GR_CALC, GR_ASK_TOP, GR_APPS, GR_COMMANDS, GR_SETTINGS, GR_FILES, GR_RECENT, GR_SUGGEST, GR_GET, GR_ASK, GR_COUNT };
static const char *const group_names[GR_COUNT] = {"Answer", "Ask", "Apps", "Commands", "Settings", "Files",
                                                  "Recent", "Suggestions", "From Flathub", ""};

struct res {
    int group, score;
    char kind; /* 'a' app, 'x' program, 'g' to get from Flathub, 'f' file, 'c' command, 's' setting, 'q' sum, '?' ask */
    char title[160], sub[200], target[512];
    int glyph, fkind;
};

#define MAX_RES 48
static struct res R[MAX_RES];
static int NR;

static struct res *add(int group, int score, char kind, const char *title, const char *sub, const char *target) {
    if (NR == MAX_RES) { /* full: replace the weakest if this is better */
        int w = 0;
        for (int i = 1; i < NR; i++)
            if (R[i].score < R[w].score)
                w = i;
        if (R[w].score >= score)
            return NULL;
        NR--;
        R[w] = R[NR];
    }
    struct res *r = &R[NR++];
    memset(r, 0, sizeof *r);
    r->group = group, r->score = score, r->kind = kind;
    snprintf(r->title, sizeof r->title, "%s", title);
    snprintf(r->sub, sizeof r->sub, "%s", sub ? sub : "");
    snprintf(r->target, sizeof r->target, "%s", target);
    return r;
}

/* ---- the calculator ---- */

struct calc {
    const char *p;
    bool ok;
    int ops;
};

static double expr(struct calc *c);

static void sp(struct calc *c) {
    while (*c->p == ' ')
        c->p++;
}

static double atom(struct calc *c) {
    sp(c);
    if (*c->p == '(') {
        c->p++;
        double v = expr(c);
        sp(c);
        if (*c->p == ')')
            c->p++;
        else
            c->ok = false;
        return v;
    }
    if (isdigit((unsigned char)*c->p) || *c->p == '.') {
        char *end;
        double v = strtod(c->p, &end);
        if (end == c->p)
            c->ok = false;
        c->p = end;
        return v;
    }
    static const struct {
        const char *name;
        double (*fn)(double);
    } fns[] = {{"sqrt", sqrt}, {"sin", sin}, {"cos", cos}, {"tan", tan}, {"log", log10}, {"ln", log},
               {"abs", fabs}, {"round", round}, {"floor", floor}, {"ceil", ceil}, {"exp", exp}};
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        size_t l = strlen(fns[i].name);
        if (!strncasecmp(c->p, fns[i].name, l) && !isalpha((unsigned char)c->p[l])) {
            c->p += l;
            c->ops++;
            return fns[i].fn(atom(c));
        }
    }
    if (!strncasecmp(c->p, "pi", 2) && !isalpha((unsigned char)c->p[2])) {
        c->p += 2;
        c->ops++;
        return M_PI;
    }
    if ((*c->p == 'e' || *c->p == 'E') && !isalpha((unsigned char)c->p[1])) {
        c->p++;
        c->ops++;
        return M_E;
    }
    c->ok = false;
    return 0;
}

static double unary(struct calc *c) {
    sp(c);
    if (*c->p == '-') {
        c->p++;
        return -unary(c);
    }
    if (*c->p == '+') {
        c->p++;
        return unary(c);
    }
    double v = atom(c);
    sp(c);
    if (*c->p == '^' || (c->p[0] == '*' && c->p[1] == '*')) {
        c->p += *c->p == '^' ? 1 : 2;
        c->ops++;
        return pow(v, unary(c)); /* right to left: 2^3^2 = 2^9 */
    }
    if (*c->p == '%' && !isdigit((unsigned char)c->p[1]) && c->p[1] != '(' && c->p[1] != ' ') {
        c->p++;
        c->ops++;
        return v / 100;
    }
    return v;
}

static double term(struct calc *c) {
    double v = unary(c);
    for (;;) {
        sp(c);
        char o = *c->p;
        bool times = o == '*' || o == 'x' || o == 'X';
        if (!strncmp(c->p, "\xC3\x97", 2)) /* × */
            times = true, c->p++;
        if (!times && o != '/' && o != '%' && strncmp(c->p, "\xC3\xB7", 2))
            return v;
        if (!strncmp(c->p, "\xC3\xB7", 2))
            o = '/', c->p++;
        c->p++;
        c->ops++;
        double r = unary(c);
        v = times ? v * r : o == '/' ? v / r : fmod(v, r);
    }
}

static double expr(struct calc *c) {
    double v = term(c);
    for (;;) {
        sp(c);
        char o = *c->p;
        if (o != '+' && o != '-')
            return v;
        c->p++;
        c->ops++;
        double r = term(c);
        v = o == '+' ? v + r : v - r;
    }
}

/* A sum, if the whole query is one ("2+2", "=sqrt(2)", "15% * 80"). */
static bool calculate(const char *q, char *out, size_t n) {
    while (*q == ' ' || *q == '=')
        q++;
    if (!*q || !strpbrk(q, "0123456789pP"))
        return false;
    struct calc c = {q, true, 0};
    double v = expr(&c);
    sp(&c);
    if (!c.ok || *c.p || !c.ops || !isfinite(v))
        return false;
    if (fabs(v - round(v)) < 1e-9 && fabs(v) < 1e15)
        snprintf(out, n, "%.0f", round(v) == 0 ? 0.0 : round(v));
    else
        snprintf(out, n, "%.10g", v);
    return true;
}

/* ---- commands and settings ---- */

static const struct cmd {
    const char *title, *sub, *keys, *target;
    int glyph;
    char kind;
} cmds[] = {
    {"Terminal", "Open a command line", "terminal shell console command prompt", "open:terminal", G_TERMINAL, 'c'},
    {"Dark mode", "Have your companion redo the space in dark tones", "dark mode night theme dim", "cmd:dark", G_MOON, 'c'},
    {"Light mode", "Have your companion redo the space in light tones", "light mode day bright theme", "cmd:light", G_SUN, 'c'},
    {"Mute", "Turn the sound off", "mute silence quiet sound off volume", "cmd:volume:0", G_SPEAKER, 'c'},
    {"Unmute", "Turn the sound back on", "unmute sound on volume", "cmd:unmute", G_SPEAKER, 'c'},
    {"Volume up", "Louder", "volume up louder sound", "cmd:volume:+10", G_SPEAKER, 'c'},
    {"Volume down", "Quieter", "volume down quieter sound softer", "cmd:volume:-10", G_SPEAKER, 'c'},
    {"Restart", "Start the computer again", "restart reboot", "power:restart", G_POWER, 'c'},
    {"Shut down", "Turn the computer off", "shut down power off turn off shutdown", "power:off", G_POWER, 'c'},
    {"Redesign my space", "A fresh take, still true to you", "redesign new look refresh theme change", "redesign", G_SPARK, 'c'},
    {"Take a screenshot", "An area, a window or the whole screen (Print Screen)",
     "screenshot screen shot capture snip snipping picture print screen grab", "capture:shot", G_PICTURE, 'c'},
    {"Record the screen", "A video of an area, a window or the whole screen",
     "record screen recording video screencast capture film", "capture:record", G_PICTURE, 'c'},
    {"Screenshot of everything", "The whole screen, saved straight away",
     "screenshot full whole screen now print", "capture:now", G_PICTURE, 'c'},
    {"Show desktop", "Tuck every window away", "show desktop hide windows minimize all clear", "cmd:desktop", G_DESKTOP, 'c'},
    {"New note", "Write something down", "new note write jot", "open:notes", G_DOC, 'c'},
    {"Open Trash", "What you deleted, until you empty it", "trash bin deleted recycle", "cmd:trash", G_TRASH, 'c'},
    {"Empty Trash", "Erase what's in Trash for good", "empty trash bin erase", "cmd:empty-trash", G_TRASH, 'c'},
    {"Talk to your companion", "Open your conversation", "talk chat companion conversation", "open:companion", G_SPARK, 'c'},
    {"What your companion knows about you", "Settings: your portrait, and what to forget",
     "portrait memory privacy knows facts forget personal data settings", "open:settings", G_GEAR, 's'},
    {"Larger text", "Comfort: every word bigger", "larger text font size bigger accessibility zoom comfort", "set:text", G_TEXT, 's'},
    {"Strong contrast", "Comfort: words that stand out more", "contrast high accessibility comfort readable", "set:contrast", G_EYE, 's'},
    {"Less movement", "Comfort: calmer animation", "reduce motion animation movement less comfort", "set:motion", G_SPARK, 's'},
    {"Mind", "Settings: the AI behind your companion, and where your words go",
     "mind ai model provider privacy connection settings", "open:settings", G_GEAR, 's'},
    {"Forget me", "Settings: erase everything and start again", "forget reset erase wipe factory settings", "open:settings", G_GEAR, 's'},
    {"About this computer", "Version, and why it looks like this", "about version system info computer", "open:about", G_DESKTOP, 's'},
};

static bool setting_on(const char *target) {
    if (!strcmp(target, "set:text")) return A.prof.text_scale > 1.05f;
    if (!strcmp(target, "set:contrast")) return A.prof.high_contrast;
    if (!strcmp(target, "set:motion")) return A.prof.reduce_motion;
    return false;
}

#define MIXER_READ_VOLUME ((int)0x80044D00u)
#define MIXER_WRITE_VOLUME ((int)0xC0044D00u)

static int volume_get(void) {
    int fd = open("/dev/mixer", O_RDWR), v = -1;
    if (fd < 0)
        return -1;
    if (ioctl(fd, MIXER_READ_VOLUME, &v) < 0)
        v = -1;
    close(fd);
    return v < 0 ? -1 : v & 0xff;
}

static bool volume_set(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fd = open("/dev/mixer", O_RDWR);
    if (fd < 0)
        return false;
    int v = pct | pct << 8;
    bool ok = ioctl(fd, MIXER_WRITE_VOLUME, &v) >= 0;
    close(fd);
    return ok;
}

/* "volume 50", "vol 30%", "sound 80": the number they want. */
static int volume_query(const char *q) {
    static const char *const words[] = {"volume", "vol", "sound", NULL};
    for (int i = 0; words[i]; i++) {
        size_t l = strlen(words[i]);
        if (strncasecmp(q, words[i], l))
            continue;
        const char *p = q + l;
        while (*p == ' ' || *p == ':' || *p == '=')
            p++;
        if (!isdigit((unsigned char)*p))
            return -1;
        int v = atoi(p);
        while (isdigit((unsigned char)*p))
            p++;
        if (*p == '%')
            p++;
        while (*p == ' ')
            p++;
        return *p ? -1 : v > 100 ? 100 : v;
    }
    return -1;
}

static void run_command(const char *t) {
    LOG("launcher: command %s\n", t);
    if (!strncmp(t, "open:", 5) || !strncmp(t, "power:", 6) || !strcmp(t, "redesign") || !strncmp(t, "capture", 7)) {
        if (A.ui)
            fm_host_action(A.ui, NULL, t);
    } else if (!strcmp(t, "cmd:dark") || !strcmp(t, "cmd:light")) {
        bool dark = !strcmp(t, "cmd:dark");
        design_begin(dark ? "Switch the whole space to a dark mode: deep, dark surfaces and backgrounds with light, "
                            "readable text. Keep the layout, the accents and everything else that makes it theirs."
                          : "Switch the whole space to a light mode: bright, light surfaces and backgrounds with dark, "
                            "readable text. Keep the layout, the accents and everything else that makes it theirs.");
        toast("%s is redoing your space in %s tones...", D.ai_name[0] ? D.ai_name : "Your companion", dark ? "dark" : "light");
    } else if (!strncmp(t, "cmd:volume:", 11) || !strcmp(t, "cmd:unmute")) {
        static int before = 70; /* the level to come back to after muting */
        int now = volume_get(), want;
        const char *v = t + 11;
        if (!strcmp(t, "cmd:unmute"))
            want = now > 0 ? now : before;
        else if (*v == '+' || *v == '-')
            want = (now < 0 ? 70 : now) + atoi(v);
        else
            want = atoi(v);
        if (want < 0) want = 0;
        if (want > 100) want = 100;
        if (want == 0 && now > 0)
            before = now;
        if (volume_set(want)) {
            LOG("launcher: volume %d\n", want);
            if (want)
                toast("Volume %d%%", want);
            else
                toast("Sound off");
        } else {
            toast("There's no sound device to change.");
        }
    } else if (!strcmp(t, "cmd:desktop")) {
        for (int w = 0; w < WIN_MAX; w++)
            if (A.win[w].open)
                A.win[w].minimized = true;
    } else if (!strcmp(t, "cmd:trash")) {
        files_open_path(TRASH_FILES);
    } else if (!strcmp(t, "cmd:empty-trash")) {
        files_empty_trash();
        toast("Trash is empty.");
    } else if (!strncmp(t, "set:", 4)) {
        const char *what = "";
        bool on = false;
        if (!strcmp(t, "set:text")) {
            A.prof.text_scale = A.prof.text_scale > 1.05f ? 1.0f : 1.25f;
            on = A.prof.text_scale > 1.05f, what = "Larger text";
        } else if (!strcmp(t, "set:contrast")) {
            A.prof.high_contrast = !A.prof.high_contrast;
            fm_contrast_floor = A.prof.high_contrast ? 7.0f : 4.5f;
            on = A.prof.high_contrast, what = "Strong contrast";
        } else if (!strcmp(t, "set:motion")) {
            A.prof.reduce_motion = !A.prof.reduce_motion;
            D.motion = A.prof.reduce_motion ? 0 : 0.6f;
            on = A.prof.reduce_motion, what = "Less movement";
        }
        portrait_save(&A.prof, STATE_DIR "/portrait.txt");
        if (A.ui)
            A.ui->changed = true;
        toast("%s is %s.", what, on ? "on" : "off");
    }
}

/* ---- searching ---- */

static void pretty_dir(const char *p, char *out, size_t n) {
    char d[1024];
    snprintf(d, sizeof d, "%s", p);
    char *s = strrchr(d, '/');
    if (s && s != d)
        *s = 0;
    if (!strncmp(d, HOME_DIR, strlen(HOME_DIR)))
        snprintf(out, n, "Home%s", d + strlen(HOME_DIR));
    else
        snprintf(out, n, "%s", d);
}

/* "ask Sol what's on today", "hey sol ...", or a question. */
static bool asks(const char *q, char *out, size_t n) {
    const char *name = D.ai_name;
    size_t nl = strlen(name);
    const char *rest = NULL;
    if (!strncasecmp(q, "ask ", 4))
        rest = q + 4;
    else if (!strncasecmp(q, "hey ", 4) && nl && !strncasecmp(q + 4, name, nl))
        rest = q + 4;
    else if (nl && !strncasecmp(q, name, nl) && (q[nl] == ',' || q[nl] == ':'))
        rest = q;
    if (rest) {
        while (*rest == ' ')
            rest++;
        if (nl && !strncasecmp(rest, name, nl) && (!rest[nl] || strchr(" ,:", rest[nl])))
            rest += nl;
        while (*rest == ' ' || *rest == ',' || *rest == ':')
            rest++;
        snprintf(out, n, "%s", rest);
        return true;
    }
    size_t l = strlen(q);
    if (l > 3 && q[l - 1] == '?' && strchr(q, ' ')) {
        snprintf(out, n, "%s", q);
        return true;
    }
    return false;
}

static void app_result(int a, int score, int group) {
    const char *id = fm_host_app_id(a), *label = fm_host_app_label(a);
    char sub[120];
    if (a == APP_COMPANION)
        snprintf(sub, sizeof sub, "Your companion");
    else if (a < APP_COUNT)
        snprintf(sub, sizeof sub, "App");
    else
        snprintf(sub, sizeof sub, "Linux app");
    add(group, score, 'a', label, sub, id);
}

/* ---- apps from Flathub ---- */

/* What Flathub offers (/usr/libexec/os/flathub-catalog keeps the list),
 * searched like everything else; picking one installs it with Flatpak and
 * opens it. Apps built on the runtime this system is made of need only
 * their own files; others bring a runtime of their own, said up front. */

#define CATALOG "/var/lib/os/flathub.tsv"
#define OWN_RUNTIME "org.freedesktop.Platform/x86_64/26.08"

struct offer {
    char *id, *name, *summary, *runtime, *size;
};

static struct {
    struct offer *o;
    int n;
    char *text; /* the file the fields point into */
    job *refresh;
    uint64_t at; /* last refresh started */
    job *install;
    char inst_id[160], inst_name[96];
} fh;

static void catalog_load(void) {
    FILE *f = fopen(CATALOG, "r");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = len > 0 ? malloc((size_t)len + 1) : NULL;
    if (!text || fread(text, 1, (size_t)len, f) != (size_t)len) {
        free(text);
        fclose(f);
        return;
    }
    fclose(f);
    text[len] = 0;
    free(fh.o);
    free(fh.text);
    fh.text = text;
    fh.o = NULL;
    fh.n = 0;
    int cap = 0;
    for (char *line = text, *nl; *line; line = nl + 1) {
        nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *field[5] = {0};
        char *p = line;
        for (int k = 0; k < 5 && p; k++) {
            field[k] = p;
            p = strchr(p, '\t');
            if (p)
                *p++ = 0;
        }
        if (field[0] && field[0][0] && field[1]) {
            if (fh.n == cap) {
                cap = cap ? cap * 2 : 1024;
                fh.o = realloc(fh.o, sizeof *fh.o * (size_t)cap);
            }
            fh.o[fh.n++] = (struct offer){field[0], field[1], field[2] ? field[2] : "", field[3] ? field[3] : "",
                                          field[4] ? field[4] : ""};
        }
        if (!nl)
            break;
    }
    LOG("launcher: Flathub offers %d apps\n", fh.n);
}

static void catalog_child(void *arg, char **result) {
    (void)arg;
    int r = system("/usr/libexec/os/flathub-catalog " CATALOG " >/tmp/flathub-catalog.log 2>&1");
    if (r == 0) {
        *result = strdup("ok");
        return;
    }
    /* why, in flatpak's words: its last line */
    char last[256] = "", line[256];
    FILE *f = fopen("/tmp/flathub-catalog.log", "r");
    while (f && fgets(line, sizeof line, f))
        if (line[strspn(line, " \r\n")])
            snprintf(last, sizeof last, "%s", line);
    if (f)
        fclose(f);
    last[strcspn(last, "\r\n")] = 0;
    LOG("launcher: Flathub's list didn't come (%d): %s\n", r, last[0] ? last : "no reason given");
    *result = strdup("failed");
}

/* Flathub's Mesa for the system's own runtime. Flatpak fetches it with the
 * runtime, but this runtime came with the system; an app installed onto the
 * disk would otherwise have no OpenGL (SDL apps then can't open a window). */
#define GL_EXT "org.freedesktop.Platform.GL.default"
#define GL_BRANCH "26.08"

static void install_child(void *arg, char **result) {
    char cmd[400];
    /* onto the disk when it is set up, so the app is still there after a restart */
    const char *where = access(FLATPAK_DISK_READY, F_OK) == 0 ? "--installation=os-data" : "--system";
    if (access("/var/lib/os-data/flatpak/runtime/" GL_EXT "/x86_64/" GL_BRANCH, F_OK) != 0 &&
        access("/var/lib/flatpak/runtime/" GL_EXT "/x86_64/" GL_BRANCH, F_OK) != 0) {
        snprintf(cmd, sizeof cmd,
                 "flatpak install %s -y --noninteractive flathub " GL_EXT "//" GL_BRANCH " >/tmp/flatpak-gl.log 2>&1", where);
        if (system(cmd) != 0)
            LOG("launcher: graphics drivers (" GL_EXT ") didn't install; see /tmp/flatpak-gl.log\n");
    }
    snprintf(cmd, sizeof cmd, "flatpak install %s -y --noninteractive flathub '%s' >/tmp/flatpak-install.log 2>&1",
             where, (const char *)arg);
    int r = system(cmd);
    *result = strdup(r == 0 ? "ok" : "failed");
}

/* The .desktop file an installed app exports, in either installation (false if none). */
static bool installed_entry(const char *id, char *path, size_t n) {
    snprintf(path, n, FLATPAK_DISK_EXPORTS "/applications/%s.desktop", id);
    if (access(path, F_OK) == 0)
        return true;
    snprintf(path, n, FLATPAK_SYSTEM_EXPORTS "/applications/%s.desktop", id);
    return access(path, F_OK) == 0;
}

static bool offer_installed(const char *id) {
    char p[300];
    return installed_entry(id, p, sizeof p);
}

/* The last line Flatpak wrote, to say what went wrong in its own words. */
static void install_error(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/tmp/flatpak-install.log", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0])
            snprintf(out, n, "%s", line);
    }
    if (f)
        fclose(f);
}

static void flathub_poll(void) {
    char *res = NULL;
    if (!fh.n && !fh.refresh && !fh.at)
        catalog_load(); /* what an earlier refresh left */
    /* a fresh list at start, again daily; every minute while there is none (no network yet) */
    uint64_t every = fh.n ? 24ull * 3600 * 1000 : 60 * 1000;
    if (!fh.refresh && (!fh.at || A.now - fh.at > every)) {
        fh.at = A.now ? A.now : 1;
        fh.refresh = job_start(catalog_child, NULL);
    }
    if (fh.refresh && job_poll(fh.refresh, &res)) {
        job_free(fh.refresh);
        fh.refresh = NULL;
        if (res && !strcmp(res, "ok"))
            catalog_load();
        free(res);
        res = NULL;
    }
    if (fh.install && job_poll(fh.install, &res)) {
        job_free(fh.install);
        fh.install = NULL;
        launcher_apps_changed();
        if (offer_installed(fh.inst_id)) {
            LOG("launcher: installed %s\n", fh.inst_id);
            char p[300];
            installed_entry(fh.inst_id, p, sizeof p);
            toast("%s is installed.", fh.inst_name);
            launcher_run_desktop_file(p);
        } else {
            char why[256];
            install_error(why, sizeof why);
            LOG("launcher: installing %s failed: %s\n", fh.inst_id, why);
            toast("%s couldn't be installed%s%s", fh.inst_name, why[0] ? ": " : ".", why);
        }
        fh.inst_id[0] = 0;
        free(res);
    }
}

static void flathub_install(const char *id, const char *name) {
    if (fh.install) {
        toast("Still installing %s. One app at a time.", fh.inst_name);
        return;
    }
    snprintf(fh.inst_id, sizeof fh.inst_id, "%s", id);
    snprintf(fh.inst_name, sizeof fh.inst_name, "%s", name);
    fh.install = job_start(install_child, fh.inst_id);
    LOG("launcher: installing %s from Flathub\n", id);
    toast(fh.install ? "Installing %s from Flathub..." : "%s couldn't be installed.", name);
}

/* "GNOME 50" for org.gnome.Platform/x86_64/50 */
static void runtime_name(const char *rt, char *out, size_t n) {
    const char *who = strstr(rt, "org.gnome.") ? "GNOME" : strstr(rt, "org.kde.") ? "KDE" : "Freedesktop";
    const char *ver = strrchr(rt, '/');
    snprintf(out, n, "%s %s", who, ver ? ver + 1 : "");
}

static void flathub_results(const char *s) {
    if (strlen(s) < 2)
        return;
    for (int i = 0; i < fh.n; i++) {
        const struct offer *o = &fh.o[i];
        int sc = fuzzy(s, o->name), s2 = fuzzy(s, o->id) - 60;
        if (s2 > sc)
            sc = s2;
        if (sc < 260 && strlen(s) >= 4) { /* what it is ("browser"), from its summary */
            const char *at = strcasestr(o->summary, s); /* the words themselves, not scattered letters */
            int s3 = at ? 560 - (int)(at - o->summary < 99 ? at - o->summary : 99) : -1;
            if (s3 > sc)
                sc = s3;
        }
        if (sc < 260 || offer_installed(o->id))
            continue;
        bool own = !strcmp(o->runtime, OWN_RUNTIME);
        char sub[200], rt[64];
        if (!strcmp(fh.inst_id, o->id))
            snprintf(sub, sizeof sub, "Installing...");
        else if (own)
            snprintf(sub, sizeof sub, "%s%s%s", o->summary, o->summary[0] ? "  \xC2\xB7  " : "", o->size);
        else {
            runtime_name(o->runtime, rt, sizeof rt);
            snprintf(sub, sizeof sub, "%s%s%s, and the %s runtime", o->summary, o->summary[0] ? "  \xC2\xB7  " : "",
                     o->size, rt);
        }
        add(GR_GET, sc - 120 + (own ? 30 : 0), 'g', o->name, sub, o->id);
    }
}
static void search(const char *q_in) {
    NR = 0;
    char q[256];
    snprintf(q, sizeof q, "%s", q_in);
    char *s = q;
    while (*s == ' ')
        s++;
    for (int l = (int)strlen(s); l > 0 && s[l - 1] == ' '; l--)
        s[l - 1] = 0;
    const char *ai = D.ai_name[0] ? D.ai_name : "your companion";
    char buf[300], sub[300];

    if (!*s) { /* nothing typed: what they opened lately, then a few suggestions */
        const struct recent *rec;
        int nr = recent_list(&rec);
        for (int i = 0; i < nr && NR < 8; i++) {
            const struct recent *r = &rec[i];
            if (r->kind == 'f') {
                if (access(r->target, F_OK))
                    continue;
                pretty_dir(r->target, sub, sizeof sub);
            } else if (r->kind == 'a') {
                if (fm_app_index(r->target) < 0)
                    continue;
                snprintf(sub, sizeof sub, "App");
            } else {
                snprintf(sub, sizeof sub, "%s", r->kind == 'x' ? "Program" : r->kind == 's' ? "Setting" : "Command");
            }
            struct res *x = add(GR_RECENT, 1000 - i, r->kind, r->title, sub, r->target);
            if (x && (x->kind == 'c' || x->kind == 's'))
                for (size_t k = 0; k < sizeof cmds / sizeof cmds[0]; k++)
                    if (!strcmp(cmds[k].target, r->target))
                        x->glyph = cmds[k].glyph;
        }
        static const int sugg[] = {APP_FILES, APP_NOTES, APP_TERMINAL, APP_SETTINGS};
        for (int i = 0; i < 4 && NR < 6; i++) {
            bool dup = false;
            for (int k = 0; k < NR; k++)
                dup |= R[k].kind == 'a' && !strcmp(R[k].target, app_id(sugg[i]));
            if (!dup)
                app_result(sugg[i], 500 - i, GR_SUGGEST);
        }
        goto done;
    }

    /* a sum */
    if (calculate(s, buf, sizeof buf)) {
        struct res *r = add(GR_CALC, 5000, 'q', buf, s, buf);
        if (r)
            r->glyph = G_CALC;
    }
    /* a question for the companion */
    char question[256];
    bool asking = asks(s, question, sizeof question);
    if (asking && question[0]) {
        snprintf(sub, sizeof sub, "Ask %s", ai);
        add(GR_ASK_TOP, 4000, '?', question, sub, question);
    }
    /* "volume 40" */
    int vol = volume_query(s);
    if (vol >= 0) {
        snprintf(buf, sizeof buf, "Set the volume to %d%%", vol);
        char t[32];
        snprintf(t, sizeof t, "cmd:volume:%d", vol);
        int now = volume_get();
        if (now >= 0)
            snprintf(sub, sizeof sub, "It's at %d%% now", now);
        else
            snprintf(sub, sizeof sub, "Sound");
        struct res *r = add(GR_COMMANDS, 3000, 'c', buf, sub, t);
        if (r)
            r->glyph = G_SPEAKER;
    }

    /* apps */
    int na = fm_host_app_count();
    for (int a = 0; a < na; a++) {
        if (a == APP_REDESIGN || a == APP_POWER) /* these are commands */
            continue;
        int sc = fuzzy(s, fm_host_app_label(a));
        int s2 = fuzzy(s, fm_host_app_id(a)) - 30;
        int s3 = a < APP_COUNT ? fuzzy(s, app_default_name(a)) - 20 : -1;
        if (s2 > sc) sc = s2;
        if (s3 > sc) sc = s3;
        if (sc >= 200)
            app_result(a, sc + 40 + recency(fm_host_app_id(a)), GR_APPS);
    }
    scan_desktop_entries();
    for (int i = 0; i < ndents; i++) {
        int sc = fuzzy(s, dents[i].name);
        if (sc >= 200)
            add(GR_APPS, sc + 20 + recency(dents[i].exec), 'x', dents[i].name, dents[i].comment[0] ? dents[i].comment : "Program",
                dents[i].exec);
    }
    /* commands and settings */
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        const struct cmd *c = &cmds[i];
        char title[160];
        snprintf(title, sizeof title, "%s", c->title);
        if (!strcmp(c->title, "What your companion knows about you") && D.ai_name[0])
            snprintf(title, sizeof title, "What %s knows about you", D.ai_name);
        int sc = fuzzy(s, title), k = fuzzy(s, c->keys) - 70;
        if (k > sc)
            sc = k;
        if (sc < 200 || (vol >= 0 && strstr(c->target, "volume")))
            continue;
        const char *st = c->sub;
        if (c->kind == 's' && !strncmp(c->target, "set:", 4)) {
            snprintf(sub, sizeof sub, "%s. Now %s.", c->sub, setting_on(c->target) ? "on" : "off");
            st = sub;
        }
        struct res *r = add(c->kind == 's' ? GR_SETTINGS : GR_COMMANDS, sc + recency(c->target), c->kind, title, st, c->target);
        if (r)
            r->glyph = c->glyph;
    }
    /* files, by name */
    if (strlen(s) >= 2)
        for (int i = 0; i < ix.n; i++) {
            const char *p = ix.path[i], *base = strrchr(p, '/');
            base = base ? base + 1 : p;
            int sc = fuzzy(s, base);
            if (sc < 300)
                continue;
            int depth = 0;
            for (const char *c = p + strlen(HOME_DIR); *c; c++)
                depth += *c == '/';
            sc += recency(p) - depth * 6 - (ix.dir[i] ? 0 : 0);
            pretty_dir(p, sub, sizeof sub);
            struct res *r = add(GR_FILES, sc, 'f', base, sub, p);
            if (r)
                r->fkind = ix.dir[i] ? FK_FOLDER : file_kind(p, false);
        }
    /* apps that could be installed */
    flathub_results(s);
    /* and always, at the end: ask the companion */
    if (!asking) {
        snprintf(buf, sizeof buf, "Ask %s about \xE2\x80\x9C%s\xE2\x80\x9D", ai, s);
        add(GR_ASK, 1, '?', buf, "Your companion", s);
    }

done:;
    /* groups in the order of their best result (sums and questions first,
     * the fallback question last), results by score within each */
    int rank[GR_COUNT], count[GR_COUNT] = {0};
    for (int g = 0; g < GR_COUNT; g++)
        rank[g] = INT32_MIN;
    for (int i = 0; i < NR; i++) {
        count[R[i].group]++;
        if (R[i].score > rank[R[i].group])
            rank[R[i].group] = R[i].score;
    }
    rank[GR_CALC] = 1 << 20, rank[GR_ASK_TOP] = 1 << 19, rank[GR_ASK] = -1;
    static const int caps[GR_COUNT] = {1, 1, 6, 5, 4, 8, 8, 6, 5, 1};
    struct res tmp[MAX_RES];
    int nt = 0;
    bool used[GR_COUNT] = {0};
    for (;;) {
        int g = -1;
        for (int k = 0; k < GR_COUNT; k++)
            if (!used[k] && count[k] && (g < 0 || rank[k] > rank[g]))
                g = k;
        if (g < 0)
            break;
        used[g] = true;
        int taken = 0;
        for (;;) { /* this group's best remaining, up to its cap */
            int b = -1;
            for (int i = 0; i < NR; i++)
                if (R[i].group == g && R[i].score > -1000000 && (b < 0 || R[i].score > R[b].score))
                    b = i;
            if (b < 0 || taken >= caps[g])
                break;
            tmp[nt++] = R[b];
            R[b].score = -1000001;
            taken++;
        }
    }
    memcpy(R, tmp, sizeof tmp[0] * (size_t)nt);
    NR = nt;
}

/* ---- state ---- */

static struct {
    bool open;      /* the panel */
    bool attached;  /* a design's <search> has the keyboard */
    fm_node *field; /* that search */
    char q[256], searched[256];
    bool searched_once;
    int sel, scroll;
    uint64_t opened_at;
    input_state in;  /* the input taken from the desktop this frame */
    bool took_pointer;
    int saved_mx, saved_my;
    rect panel, list; /* last frame */
    rect rows[MAX_RES];
    int nrows_drawn;
    bool started;
} L;

bool launcher_active(void) { return L.open; }

void launcher_open(const char *query) {
    if (A.confirm_power)
        return;
    L.open = true;
    L.attached = false;
    snprintf(L.q, sizeof L.q, "%s", query ? query : "");
    L.searched_once = false;
    L.sel = 0;
    L.scroll = 0;
    L.opened_at = A.now;
    if (A.ui) {
        A.ui->focus = NULL;
        fm_close_popovers(A.ui, NULL);
    }
    files_setup();
    index_refresh(false);
    LOG("launcher: open\n");
}

void launcher_close(void) {
    if (L.open)
        LOG("launcher: close\n");
    L.open = false;
}

static void refresh_results(void) {
    if (L.searched_once && !strcmp(L.searched, L.q))
        return;
    search(L.q);
    snprintf(L.searched, sizeof L.searched, "%s", L.q);
    L.searched_once = true;
    L.sel = 0;
    L.scroll = 0;
    LOG("launcher: query \"%s\": %d results%s%c %s\n", L.q, NR, NR ? ", top " : "", NR ? R[0].kind : ' ',
        NR ? R[0].target : "");
}

static bool is_search(const fm_node *n) { return n && n->kind == FM_ELEM && !strcasecmp(n->tag, "search"); }

static bool in_popover(const fm_node *n) {
    for (n = n ? n->parent : NULL; n; n = n->parent)
        if (n->kind == FM_ELEM && (!strcasecmp(n->tag, "popover") || !strcasecmp(n->tag, "sheet") || !strcasecmp(n->tag, "menu")))
            return true;
    return false;
}

static void run(const struct res *r) {
    LOG("launcher: run %c %s\n", r->kind, r->target);
    char act[600];
    switch (r->kind) {
    case 'q': /* the answer becomes the query, to carry on from */
        snprintf(L.q, sizeof L.q, "%s", r->target);
        if (L.attached && L.field)
            fm_attr_set(L.field, "value", L.q);
        return;
    case 'a':
        snprintf(act, sizeof act, "open:%s", r->target);
        if (A.ui)
            fm_host_action(A.ui, NULL, act);
        recent_add('a', r->title, r->target);
        break;
    case 'x':
        spawn_command(r->target, r->title);
        recent_add('x', r->title, r->target);
        break;
    case 'g':
        flathub_install(r->target, r->title);
        break;
    case 'f':
        files_open_path(r->target);
        break;
    case 'c':
    case 's':
        run_command(r->target);
        if (strncmp(r->target, "cmd:volume:", 11) || strchr("+-0", r->target[11]))
            recent_add(r->kind, r->title, r->target);
        break;
    case '?':
        snprintf(act, sizeof act, "ask:%s", r->target);
        if (A.ui)
            fm_host_action(A.ui, NULL, act);
        break;
    }
    launcher_close();
    if (L.attached && L.field && A.ui) {
        fm_attr_set(L.field, "value", "");
        A.ui->focus = NULL;
        fm_close_popovers(A.ui, NULL);
    }
    L.attached = false;
}

/* Applies one typed key to a query; false if it isn't typing. */
static bool type_key(char *q, size_t n, int k) {
    size_t len = strlen(q);
    if (k == K_BACKSPACE) {
        while (len > 0 && (q[len - 1] & 0xC0) == 0x80)
            len--;
        if (len)
            len--;
        q[len] = 0;
    } else if (k == K_PASTE) {
        for (const char *p = clipboard_text(); *p && len < n - 2; p++)
            if ((unsigned char)*p >= 32)
                q[len++] = *p;
        q[len] = 0;
    } else if (k >= 32 && k < 0x80 && len < n - 2) {
        q[len++] = (char)k;
        q[len] = 0;
    } else if (k >= 0xA0 && k < K_ENTER && len < n - 3) { /* Latin-1 letters; K_* keys start at 0x100 */
        q[len++] = (char)(0xC0 | k >> 6);
        q[len++] = (char)(0x80 | (k & 0x3f));
        q[len] = 0;
    } else {
        return false;
    }
    return true;
}

static void move_sel(int d) {
    if (!NR)
        return;
    L.sel = ((L.sel + d) % NR + NR) % NR;
}

void launcher_input(fm_doc *ui) {
    if (!L.started) { /* the first desktop frame: Files' folders, and an index ready before it's needed */
        L.started = true;
        files_setup();
        index_refresh(true);
        /* development aid (fw_cfg opt/os/autorun.sh): a test script, run once as the person */
        if (access("/etc/os/autorun.sh", R_OK) == 0)
            spawn_command("sh /etc/os/autorun.sh </dev/null >/dev/console 2>&1", "Autorun");
    }
    index_poll();
    flathub_poll();
    if (ix.dirty && !ix.job && A.now - ix.at > 4000)
        index_refresh(true);
    L.took_pointer = false;
    if (A.confirm_power || A.screen != SCR_DESKTOP) {
        L.open = false;
        return;
    }
    L.in = A.in;

    /* a design's own <search> has the keyboard: grow results from it */
    fm_node *f = ui ? ui->focus : NULL;
    const char *what = f ? fm_attr_get(f, "what") : NULL;
    bool attach = !L.open && is_search(f) && !(what && !strncasecmp(what, "ask", 3));
    if (attach != L.attached || f != L.field) {
        L.attached = attach;
        L.field = attach ? f : NULL;
        L.searched_once = false;
        L.panel = (rect){0, 0, 0, 0};
    }
    if (L.attached) {
        const char *v = fm_attr_get(f, "value");
        snprintf(L.q, sizeof L.q, "%s", v ? v : "");
        int keep[32], nk = 0;
        bool typed = false, ran = false;
        for (int i = 0; i < A.in.nkeys; i++) {
            int k = A.in.keys[i];
            if (type_key(L.q, sizeof L.q, k)) {
                typed = true;
                continue;
            }
            if (typed) { /* see the results for what was typed before moving through them */
                refresh_results();
                typed = false;
            }
            refresh_results();
            if ((k == K_DOWN || k == K_TAB) && NR)
                move_sel(1);
            else if (k == K_UP && NR)
                move_sel(-1);
            else if (k == K_ENTER && NR && L.q[0] && !ran) {
                ran = true;
                fm_attr_set(f, "value", L.q);
                run(&R[L.sel]);
                break;
            } else
                keep[nk++] = k;
        }
        if (!ran && L.field) {
            fm_attr_set(f, "value", L.q);
            refresh_results();
        }
        memcpy(A.in.keys, keep, sizeof keep[0] * (size_t)nk);
        A.in.nkeys = nk;
        /* the pointer over the list is the list's */
        if (L.panel.w && pt_in(L.panel, A.in.mx, A.in.my)) {
            L.took_pointer = true;
            L.saved_mx = A.in.mx, L.saved_my = A.in.my;
            A.in.pressed = A.in.clicked = A.in.right_clicked = false;
            A.in.wheel = 0;
            A.in.mx = A.in.my = -10000;
        }
        return;
    }
    if (!L.open)
        return;

    /* the panel takes everything while it is up */
    for (int i = 0; i < A.in.nkeys && L.open; i++) {
        int k = A.in.keys[i];
        if (type_key(L.q, sizeof L.q, k))
            continue;
        refresh_results();
        if (k == K_ESC || k == K_META)
            launcher_close();
        else if (k == K_DOWN || k == K_TAB)
            move_sel(1);
        else if (k == K_UP)
            move_sel(-1);
        else if (k == K_PGDN)
            move_sel(5);
        else if (k == K_PGUP)
            move_sel(-5);
        else if (k == K_ENTER && NR)
            run(&R[L.sel]);
    }
    if (L.open)
        refresh_results();
    L.took_pointer = true;
    L.saved_mx = A.in.mx, L.saved_my = A.in.my;
    A.in.nkeys = 0;
    A.in.nraw = 0;
    A.in.pressed = A.in.clicked = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.mx = A.in.my = -10000;
}

/* ---- drawing ---- */

static float ease(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

static void result_icon(rect ic, const struct res *r, bool sel) {
    static fm_style none; /* the design's own icon shape and style */
    color tile = sel ? RGBA(255, 255, 255, 46) : color_alpha(D.accent, 0.16f);
    color ink = sel ? readable_on(D.accent) : D.accent;
    if (!sel && color_contrast(D.accent, D.surface) < 2.2f)
        ink = D.surface_text;
    float rad = D.radius * 0.4f + 4;
    switch (r->kind) {
    case 'a': {
        int a = fm_app_index(r->target);
        if (a >= 0) {
            fm_host_draw_icon(ic, a, &none, false);
            return;
        }
        break;
    }
    case '?':
        fm_host_draw_icon(ic, APP_COMPANION, &none, false);
        return;
    case 'f':
        file_icon(A.frame, ic, r->fkind, r->title, sel);
        return;
    case 'x':
    case 'g': {
        /* an app from Flathub shows the icon its catalog brings */
        if (r->kind == 'g' && app_icon_draw(A.frame, ic, r->target, "", 0, LOOK_COLOR, 0, 1.0f, sel))
            return;
        unsigned h = 2166136261u;
        for (const char *p = r->title; *p; p++)
            h = (h ^ (uint8_t)*p) * 16777619u;
        color t = color_mix(D.accent, D.accent2, (float)(h % 100) / 100.0f);
        fill_round_vgradient(A.frame, ic, rad, color_mix(t, RGBA(255, 255, 255, 255), 0.18f), t);
        char letter[2] = {r->title[0] >= 'a' && r->title[0] <= 'z' ? (char)(r->title[0] - 32) : r->title[0], 0};
        draw_text_centered(A.frame, font_get(D.font_heading, ic.h * 0.5f), ic, letter, readable_on(t));
        return;
    }
    }
    fill_round(A.frame, ic, rad, tile);
    ui_glyph(A.frame, rect_inset(ic, ic.w / 5), r->glyph, 1.7f, ink);
}

/* Long names lose their middle, not their end (file names end in what
 * tells them apart: "report-final-v3.txt"). */
static void middle_fit(font *f, const char *s, int maxw, char *out, size_t n) {
    snprintf(out, n, "%s", s);
    int len = (int)strlen(s);
    for (int cut = 1; text_width(f, out) > maxw && cut < len - 2; cut++) {
        int a = (len - cut) / 2, b = a + cut;
        while (a > 0 && (s[a] & 0xC0) == 0x80)
            a--;
        while (b < len && (s[b] & 0xC0) == 0x80)
            b++;
        snprintf(out, n, "%.*s\xE2\x80\xA6%s", a, s, s + b);
    }
}

/* The height the results want, with a heading over each group. */
static int results_height(int row_h, int head_h) {
    int h = 0, g = -1;
    for (int i = 0; i < NR; i++) {
        if (R[i].group != g) {
            g = R[i].group;
            if (group_names[g][0])
                h += head_h;
        }
        h += R[i].kind == 'q' ? row_h + 16 : row_h;
    }
    return h;
}

static void draw_results(rect area, int row_h, int head_h, const input_state *in) {
    font *f = FB(D.body_px), *small = FB(D.body_px - 3), *head = FB(D.body_px - 4);
    font *big = fm_host_font(D.font_heading, D.body_px + 12);
    /* keep the selection in view */
    int y = 0, g = -1, sel_y = 0, sel_h = row_h;
    for (int i = 0; i < NR; i++) {
        if (R[i].group != g) {
            g = R[i].group;
            if (group_names[g][0])
                y += head_h;
        }
        int h = R[i].kind == 'q' ? row_h + 16 : row_h;
        if (i == L.sel)
            sel_y = y, sel_h = h;
        y += h;
    }
    if (sel_y - L.scroll < (L.sel == 0 ? 0 : head_h))
        L.scroll = sel_y - (L.sel == 0 ? sel_y : head_h);
    if (sel_y + sel_h - L.scroll > area.h)
        L.scroll = sel_y + sel_h - area.h;
    if (L.scroll < 0)
        L.scroll = 0;
    canvas_clip(A.frame, area);
    y = area.y - L.scroll;
    g = -1;
    L.nrows_drawn = 0;
    bool hover_moves = in->moved && pt_in(area, in->mx, in->my);
    for (int i = 0; i < NR; i++) {
        const struct res *r = &R[i];
        if (r->group != g) {
            g = r->group;
            if (group_names[g][0]) {
                char up[40];
                int k = 0;
                for (const char *p = group_names[g]; *p && k < 38; p++)
                    up[k++] = (char)toupper((unsigned char)*p);
                up[k] = 0;
                draw_text(A.frame, head, area.x + 14, y + head_h - font_line_height(head) - 4, up, D.muted);
                y += head_h;
            }
        }
        int h = r->kind == 'q' ? row_h + 16 : row_h;
        rect row = {area.x + 4, y, area.w - 8, h};
        L.rows[i] = row;
        if (hover_moves && pt_in(row, in->mx, in->my))
            L.sel = i;
        bool sel = i == L.sel;
        if (sel)
            fill_round(A.frame, row, D.radius * 0.45f + 4, D.accent);
        color ink = sel ? readable_on(D.accent) : D.surface_text;
        color sub = sel ? color_alpha(readable_on(D.accent), 0.78f) : D.muted;
        int is = r->kind == 'q' ? 40 : row_h - 16;
        rect ic = {row.x + 10, row.y + (h - is) / 2, is, is};
        result_icon(ic, r, sel);
        int tx = ic.x + is + 14, tw = row.w - (tx - row.x) - 90;
        if (r->kind == 'q') { /* the answer, large */
            char t[200];
            snprintf(t, sizeof t, "= %s", r->title);
            draw_text(A.frame, big, tx, row.y + 6, t, ink);
            draw_text(A.frame, small, tx, row.y + 8 + font_line_height(big), r->sub, sub);
        } else {
            char t[200];
            int th = font_line_height(f), sh = r->sub[0] ? font_line_height(small) : 0;
            int ty = row.y + (h - th - sh - (sh ? 2 : 0)) / 2;
            middle_fit(f, r->title, tw, t, sizeof t);
            draw_text(A.frame, f, tx, ty, t, ink);
            if (sh)
                draw_text(A.frame, small, tx, ty + th + 2, r->sub, sub);
        }
        if (sel) { /* what Enter will do */
            const char *verb = r->kind == 'q' ? "Use" : r->kind == '?' ? "Ask" : r->kind == 'c' || r->kind == 's' ? "Run" : "Open";
            char hint[24];
            snprintf(hint, sizeof hint, "%s \xE2\x86\xB5", verb);
            int w = text_width(small, hint) + 16;
            rect hr = {row.x + row.w - w - 10, row.y + (h - 22) / 2, w, 22};
            fill_round(A.frame, hr, 11, RGBA(255, 255, 255, color_luma(D.accent) > 0.5f ? 70 : 40));
            draw_text_centered(A.frame, small, hr, hint, ink);
        }
        if (i < 16) {
            char nm[32];
            snprintf(nm, sizeof nm, "launcher-row-%d", i);
            static uint64_t last;
            if (A.now - last > 400 || i == 0)
                LOGRECT(nm, row);
            if (i == NR - 1 || i == 15)
                last = A.now;
        }
        L.nrows_drawn = i + 1;
        y += h;
    }
    canvas_unclip(A.frame);
}

static void restore_pointer(void) {
    if (L.took_pointer) {
        A.in.mx = L.saved_mx, A.in.my = L.saved_my;
        L.took_pointer = false;
    }
}

/* Clicks in the list (panel or dropdown) open what was clicked. */
static void click_rows(const input_state *in) {
    if (!in->clicked)
        return;
    for (int i = 0; i < L.nrows_drawn; i++)
        if (pt_in(L.rows[i], in->mx, in->my) && pt_in(L.list, in->mx, in->my)) {
            L.sel = i;
            run(&R[i]);
            return;
        }
}

static void draw_dropdown(void) {
    fm_node *f = L.field;
    if (!f || !f->laid_out || (!L.q[0] && in_popover(f))) {
        L.panel = (rect){0, 0, 0, 0};
        return;
    }
    refresh_results();
    if (!NR) {
        L.panel = (rect){0, 0, 0, 0};
        return;
    }
    int row_h = font_line_height(FB(D.body_px)) + font_line_height(FB(D.body_px - 3)) + 16;
    if (row_h < 46)
        row_h = 46;
    int head_h = 26;
    rect s = f->vis;
    int w = s.w > 460 ? s.w : 460;
    if (w > A.W - 24)
        w = A.W - 24;
    int x = s.x + s.w / 2 - w / 2;
    if (x + w > A.W - 12) x = A.W - 12 - w;
    if (x < 12) x = 12;
    int want = results_height(row_h, head_h) + 16;
    int below = A.H - (s.y + s.h) - 24, above = s.y - 24;
    bool down = below >= 260 || below >= above;
    int room = down ? below : above;
    int h = want < room ? want : room;
    if (h > A.H * 6 / 10)
        h = A.H * 6 / 10;
    rect p = {x, down ? s.y + s.h + 8 : s.y - 8 - h, w, h};
    L.panel = p;
    material_panel(p, D.radius * 0.6f + 6);
    L.list = rect_inset(p, 6);
    input_state in = L.in;
    draw_results(L.list, row_h, head_h, &in);
    LOGRECT("launcher-dropdown", p);
    click_rows(&in);
}

static void draw_panel(void) {
    refresh_results();
    float e = ease((float)(A.now - L.opened_at) / 220.0f);
    /* the world behind steps back a little */
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), 0.22f * e));
    int w = A.W - 64 < 720 ? A.W - 64 : 720;
    int field_h = 68, row_h = font_line_height(FB(D.body_px)) + font_line_height(FB(D.body_px - 3)) + 18;
    if (row_h < 50)
        row_h = 50;
    int head_h = 28, foot_h = 34;
    int list_h = NR ? results_height(row_h, head_h) + 10 : 0;
    int max_list = A.H * 62 / 100 - field_h;
    if (list_h > max_list)
        list_h = max_list;
    int h = field_h + (NR ? list_h + 1 : 0) + foot_h;
    int x = (A.W - w) / 2, y = A.H * 15 / 100 - (int)((1 - e) * 18 * (0.3f + D.motion));
    rect p = {x, y, w, h};
    L.panel = p;
    float radius = D.radius + 8 > 30 ? 30 : D.radius + 8;
    material_panel(p, radius);
    LOGRECT("launcher-panel", p);

    /* the field */
    font *qf = FB(D.body_px + 7), *small = FB(D.body_px - 3);
    rect gl = {x + 22, y + (field_h - 26) / 2, 26, 26};
    ui_glyph(A.frame, gl, G_SEARCH, 2.0f, color_contrast(D.accent, D.surface) > 2.2f ? D.accent : D.surface_text);
    int tx = gl.x + 40, ty = y + (field_h - font_line_height(qf)) / 2;
    rect fr = {tx, y, w - (tx - x) - 20, field_h};
    canvas_clip(A.frame, fr);
    int tw = text_width(qf, L.q), sx = tx;
    if (tw > fr.w - 6)
        sx = tx + fr.w - 6 - tw;
    if (L.q[0])
        draw_text(A.frame, qf, sx, ty, L.q, D.surface_text);
    else {
        char ph[160];
        snprintf(ph, sizeof ph, "Apps, files, settings, sums, or ask %s", D.ai_name[0] ? D.ai_name : "me");
        draw_text(A.frame, qf, tx, ty, ph, color_alpha(D.muted, 0.9f));
    }
    if ((A.now / 530) % 2 == 0)
        fill_rect(A.frame, (rect){sx + (L.q[0] ? tw : 0) + 2, ty + 3, 2, font_line_height(qf) - 6}, D.accent);
    canvas_unclip(A.frame);
    LOGRECT("launcher-field", fr);

    if (NR) {
        fill_rect(A.frame, (rect){x + 16, y + field_h, w - 32, 1}, color_alpha(D.surface_text, 0.1f));
        L.list = (rect){x + 6, y + field_h + 6, w - 12, list_h - 6};
        input_state in = L.in;
        in.mx = L.saved_mx, in.my = L.saved_my;
        draw_results(L.list, row_h, head_h, &in);
        click_rows(&in);
        if (!L.open)
            return;
    }
    /* the footer: how to drive it */
    int fy = y + h - foot_h;
    fill_rect(A.frame, (rect){x + 16, fy, w - 32, 1}, color_alpha(D.surface_text, 0.07f));
    draw_text(A.frame, small, x + 22, fy + (foot_h - font_line_height(small)) / 2,
              "\xE2\x86\x91\xE2\x86\x93 choose   \xE2\x86\xB5 open   esc close", D.muted);
    char right[64];
    if (!L.q[0])
        snprintf(right, sizeof right, "%s", NR && R[0].group == GR_RECENT ? "Recently opened" : "Start typing");
    else
        snprintf(right, sizeof right, "%d result%s", NR, NR == 1 ? "" : "s");
    draw_text(A.frame, small, x + w - 22 - text_width(small, right), fy + (foot_h - font_line_height(small)) / 2, right,
              D.muted);
    /* a click outside puts it away */
    if (L.in.pressed && !pt_in(p, L.saved_mx, L.saved_my))
        launcher_close();
}

void launcher_draw(void) {
    if (L.open)
        draw_panel();
    else if (L.attached)
        draw_dropdown();
    else
        L.panel = (rect){0, 0, 0, 0};
    restore_pointer();
}
