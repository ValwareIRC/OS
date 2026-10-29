#define _GNU_SOURCE
/* Files: a file manager for the person's things under /root.
 *
 * A sidebar of places (and what they opened lately), a breadcrumb path they
 * can click and drop onto, list and grid views sorted by name, size, date or
 * kind, icons by type with picture thumbnails, a picture viewer, Trash with
 * put-back, copy/cut/paste, rename in place, drag and drop, multi-select
 * (Ctrl, Shift and a rubber band), a context menu and the keyboard. Every
 * colour, font and corner comes from the person's design. */
#include "files.h"
#include "app.h"
#include "fm/fm.h"
#include "image.h"
#include "launcher.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define D (A.d)

/* Linux key codes, for shortcuts the typed-character stream can't carry
 * (F2, Alt+arrows) or that it carries ambiguously (Ctrl+C is just 'c'). */
enum { RK_A = 30, RK_C = 46, RK_X = 45, RK_N = 49, RK_F2 = 60, RK_LCTRL = 29, RK_RCTRL = 97, RK_LSHIFT = 42,
       RK_RSHIFT = 54, RK_LALT = 56, RK_RALT = 100, RK_UP = 103, RK_LEFT = 105, RK_RIGHT = 106 };

#define MAX_ENTS 2048
#define PATHN 1024

struct ent {
    char name[256];
    bool dir, sel, was_sel;
    int kind;
    long long size;
    time_t mtime;
    mode_t mode;
    char origin[400]; /* in Trash: where it was */
    rect r;           /* where it is on screen this frame */
};

enum { V_LIST, V_GRID };
enum { S_NAME, S_SIZE, S_DATE, S_KIND };

enum { M_OPEN = 1, M_OPEN_NOTES, M_QUICKLOOK, M_RENAME, M_COPY, M_CUT, M_PASTE, M_TRASH, M_RESTORE, M_ERASE,
       M_EMPTY_TRASH, M_NEW_FOLDER, M_VIEW_LIST, M_VIEW_GRID, M_SORT_NAME, M_SORT_SIZE, M_SORT_DATE, M_SORT_KIND,
       M_SELECT_ALL };

struct mitem {
    const char *label;
    int id;
    bool enabled, checked, sep;
};

static struct {
    bool init;
    char path[PATHN];
    char back[24][PATHN];
    int nback;
    char fwd[24][PATHN];
    int nfwd;
    struct ent *e;
    int n;
    bool readable;
    char loaded[PATHN];
    uint64_t loaded_at;
    bool reload;
    char select_after[256]; /* select this name once the folder is read again */
    bool rename_after;      /* ...and start renaming it */
    int view, sort;
    bool desc, view_chosen;
    int scroll, content_h;
    int cursor, anchor;
    /* rename in place */
    int renaming;
    struct textfield rn;
    /* the clipboard (Files' own: paths, not text) */
    char clip[64][PATHN];
    int nclip;
    bool clip_cut;
    /* the pointer */
    int press_idx, press_x, press_y;
    bool press_defer; /* pressed an already-selected item: decide on release */
    uint64_t last_click_at;
    int last_click_idx;
    bool dragging, banding, band_add;
    int band_x0, band_y0; /* content coordinates (scroll included) */
    char drag[64][PATHN];
    int ndrag;
    char drop[PATHN]; /* where a drop would land this frame */
    /* the context menu */
    bool menu_on;
    int menu_target; /* the item it was opened on; -1 the folder, -2 the sort button */
    rect menu_r;
    struct mitem mi[20];
    int nm;
    /* the picture viewer */
    bool viewing;
    char view_path[PATHN];
    image *view_full, *view_fit;
    bool view_failed;
    /* typing a name selects it */
    char ahead[64];
    uint64_t ahead_at;
    uint64_t erase_confirm_at;
    /* a word in the status bar, and an undo for the last move to Trash */
    char status[200];
    uint64_t status_until;
    char undo[32][300];
    int nundo;
    int grid_cols;
} F = {.renaming = -1, .cursor = -1, .anchor = -1, .press_idx = -1, .last_click_idx = -1};

static input_state I; /* this frame's input, as far as Files may use it */
static bool focused;  /* Files has the keyboard */

static font *FB(float px) { return fm_host_font(D.font_body, px); }
static font *FH(float px) { return fm_host_font(D.font_heading, px); }

/* ---- paths ---- */

static void join(char *out, size_t n, const char *dir, const char *name) {
    size_t l = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, l && dir[l - 1] == '/' ? "" : "/", name);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}

static void dir_name(const char *p, char *out, size_t n) {
    snprintf(out, n, "%s", p);
    char *s = strrchr(out, '/');
    if (s && s != out)
        *s = 0;
    else
        snprintf(out, n, "/");
}

static bool exists(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Is `p` the folder `dir` or inside it? (A folder can't go into itself.) */
static bool inside(const char *p, const char *dir) {
    size_t l = strlen(dir);
    return !strncmp(p, dir, l) && (p[l] == 0 || p[l] == '/');
}

/* A name not yet taken in `dir`: "Report.txt", "Report 2.txt", ... For
 * copies into the same folder, "Report copy.txt" first, the way people
 * expect. */
static void unique_in(const char *dir, const char *name, bool as_copy, char *out, size_t n) {
    char full[PATHN];
    join(full, sizeof full, dir, name);
    if (!exists(full) && !as_copy) {
        snprintf(out, n, "%s", name);
        return;
    }
    char stem[256], ext[64] = "";
    snprintf(stem, sizeof stem, "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem && strlen(dot) < sizeof ext && !is_dir(full)) {
        snprintf(ext, sizeof ext, "%s", dot);
        *dot = 0;
    }
    for (int i = 1; i < 1000; i++) {
        if (as_copy && i > 1)
            snprintf(out, n, "%s copy %d%s", stem, i, ext);
        else if (as_copy)
            snprintf(out, n, "%s copy%s", stem, ext);
        else
            snprintf(out, n, "%s %d%s", stem, i + 1, ext);
        join(full, sizeof full, dir, out);
        if (!exists(full))
            return;
    }
}

/* ---- file operations ---- */

static int rm_rf(const char *p) {
    struct stat st;
    if (lstat(p, &st) < 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char c[PATHN];
            join(c, sizeof c, p, e->d_name);
            rm_rf(c);
        }
        if (d)
            closedir(d);
        return rmdir(p);
    }
    return unlink(p);
}

static int copy_rec(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) < 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) < 0 && errno != EEXIST)
            return -1;
        DIR *d = opendir(src);
        struct dirent *e;
        int r = 0;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char a[PATHN], b[PATHN];
            join(a, sizeof a, src, e->d_name);
            join(b, sizeof b, dst, e->d_name);
            r |= copy_rec(a, b);
        }
        if (d)
            closedir(d);
        return r;
    }
    if (S_ISLNK(st.st_mode)) {
        char t[PATHN];
        ssize_t l = readlink(src, t, sizeof t - 1);
        if (l < 0)
            return -1;
        t[l] = 0;
        return symlink(t, dst);
    }
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (out < 0) {
        close(in);
        return -1;
    }
    static char buf[65536];
    ssize_t r;
    int res = 0;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)r) != r) {
            res = -1;
            break;
        }
    close(in);
    close(out);
    return r < 0 ? -1 : res;
}

/* Moves, by renaming when it can and copying when it must. */
static int move_path(const char *src, const char *dst) {
    if (rename(src, dst) == 0)
        return 0;
    if (copy_rec(src, dst) < 0)
        return -1;
    return rm_rf(src);
}

static void changed(void) {
    F.reload = true;
    launcher_files_changed();
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(F.status, sizeof F.status, fmt, ap);
    va_end(ap);
    F.status_until = A.now + 6000;
    F.nundo = 0;
    LOG("files: %s\n", F.status);
}

/* Into Trash, remembering where it was so it can go back. */
static bool trash_put(const char *path, char *trashed, size_t tn) {
    char name[256];
    unique_in(TRASH_FILES, base_name(path), false, name, sizeof name);
    char dst[PATHN], info[PATHN];
    join(dst, sizeof dst, TRASH_FILES, name);
    if (move_path(path, dst) < 0)
        return false;
    snprintf(info, sizeof info, TRASH_INFO "/%s.trashinfo", name);
    FILE *f = fopen(info, "w");
    if (f) {
        fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%lld\n", path, (long long)time(NULL));
        fclose(f);
    }
    if (trashed)
        snprintf(trashed, tn, "%s", name);
    recent_forget(path);
    return true;
}

static bool trash_origin(const char *name, char *out, size_t n) {
    char info[PATHN], line[PATHN + 16];
    snprintf(info, sizeof info, TRASH_INFO "/%s.trashinfo", name);
    FILE *f = fopen(info, "r");
    out[0] = 0;
    if (!f)
        return false;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "Path=", 5)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(out, n, "%s", line + 5);
        }
    fclose(f);
    return out[0] != 0;
}

static void mkdirs(const char *dir) {
    char p[PATHN];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

/* Back where it came from (beside it, if that name is taken now). */
static bool trash_restore(const char *name, char *restored, size_t rn) {
    char from[PATHN], src[PATHN], dir[PATHN], dst[PATHN], fresh[256], info[PATHN];
    join(src, sizeof src, TRASH_FILES, name);
    if (!trash_origin(name, from, sizeof from))
        join(from, sizeof from, HOME_DIR, name);
    dir_name(from, dir, sizeof dir);
    mkdirs(dir);
    unique_in(dir, base_name(from), false, fresh, sizeof fresh);
    join(dst, sizeof dst, dir, fresh);
    if (move_path(src, dst) < 0)
        return false;
    snprintf(info, sizeof info, TRASH_INFO "/%s.trashinfo", name);
    unlink(info);
    if (restored)
        snprintf(restored, rn, "%s", dst);
    return true;
}

void files_empty_trash(void) {
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *dir = pass ? TRASH_INFO : TRASH_FILES;
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char p[PATHN];
            join(p, sizeof p, dir, e->d_name);
            rm_rf(p);
            n += !pass;
        }
        if (d)
            closedir(d);
    }
    changed();
    say(n ? "Trash emptied (%d item%s erased)." : "Trash is already empty.", n, n == 1 ? "" : "s");
}

void files_setup(void) {
    static bool done;
    if (done)
        return;
    done = true;
    static const char *const dirs[] = {HOME_DIR "/Documents", HOME_DIR "/Downloads", HOME_DIR "/Pictures",
                                       TRASH_DIR, TRASH_FILES, TRASH_INFO, NULL};
    for (int i = 0; dirs[i]; i++)
        mkdir(dirs[i], dirs[i][strlen(HOME_DIR) + 1] == '.' ? 0700 : 0755);
}

/* ---- kinds ---- */

static const struct {
    const char *ext;
    int kind;
} kinds[] = {
    {"txt", FK_TEXT}, {"md", FK_TEXT}, {"log", FK_TEXT}, {"csv", FK_TEXT}, {"ini", FK_TEXT}, {"conf", FK_TEXT},
    {"cfg", FK_TEXT}, {"rst", FK_TEXT}, {"tex", FK_TEXT}, {"nfo", FK_TEXT}, {"srt", FK_TEXT},
    {"c", FK_CODE}, {"h", FK_CODE}, {"cpp", FK_CODE}, {"hpp", FK_CODE}, {"cc", FK_CODE}, {"py", FK_CODE},
    {"js", FK_CODE}, {"mjs", FK_CODE}, {"ts", FK_CODE}, {"tsx", FK_CODE}, {"jsx", FK_CODE}, {"rs", FK_CODE},
    {"go", FK_CODE}, {"java", FK_CODE}, {"kt", FK_CODE}, {"swift", FK_CODE}, {"zig", FK_CODE}, {"lua", FK_CODE},
    {"rb", FK_CODE}, {"php", FK_CODE}, {"css", FK_CODE}, {"html", FK_CODE}, {"htm", FK_CODE}, {"json", FK_CODE},
    {"xml", FK_CODE}, {"yaml", FK_CODE}, {"yml", FK_CODE}, {"toml", FK_CODE}, {"sql", FK_CODE}, {"svg", FK_CODE},
    {"sh", FK_CODE}, {"mk", FK_CODE},
    {"png", FK_IMAGE}, {"jpg", FK_IMAGE}, {"jpeg", FK_IMAGE}, {"gif", FK_IMAGE}, {"bmp", FK_IMAGE},
    {"tga", FK_IMAGE}, {"ppm", FK_IMAGE}, {"pgm", FK_IMAGE}, {"pnm", FK_IMAGE}, {"webp", FK_IMAGE},
    {"heic", FK_IMAGE},
    {"mp3", FK_AUDIO}, {"wav", FK_AUDIO}, {"ogg", FK_AUDIO}, {"flac", FK_AUDIO}, {"m4a", FK_AUDIO},
    {"opus", FK_AUDIO}, {"aiff", FK_AUDIO},
    {"mp4", FK_VIDEO}, {"mkv", FK_VIDEO}, {"webm", FK_VIDEO}, {"mov", FK_VIDEO}, {"avi", FK_VIDEO},
    {"zip", FK_ARCHIVE}, {"tar", FK_ARCHIVE}, {"gz", FK_ARCHIVE}, {"tgz", FK_ARCHIVE}, {"xz", FK_ARCHIVE},
    {"bz2", FK_ARCHIVE}, {"7z", FK_ARCHIVE}, {"rar", FK_ARCHIVE}, {"zst", FK_ARCHIVE}, {"cpio", FK_ARCHIVE},
    {"pdf", FK_DOC}, {"doc", FK_DOC}, {"docx", FK_DOC}, {"odt", FK_DOC}, {"rtf", FK_DOC}, {"xls", FK_DOC},
    {"xlsx", FK_DOC}, {"ods", FK_DOC}, {"ppt", FK_DOC}, {"pptx", FK_DOC}, {"epub", FK_DOC},
    {"appimage", FK_PROGRAM}, {"bin", FK_PROGRAM}, {"run", FK_PROGRAM}, {"elf", FK_PROGRAM},
    {"desktop", FK_APP},
};

int file_kind(const char *path, bool dir) {
    if (dir)
        return FK_FOLDER;
    const char *name = base_name(path), *dot = strrchr(name, '.');
    if (dot && dot != name)
        for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
            if (!strcasecmp(dot + 1, kinds[i].ext))
                return kinds[i].kind;
    static const char *const plain[] = {"README", "LICENSE", "COPYING", "NOTES", "TODO", "CHANGELOG", NULL};
    static const char *const code[] = {"Makefile", "Dockerfile", "CMakeLists.txt", NULL};
    for (int i = 0; plain[i]; i++)
        if (!strcasecmp(name, plain[i]))
            return FK_TEXT;
    for (int i = 0; code[i]; i++)
        if (!strcmp(name, code[i]))
            return FK_CODE;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111))
        return FK_PROGRAM;
    return FK_OTHER;
}

const char *file_kind_name(int k) {
    static const char *const names[FK_COUNT] = {"Folder", "Text",    "Code",     "Picture", "Sound", "Video",
                                                "Archive", "Document", "Program", "App",     "File"};
    return k >= 0 && k < FK_COUNT ? names[k] : "File";
}

/* Does the start of the file look like text? */
static bool looks_like_text(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    unsigned char buf[2048];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < 0)
        return false;
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == 0 || (buf[i] < 9 && buf[i] != 0x1b))
            return false;
    return true;
}

/* ---- glyphs and icons ---- */

static void ln(canvas *c, rect b, float x0, float y0, float x1, float y1, float w, color col) {
    draw_line(c, b.x + x0 * b.w, b.y + y0 * b.h, b.x + x1 * b.w, b.y + y1 * b.h, w, col);
}

static rect rel(rect b, float x, float y, float w, float h) {
    return (rect){(int)(b.x + x * b.w), (int)(b.y + y * b.h), (int)(w * b.w), (int)(h * b.h)};
}

void ui_glyph(canvas *c, rect b, int g, float w, color col) {
    float cx = b.x + b.w / 2.0f, cy = b.y + b.h / 2.0f, s = (float)(b.w < b.h ? b.w : b.h);
    switch (g) {
    case G_BACK:
        ln(c, b, 0.62f, 0.2f, 0.32f, 0.5f, w, col);
        ln(c, b, 0.32f, 0.5f, 0.62f, 0.8f, w, col);
        break;
    case G_FORWARD:
        ln(c, b, 0.38f, 0.2f, 0.68f, 0.5f, w, col);
        ln(c, b, 0.68f, 0.5f, 0.38f, 0.8f, w, col);
        break;
    case G_UP:
        ln(c, b, 0.5f, 0.2f, 0.5f, 0.8f, w, col);
        ln(c, b, 0.26f, 0.44f, 0.5f, 0.2f, w, col);
        ln(c, b, 0.5f, 0.2f, 0.74f, 0.44f, w, col);
        break;
    case G_LIST:
        for (int i = 0; i < 3; i++) {
            float y = 0.28f + i * 0.22f;
            fill_circle(c, b.x + 0.2f * b.w, b.y + y * b.h, w * 0.75f, col);
            ln(c, b, 0.36f, y, 0.82f, y, w, col);
        }
        break;
    case G_GRID:
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++)
                stroke_round(c, rel(b, 0.18f + i * 0.36f, 0.18f + j * 0.36f, 0.28f, 0.28f), s * 0.06f, w, col);
        break;
    case G_FOLDER:
    case G_NEWFOLDER:
        stroke_round(c, rel(b, 0.12f, 0.3f, 0.76f, 0.52f), s * 0.08f, w, col);
        ln(c, b, 0.14f, 0.3f, 0.2f, 0.2f, w, col);
        ln(c, b, 0.2f, 0.2f, 0.42f, 0.2f, w, col);
        ln(c, b, 0.42f, 0.2f, 0.48f, 0.3f, w, col);
        if (g == G_NEWFOLDER) {
            ln(c, b, 0.5f, 0.44f, 0.5f, 0.7f, w, col);
            ln(c, b, 0.37f, 0.57f, 0.63f, 0.57f, w, col);
        }
        break;
    case G_TRASH:
        ln(c, b, 0.18f, 0.28f, 0.82f, 0.28f, w, col);
        ln(c, b, 0.4f, 0.28f, 0.42f, 0.18f, w, col);
        ln(c, b, 0.42f, 0.18f, 0.58f, 0.18f, w, col);
        ln(c, b, 0.58f, 0.18f, 0.6f, 0.28f, w, col);
        ln(c, b, 0.26f, 0.28f, 0.31f, 0.84f, w, col);
        ln(c, b, 0.31f, 0.84f, 0.69f, 0.84f, w, col);
        ln(c, b, 0.69f, 0.84f, 0.74f, 0.28f, w, col);
        ln(c, b, 0.43f, 0.42f, 0.44f, 0.7f, w * 0.8f, col);
        ln(c, b, 0.57f, 0.42f, 0.56f, 0.7f, w * 0.8f, col);
        break;
    case G_HOME:
        ln(c, b, 0.16f, 0.48f, 0.5f, 0.18f, w, col);
        ln(c, b, 0.5f, 0.18f, 0.84f, 0.48f, w, col);
        ln(c, b, 0.26f, 0.4f, 0.26f, 0.82f, w, col);
        ln(c, b, 0.74f, 0.4f, 0.74f, 0.82f, w, col);
        ln(c, b, 0.26f, 0.82f, 0.74f, 0.82f, w, col);
        stroke_round(c, rel(b, 0.42f, 0.58f, 0.16f, 0.24f), s * 0.03f, w * 0.8f, col);
        break;
    case G_DOWNLOAD:
        ln(c, b, 0.5f, 0.14f, 0.5f, 0.6f, w, col);
        ln(c, b, 0.3f, 0.42f, 0.5f, 0.62f, w, col);
        ln(c, b, 0.5f, 0.62f, 0.7f, 0.42f, w, col);
        ln(c, b, 0.18f, 0.66f, 0.18f, 0.84f, w, col);
        ln(c, b, 0.18f, 0.84f, 0.82f, 0.84f, w, col);
        ln(c, b, 0.82f, 0.84f, 0.82f, 0.66f, w, col);
        break;
    case G_PICTURE:
        stroke_round(c, rel(b, 0.14f, 0.2f, 0.72f, 0.6f), s * 0.08f, w, col);
        ln(c, b, 0.2f, 0.72f, 0.42f, 0.48f, w, col);
        ln(c, b, 0.42f, 0.48f, 0.58f, 0.64f, w, col);
        ln(c, b, 0.54f, 0.6f, 0.64f, 0.52f, w, col);
        ln(c, b, 0.64f, 0.52f, 0.8f, 0.7f, w, col);
        fill_circle(c, b.x + 0.66f * b.w, b.y + 0.34f * b.h, s * 0.06f, col);
        break;
    case G_CLOCK:
        draw_arc(c, cx, cy, s * 0.34f, 0, 6.2832f, w, col);
        ln(c, b, 0.5f, 0.5f, 0.5f, 0.3f, w, col);
        ln(c, b, 0.5f, 0.5f, 0.64f, 0.58f, w, col);
        break;
    case G_SPEAKER:
        ln(c, b, 0.16f, 0.4f, 0.16f, 0.6f, w, col);
        ln(c, b, 0.16f, 0.4f, 0.3f, 0.4f, w, col);
        ln(c, b, 0.16f, 0.6f, 0.3f, 0.6f, w, col);
        ln(c, b, 0.3f, 0.4f, 0.48f, 0.22f, w, col);
        ln(c, b, 0.3f, 0.6f, 0.48f, 0.78f, w, col);
        ln(c, b, 0.48f, 0.22f, 0.48f, 0.78f, w, col);
        draw_arc(c, b.x + 0.5f * b.w, cy, s * 0.16f, -0.8f, 0.8f, w, col);
        draw_arc(c, b.x + 0.5f * b.w, cy, s * 0.3f, -0.8f, 0.8f, w, col);
        break;
    case G_SEARCH:
        draw_arc(c, cx - s * 0.06f, cy - s * 0.06f, s * 0.24f, 0, 6.2832f, w, col);
        ln(c, b, 0.62f, 0.62f, 0.82f, 0.82f, w, col);
        break;
    case G_MOON:
        draw_arc(c, cx, cy, s * 0.3f, 1.0f, 5.6f, w, col);
        draw_arc(c, cx + s * 0.16f, cy - s * 0.1f, s * 0.24f, 1.9f, 4.4f, w, col);
        break;
    case G_SUN:
        draw_arc(c, cx, cy, s * 0.16f, 0, 6.2832f, w, col);
        for (int i = 0; i < 8; i++) {
            float a = i * 0.785398f;
            draw_line(c, cx + cosf(a) * s * 0.26f, cy + sinf(a) * s * 0.26f, cx + cosf(a) * s * 0.36f,
                      cy + sinf(a) * s * 0.36f, w, col);
        }
        break;
    case G_RESTORE:
        draw_arc(c, cx, cy + s * 0.04f, s * 0.28f, 3.6f, 3.6f + 4.6f, w, col);
        ln(c, b, 0.22f, 0.24f, 0.24f, 0.44f, w, col);
        ln(c, b, 0.24f, 0.44f, 0.44f, 0.42f, w, col);
        break;
    case G_SORT:
        ln(c, b, 0.18f, 0.28f, 0.82f, 0.28f, w, col);
        ln(c, b, 0.18f, 0.5f, 0.64f, 0.5f, w, col);
        ln(c, b, 0.18f, 0.72f, 0.46f, 0.72f, w, col);
        break;
    case G_DESKTOP:
        stroke_round(c, rel(b, 0.12f, 0.18f, 0.76f, 0.5f), s * 0.06f, w, col);
        ln(c, b, 0.5f, 0.68f, 0.5f, 0.82f, w, col);
        ln(c, b, 0.32f, 0.82f, 0.68f, 0.82f, w, col);
        break;
    case G_CALC:
        stroke_round(c, rel(b, 0.2f, 0.12f, 0.6f, 0.76f), s * 0.08f, w, col);
        ln(c, b, 0.32f, 0.3f, 0.68f, 0.3f, w, col);
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 3; i++)
                fill_circle(c, b.x + (0.34f + i * 0.16f) * b.w, b.y + (0.5f + j * 0.18f) * b.h, w * 0.8f, col);
        break;
    case G_DOC:
        stroke_round(c, rel(b, 0.22f, 0.14f, 0.56f, 0.72f), s * 0.07f, w, col);
        ln(c, b, 0.34f, 0.38f, 0.66f, 0.38f, w * 0.85f, col);
        ln(c, b, 0.34f, 0.52f, 0.66f, 0.52f, w * 0.85f, col);
        ln(c, b, 0.34f, 0.66f, 0.54f, 0.66f, w * 0.85f, col);
        break;
    case G_MUSIC:
        ln(c, b, 0.4f, 0.7f, 0.4f, 0.22f, w, col);
        ln(c, b, 0.4f, 0.22f, 0.74f, 0.16f, w, col);
        ln(c, b, 0.74f, 0.16f, 0.74f, 0.62f, w, col);
        fill_circle(c, b.x + 0.32f * b.w, b.y + 0.72f * b.h, s * 0.1f, col);
        fill_circle(c, b.x + 0.66f * b.w, b.y + 0.64f * b.h, s * 0.1f, col);
        break;
    case G_POWER:
        draw_arc(c, cx, cy + s * 0.04f, s * 0.3f, -1.05f - 3.1416f * 0.5f + 3.1416f, 4.19f - 3.1416f * 0.5f + 3.1416f,
                 w, col);
        ln(c, b, 0.5f, 0.14f, 0.5f, 0.46f, w, col);
        break;
    case G_TEXT: {
        font *f = font_get(D.font_heading, s * 0.62f);
        draw_text_centered(c, f, b, "Aa", col);
        break;
    }
    case G_SPARK:
        ln(c, b, 0.5f, 0.14f, 0.5f, 0.86f, w, col);
        ln(c, b, 0.14f, 0.5f, 0.86f, 0.5f, w, col);
        ln(c, b, 0.3f, 0.3f, 0.7f, 0.7f, w * 0.7f, col);
        ln(c, b, 0.7f, 0.3f, 0.3f, 0.7f, w * 0.7f, col);
        break;
    case G_GEAR:
        for (int i = 0; i < 8; i++) {
            float a = i * 0.785398f;
            draw_line(c, cx + cosf(a) * s * 0.24f, cy + sinf(a) * s * 0.24f, cx + cosf(a) * s * 0.36f,
                      cy + sinf(a) * s * 0.36f, w * 1.3f, col);
        }
        draw_arc(c, cx, cy, s * 0.23f, 0, 6.2832f, w, col);
        draw_arc(c, cx, cy, s * 0.08f, 0, 6.2832f, w, col);
        break;
    case G_TERMINAL:
        stroke_round(c, rel(b, 0.1f, 0.18f, 0.8f, 0.64f), s * 0.1f, w, col);
        ln(c, b, 0.26f, 0.38f, 0.38f, 0.5f, w, col);
        ln(c, b, 0.38f, 0.5f, 0.26f, 0.62f, w, col);
        ln(c, b, 0.46f, 0.64f, 0.66f, 0.64f, w, col);
        break;
    case G_EYE:
        draw_arc(c, cx, cy + s * 0.3f, s * 0.42f, 3.82f, 5.6f, w, col);
        draw_arc(c, cx, cy - s * 0.3f, s * 0.42f, 0.68f, 2.46f, w, col);
        draw_arc(c, cx, cy, s * 0.1f, 0, 6.2832f, w, col);
        break;
    }
}

/* Kinds keep familiar hues, nudged toward the design's accent so they sit
 * in its palette rather than on top of it. */
static color kind_color(int k) {
    static const color base[FK_COUNT] = {0,
                                         RGBA(112, 124, 146, 255), /* text */
                                         RGBA(74, 110, 224, 255),  /* code */
                                         RGBA(36, 160, 118, 255),  /* picture */
                                         RGBA(226, 74, 130, 255),  /* sound */
                                         RGBA(136, 84, 220, 255),  /* video */
                                         RGBA(214, 146, 40, 255),  /* archive */
                                         RGBA(220, 78, 58, 255),   /* document */
                                         RGBA(52, 56, 66, 255),    /* program */
                                         RGBA(52, 56, 66, 255),    /* app */
                                         RGBA(132, 138, 150, 255)};
    if (k == FK_FOLDER)
        return D.accent;
    return color_mix(base[k < 0 || k >= FK_COUNT ? FK_OTHER : k], D.accent, 0.16f);
}

void file_icon(canvas *c, rect r, int kind, const char *name, bool selected) {
    (void)selected;
    float s = (float)(r.w < r.h ? r.w : r.h);
    rect b = {r.x + (int)((r.w - s) / 2), r.y + (int)((r.h - s) / 2), (int)s, (int)s};
    color white = RGBA(255, 255, 255, 255), black = RGBA(0, 0, 0, 255);
    if (kind == FK_FOLDER) {
        color a = D.accent;
        color back = color_mix(a, black, 0.22f);
        fill_round(c, rel(b, 0.06f, 0.14f, 0.4f, 0.24f), s * 0.06f, back);
        fill_round(c, rel(b, 0.06f, 0.22f, 0.88f, 0.64f), s * 0.08f, back);
        rect front = rel(b, 0.06f, 0.32f, 0.88f, 0.54f);
        fill_round_vgradient(c, front, s * 0.08f, color_mix(a, white, 0.3f), color_mix(a, white, 0.05f));
        if (s >= 28)
            stroke_round_vgradient(c, front, s * 0.08f, 1, RGBA(255, 255, 255, 120), RGBA(255, 255, 255, 0));
        return;
    }
    color kc = kind_color(kind);
    if (kind == FK_PROGRAM || kind == FK_APP) {
        rect t = rel(b, 0.1f, 0.1f, 0.8f, 0.8f);
        if (s >= 40 && D.shadows)
            shadow_round(c, (rect){t.x, t.y + (int)(s * 0.04f), t.w, t.h}, s * 0.18f, s * 0.12f, RGBA(0, 0, 0, 70));
        fill_round_vgradient(c, t, s * 0.2f, color_mix(kc, white, 0.18f), kc);
        ui_glyph(c, rel(t, 0.15f, 0.15f, 0.7f, 0.7f), kind == FK_APP ? G_SPARK : G_TERMINAL, s * 0.05f + 0.6f,
                 RGBA(255, 255, 255, 235));
        return;
    }
    /* a sheet of paper, with what kind of thing it holds */
    rect pg = rel(b, 0.17f, 0.06f, 0.66f, 0.88f);
    float pr = s * 0.07f;
    if (s >= 40 && D.shadows)
        shadow_round(c, (rect){pg.x, pg.y + (int)(s * 0.03f), pg.w, pg.h}, pr, s * 0.1f, RGBA(0, 0, 0, 60));
    fill_round(c, pg, pr, RGBA(251, 251, 253, 255));
    stroke_round(c, pg, pr, 1, RGBA(0, 0, 0, 36));
    float w = s * 0.035f + 0.5f;
    rect art = rel(pg, 0.18f, 0.12f, 0.64f, s >= 40 ? 0.46f : 0.76f);
    switch (kind) {
    case FK_IMAGE:
        ui_glyph(c, art, G_PICTURE, w, kc);
        break;
    case FK_AUDIO:
        ui_glyph(c, art, G_MUSIC, w, kc);
        break;
    case FK_VIDEO:
        stroke_round(c, rel(art, 0.08f, 0.16f, 0.84f, 0.68f), s * 0.04f, w, kc);
        ln(c, art, 0.42f, 0.34f, 0.42f, 0.66f, w, kc);
        ln(c, art, 0.42f, 0.34f, 0.64f, 0.5f, w, kc);
        ln(c, art, 0.42f, 0.66f, 0.64f, 0.5f, w, kc);
        break;
    case FK_ARCHIVE:
        for (int i = 0; i < 4; i++)
            fill_rect(c, rel(art, i % 2 ? 0.5f : 0.38f, 0.08f + i * 0.2f, 0.12f, 0.1f), kc);
        stroke_round(c, rel(art, 0.36f, 0.82f, 0.28f, 0.18f), s * 0.02f, w, kc);
        break;
    case FK_CODE:
        ln(c, art, 0.32f, 0.3f, 0.12f, 0.5f, w, kc);
        ln(c, art, 0.12f, 0.5f, 0.32f, 0.7f, w, kc);
        ln(c, art, 0.68f, 0.3f, 0.88f, 0.5f, w, kc);
        ln(c, art, 0.88f, 0.5f, 0.68f, 0.7f, w, kc);
        ln(c, art, 0.56f, 0.24f, 0.44f, 0.76f, w, color_alpha(kc, 0.7f));
        break;
    default:
        for (int i = 0; i < 4; i++)
            ln(c, art, 0.06f, 0.18f + i * 0.22f, i == 3 ? 0.6f : 0.94f, 0.18f + i * 0.22f, w, color_alpha(kc, 0.75f));
        break;
    }
    /* the extension, on a band of the kind's colour, when there is room */
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (s >= 40 && dot && dot != name && dot[1]) {
        char ext[6];
        int i = 0;
        for (const char *p = dot + 1; *p && i < 4; p++)
            ext[i++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        ext[i] = 0;
        rect band = rel(pg, 0.12f, 0.64f, 0.76f, 0.22f);
        fill_round(c, band, s * 0.04f, kc);
        draw_text_centered(c, font_get("Inter", s * 0.15f), band, ext, readable_on(kc));
    }
}

/* ---- text helpers ---- */

/* `s` cut to fit `maxw`, ending in an ellipsis if it had to be cut. */
static void fit(font *f, const char *s, int maxw, char *out, size_t n) {
    snprintf(out, n, "%s", s);
    if (text_width(f, out) <= maxw)
        return;
    int lo = 0, hi = (int)strlen(out);
    char tmp[600];
    while (lo < hi) { /* the longest prefix that fits with "…" */
        int mid = (lo + hi + 1) / 2;
        int m = mid;
        while (m > 0 && (s[m] & 0xC0) == 0x80)
            m--;
        snprintf(tmp, sizeof tmp, "%.*s\xE2\x80\xA6", m, s);
        if (text_width(f, tmp) <= maxw)
            lo = mid;
        else
            hi = mid - 1;
    }
    while (lo > 0 && (s[lo] & 0xC0) == 0x80)
        lo--;
    snprintf(out, n, "%.*s\xE2\x80\xA6", lo, s);
}

/* Two centred lines for grid labels: break at a space, dot or dash when
 * one is near, and end the second line with "…" if it still doesn't fit. */
static void two_lines(font *f, const char *s, int maxw, char *l1, char *l2, size_t n) {
    l2[0] = 0;
    if (text_width(f, s) <= maxw) {
        snprintf(l1, n, "%s", s);
        return;
    }
    int len = (int)strlen(s), cut = 0;
    for (int i = 1; i <= len; i++) {
        if (i < len && (s[i] & 0xC0) == 0x80)
            continue;
        if (text_width_n(f, s, i) > maxw)
            break;
        cut = i;
    }
    int brk = cut;
    for (int i = cut; i > cut / 2; i--)
        if (s[i - 1] == ' ' || s[i - 1] == '-' || s[i - 1] == '_' || s[i] == '.') {
            brk = i;
            break;
        }
    if (brk <= 0)
        brk = cut > 0 ? cut : 1;
    snprintf(l1, n, "%.*s", brk, s);
    fit(f, s + brk + (s[brk] == ' '), maxw, l2, n);
}

static void fmt_size(long long sz, char *out, size_t n) {
    if (sz < 1000)
        snprintf(out, n, "%lld byte%s", sz, sz == 1 ? "" : "s");
    else if (sz < 1000000)
        snprintf(out, n, "%.0f KB", sz / 1000.0);
    else if (sz < 1000000000)
        snprintf(out, n, "%.1f MB", sz / 1e6);
    else
        snprintf(out, n, "%.2f GB", sz / 1e9);
}

static void fmt_date(time_t t, char *out, size_t n) {
    time_t now = time(NULL);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        strftime(out, n, "Today, %H:%M", &a);
    else if (a.tm_year == b.tm_year && b.tm_yday - a.tm_yday == 1)
        strftime(out, n, "Yesterday, %H:%M", &a);
    else if (a.tm_year == b.tm_year)
        strftime(out, n, "%d %b, %H:%M", &a);
    else
        strftime(out, n, "%d %b %Y", &a);
}

/* "~/Documents" for places under home. */
static void pretty_dir(const char *p, char *out, size_t n) {
    if (inside(p, HOME_DIR))
        snprintf(out, n, "Home%s", p + strlen(HOME_DIR));
    else
        snprintf(out, n, "%s", p);
}

/* ---- the folder ---- */

static bool in_trash(void) { return !strcmp(F.path, TRASH_FILES); }

static int cmp_ent(const void *pa, const void *pb) {
    const struct ent *a = pa, *b = pb;
    if (a->dir != b->dir)
        return a->dir ? -1 : 1; /* folders first, as both Finder and Explorer can */
    int r = 0;
    switch (F.sort) {
    case S_SIZE: r = a->size < b->size ? -1 : a->size > b->size; break;
    case S_DATE: r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime; break;
    case S_KIND: r = strcmp(file_kind_name(a->kind), file_kind_name(b->kind)); break;
    }
    if (!r)
        r = strcasecmp(a->name, b->name);
    return F.desc ? -r : r;
}

static void load(void) {
    if (!F.e)
        F.e = calloc(MAX_ENTS, sizeof *F.e);
    if (!F.e)
        return;
    bool same = !strcmp(F.loaded, F.path);
    /* remember what was selected, by name */
    static char keep[256][256];
    int nkeep = 0;
    char cursor_name[256] = "";
    if (same) {
        for (int i = 0; i < F.n && nkeep < 256; i++)
            if (F.e[i].sel)
                snprintf(keep[nkeep++], 256, "%s", F.e[i].name);
        if (F.cursor >= 0 && F.cursor < F.n)
            snprintf(cursor_name, sizeof cursor_name, "%s", F.e[F.cursor].name);
    }
    F.n = 0;
    DIR *d = opendir(F.path);
    F.readable = d != NULL;
    struct dirent *de;
    bool trash = in_trash();
    while (d && (de = readdir(d)) && F.n < MAX_ENTS) {
        if (de->d_name[0] == '.') /* hidden things stay hidden (Trash among them) */
            continue;
        char full[PATHN];
        join(full, sizeof full, F.path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 && lstat(full, &st) != 0)
            continue;
        struct ent *e = &F.e[F.n++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        e->dir = S_ISDIR(st.st_mode);
        e->size = (long long)st.st_size;
        e->mtime = st.st_mtime;
        e->mode = st.st_mode;
        e->kind = e->dir ? FK_FOLDER : file_kind(full, false);
        if (trash) {
            char from[PATHN], dir[PATHN];
            if (trash_origin(e->name, from, sizeof from)) {
                dir_name(from, dir, sizeof dir);
                pretty_dir(dir, e->origin, sizeof e->origin);
            }
        }
    }
    if (d)
        closedir(d);
    qsort(F.e, (size_t)F.n, sizeof *F.e, cmp_ent);
    F.cursor = -1;
    for (int i = 0; i < F.n; i++) {
        for (int k = 0; k < nkeep; k++)
            if (!strcmp(F.e[i].name, keep[k]))
                F.e[i].sel = true;
        if (cursor_name[0] && !strcmp(F.e[i].name, cursor_name))
            F.cursor = i;
        if (F.select_after[0] && !strcmp(F.e[i].name, F.select_after)) {
            for (int j = 0; j < i; j++)
                F.e[j].sel = false;
            F.e[i].sel = true;
            F.cursor = F.anchor = i;
        } else if (F.select_after[0]) {
            F.e[i].sel = false;
        }
    }
    if (F.select_after[0] && F.rename_after && F.cursor >= 0) {
        F.renaming = F.cursor;
        snprintf(F.rn.buf, sizeof F.rn.buf, "%s", F.e[F.cursor].name);
        F.rn.len = (int)strlen(F.rn.buf);
    }
    F.select_after[0] = 0;
    F.rename_after = false;
    if (F.anchor >= F.n)
        F.anchor = -1;
    snprintf(F.loaded, sizeof F.loaded, "%s", F.path);
    F.loaded_at = A.now;
    F.reload = false;
}

static void resort(void) {
    char cur[256] = "";
    if (F.cursor >= 0 && F.cursor < F.n)
        snprintf(cur, sizeof cur, "%s", F.e[F.cursor].name);
    qsort(F.e, (size_t)F.n, sizeof *F.e, cmp_ent);
    for (int i = 0; i < F.n; i++)
        if (cur[0] && !strcmp(F.e[i].name, cur))
            F.cursor = F.anchor = i;
}

static void close_viewer(void);

static void go(const char *path, bool record) {
    if (record && strcmp(path, F.path)) {
        if (F.nback == 24) {
            memmove(F.back[0], F.back[1], sizeof F.back[0] * 23);
            F.nback--;
        }
        snprintf(F.back[F.nback++], PATHN, "%s", F.path);
        F.nfwd = 0;
    }
    snprintf(F.path, sizeof F.path, "%s", path);
    F.scroll = 0;
    F.cursor = F.anchor = -1;
    F.renaming = -1;
    F.menu_on = false;
    F.reload = true;
    F.loaded[0] = 0;
    close_viewer();
    LOG("files: at %s\n", F.path);
}

static void go_back(void) {
    if (!F.nback)
        return;
    if (F.nfwd < 24)
        snprintf(F.fwd[F.nfwd++], PATHN, "%s", F.path);
    char p[PATHN];
    snprintf(p, sizeof p, "%s", F.back[--F.nback]);
    go(p, false);
}

static void go_forward(void) {
    if (!F.nfwd)
        return;
    if (F.nback < 24)
        snprintf(F.back[F.nback++], PATHN, "%s", F.path);
    char p[PATHN];
    snprintf(p, sizeof p, "%s", F.fwd[--F.nfwd]);
    go(p, false);
}

static bool can_go_up(void) { return strcmp(F.path, "/") && strcmp(F.path, TRASH_FILES); }

static void go_up(void) {
    if (!can_go_up())
        return;
    char p[PATHN], name[256];
    snprintf(name, sizeof name, "%s", base_name(F.path));
    dir_name(F.path, p, sizeof p);
    go(p, true);
    snprintf(F.select_after, sizeof F.select_after, "%s", name); /* land on where we were */
}

/* ---- selection ---- */

static int sel_count(void) {
    int n = 0;
    for (int i = 0; i < F.n; i++)
        n += F.e[i].sel;
    return n;
}

static void select_none(void) {
    for (int i = 0; i < F.n; i++)
        F.e[i].sel = false;
}

static void select_only(int i) {
    select_none();
    if (i >= 0 && i < F.n) {
        F.e[i].sel = true;
        F.cursor = F.anchor = i;
    }
}

static void select_range(int a, int b, bool add) {
    if (!add)
        select_none();
    if (a < 0)
        a = b;
    for (int i = a < b ? a : b; i <= (a < b ? b : a); i++)
        if (i >= 0 && i < F.n)
            F.e[i].sel = true;
}

static void ent_path(int i, char *out, size_t n) { join(out, n, F.path, F.e[i].name); }

/* ---- the picture viewer ---- */

static void close_viewer(void) {
    if (!F.viewing)
        return;
    F.viewing = false;
    image_free(F.view_full);
    image_free(F.view_fit);
    F.view_full = F.view_fit = NULL;
}

static void open_viewer(const char *path) {
    close_viewer();
    F.viewing = true;
    F.view_failed = false;
    snprintf(F.view_path, sizeof F.view_path, "%s", path);
    F.view_full = image_load(path, 2400);
    F.view_failed = F.view_full == NULL;
    LOG("files: viewing %s (%s)\n", path, F.view_failed ? "can't read it" : "ok");
}

/* The next or previous picture in this folder, in its sorted order. */
static void viewer_step(int dir) {
    int at = -1;
    for (int i = 0; i < F.n; i++) {
        char p[PATHN];
        ent_path(i, p, sizeof p);
        if (!strcmp(p, F.view_path))
            at = i;
    }
    for (int k = 1; k <= F.n && at >= 0; k++) {
        int i = ((at + dir * k) % F.n + F.n) % F.n;
        if (!F.e[i].dir && image_name_ok(F.e[i].name)) {
            char p[PATHN];
            ent_path(i, p, sizeof p);
            select_only(i);
            open_viewer(p);
            return;
        }
    }
}

/* ---- opening ---- */

void spawn_command(const char *cmd, const char *name) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
        }
        for (int i = 3; i < 256; i++)
            close(i);
        become_session_user();
        if (chdir(HOME_DIR) < 0) { /* run from wherever we are */
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    LOG("spawned %s (pid %d): %s\n", name, pid, cmd);
    if (pid > 0)
        toast("Opening %s...", name);
    else
        toast("%s couldn't be started.", name);
}

static void show_files_window(void) {
    if (A.ui)
        fm_host_action(A.ui, NULL, "open:files");
}

void files_open_path(const char *path) {
    struct stat st;
    files_setup();
    if (stat(path, &st) != 0) {
        toast("%s isn't there any more.", base_name(path));
        recent_forget(path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        go(path, true);
        show_files_window();
        return;
    }
    int k = file_kind(path, false);
    const char *name = base_name(path);
    LOG("files: open %s (%s)\n", path, file_kind_name(k));
    recent_add('f', name, path);
    if (k == FK_IMAGE && image_name_ok(name)) {
        char dir[PATHN];
        dir_name(path, dir, sizeof dir);
        if (strcmp(dir, F.path))
            go(dir, true);
        snprintf(F.select_after, sizeof F.select_after, "%s", name);
        open_viewer(path);
        show_files_window();
    } else if (k == FK_APP) {
        if (!launcher_run_desktop_file(path))
            toast("%s doesn't say how to start it.", name);
    } else if (strlen(name) > 9 && !strcasecmp(name + strlen(name) - 9, ".AppImage")) {
        /* an app downloaded as an AppImage: unpacked once and run (run-appimage) */
        char cmd[PATHN + 32];
        snprintf(cmd, sizeof cmd, "/usr/bin/run-appimage '%s'", path);
        spawn_command(cmd, name);
    } else if (k == FK_PROGRAM || (S_ISREG(st.st_mode) && (st.st_mode & 0111) && k != FK_TEXT && k != FK_CODE)) {
        char cmd[PATHN + 8];
        snprintf(cmd, sizeof cmd, "'%s'", path); /* paths here never hold a quote: see rename */
        spawn_command(cmd, name);
    } else if (k == FK_TEXT || k == FK_CODE || (k == FK_OTHER && looks_like_text(path))) {
        open_in_notes(path);
    } else {
        const char *dot = strrchr(name, '.');
        toast("Nothing on this computer opens %s files yet.", dot ? dot : file_kind_name(k));
    }
}

void files_reveal(const char *path) {
    char dir[PATHN];
    dir_name(path, dir, sizeof dir);
    go(dir, true);
    snprintf(F.select_after, sizeof F.select_after, "%s", base_name(path));
    show_files_window();
}

static void open_ent(int i) {
    if (i < 0 || i >= F.n)
        return;
    char p[PATHN];
    ent_path(i, p, sizeof p);
    if (F.e[i].dir)
        go(p, true);
    else if (in_trash())
        say("Put %s back before opening it.", F.e[i].name);
    else
        files_open_path(p);
}

/* ---- operations on the selection ---- */

static void op_trash(void) {
    int n = 0;
    char last[256] = "";
    F.nundo = 0;
    static char undo[32][300];
    int nundo = 0;
    for (int i = 0; i < F.n; i++) {
        if (!F.e[i].sel)
            continue;
        char p[PATHN], t[256];
        ent_path(i, p, sizeof p);
        if (trash_put(p, t, sizeof t)) {
            n++;
            snprintf(last, sizeof last, "%s", F.e[i].name);
            if (nundo < 32)
                snprintf(undo[nundo++], 300, "%s", t);
        }
    }
    if (!n)
        return;
    changed();
    if (n == 1)
        say("Moved %s to Trash.", last);
    else
        say("Moved %d items to Trash.", n);
    memcpy(F.undo, undo, sizeof undo);
    F.nundo = nundo;
}

static void op_undo_trash(void) {
    int n = 0;
    for (int i = 0; i < F.nundo; i++)
        n += trash_restore(F.undo[i], NULL, 0);
    F.nundo = 0;
    changed();
    say("Put back %d item%s.", n, n == 1 ? "" : "s");
}

static void op_restore(void) {
    int n = 0;
    char where[PATHN] = "";
    for (int i = 0; i < F.n; i++)
        if (F.e[i].sel && trash_restore(F.e[i].name, where, sizeof where))
            n++;
    if (!n)
        return;
    changed();
    char dir[PATHN], pretty[PATHN];
    dir_name(where, dir, sizeof dir);
    pretty_dir(dir, pretty, sizeof pretty);
    if (n == 1)
        say("Put back in %s.", pretty);
    else
        say("Put back %d items.", n);
}

static void op_erase(void) {
    int n = 0;
    for (int i = 0; i < F.n; i++) {
        if (!F.e[i].sel)
            continue;
        char p[PATHN], info[PATHN];
        ent_path(i, p, sizeof p);
        snprintf(info, sizeof info, TRASH_INFO "/%s.trashinfo", F.e[i].name);
        if (rm_rf(p) == 0)
            n++;
        unlink(info);
    }
    changed();
    say("Erased %d item%s for good.", n, n == 1 ? "" : "s");
}

static void op_new_folder(void) {
    if (in_trash())
        return;
    char name[256], p[PATHN];
    unique_in(F.path, "New folder", false, name, sizeof name);
    join(p, sizeof p, F.path, name);
    if (mkdir(p, 0755) < 0) {
        say("A folder can't be made here.");
        return;
    }
    LOG("files: new folder %s\n", p);
    snprintf(F.select_after, sizeof F.select_after, "%s", name);
    F.rename_after = true;
    changed();
}

static void op_copy(bool cut) {
    F.nclip = 0;
    for (int i = 0; i < F.n && F.nclip < 64; i++)
        if (F.e[i].sel)
            ent_path(i, F.clip[F.nclip++], PATHN);
    F.clip_cut = cut;
    if (F.nclip)
        say("%s %d item%s. Paste with Ctrl+V.", cut ? "Cut" : "Copied", F.nclip, F.nclip == 1 ? "" : "s");
}

/* Moves or copies paths into a folder. Returns how many arrived. */
static int bring(char (*paths)[PATHN], int n, const char *dir, bool move, char *first, size_t fn) {
    int done = 0;
    for (int i = 0; i < n; i++) {
        const char *src = paths[i];
        char srcdir[PATHN], name[256], dst[PATHN];
        dir_name(src, srcdir, sizeof srcdir);
        if (!exists(src))
            continue;
        if (inside(dir, src)) { /* into itself */
            say("%s can't go inside itself.", base_name(src));
            continue;
        }
        if (move && !strcmp(srcdir, dir))
            continue; /* already there */
        unique_in(dir, base_name(src), !move && !strcmp(srcdir, dir), name, sizeof name);
        join(dst, sizeof dst, dir, name);
        int r = move ? move_path(src, dst) : copy_rec(src, dst);
        if (r == 0) {
            done++;
            if (first && done == 1)
                snprintf(first, fn, "%s", name);
            if (move)
                recent_forget(src);
        }
    }
    if (done)
        changed();
    return done;
}

static void op_paste(void) {
    if (!F.nclip || in_trash())
        return;
    char first[256] = "";
    int n = bring(F.clip, F.nclip, F.path, F.clip_cut, first, sizeof first);
    if (F.clip_cut)
        F.nclip = 0;
    if (n) {
        snprintf(F.select_after, sizeof F.select_after, "%s", first);
        say("Pasted %d item%s.", n, n == 1 ? "" : "s");
    }
}

static void start_rename(int i) {
    if (i < 0 || i >= F.n || in_trash())
        return;
    select_only(i);
    F.renaming = i;
    snprintf(F.rn.buf, sizeof F.rn.buf, "%s", F.e[i].name);
    F.rn.len = (int)strlen(F.rn.buf);
}

static void commit_rename(void) {
    int i = F.renaming;
    F.renaming = -1;
    if (i < 0 || i >= F.n)
        return;
    char *nm = F.rn.buf;
    while (*nm == ' ')
        nm++;
    for (int l = (int)strlen(nm); l > 0 && nm[l - 1] == ' '; l--)
        nm[l - 1] = 0;
    if (!*nm || !strcmp(nm, F.e[i].name))
        return;
    if (strchr(nm, '/') || strchr(nm, '\'') || !strcmp(nm, ".") || !strcmp(nm, "..")) {
        say("A name can't contain / or ' here.");
        return;
    }
    char a[PATHN], b[PATHN];
    ent_path(i, a, sizeof a);
    join(b, sizeof b, F.path, nm);
    if (exists(b)) {
        say("There's already something called %s here.", nm);
        return;
    }
    if (rename(a, b) < 0) {
        say("%s couldn't be renamed.", F.e[i].name);
        return;
    }
    recent_forget(a);
    LOG("files: renamed %s to %s\n", F.e[i].name, nm);
    snprintf(F.select_after, sizeof F.select_after, "%s", nm);
    changed();
}

/* ---- the context menu ---- */

static void menu_add(const char *label, int id, bool enabled, bool checked, bool sep) {
    if (F.nm < 20)
        F.mi[F.nm++] = (struct mitem){label, id, enabled, checked, sep};
}

static void menu_open(int x, int y, int target) {
    F.nm = 0;
    bool trash = in_trash();
    if (target >= 0) {
        if (!F.e[target].sel)
            select_only(target);
        int n = sel_count();
        bool one = n == 1;
        if (trash) {
            menu_add("Put Back", M_RESTORE, true, false, false);
            menu_add("Erase for Good", M_ERASE, true, false, true);
        } else {
            menu_add("Open", M_OPEN, one, false, false);
            if (one && !F.e[target].dir && image_name_ok(F.e[target].name))
                menu_add("Quick Look", M_QUICKLOOK, true, false, false);
            if (one && !F.e[target].dir && F.e[target].kind != FK_TEXT && F.e[target].kind != FK_CODE &&
                F.e[target].kind != FK_IMAGE)
                menu_add("Open in Notes", M_OPEN_NOTES, true, false, false);
            menu_add("Rename", M_RENAME, one, false, true);
            menu_add("Copy", M_COPY, true, false, false);
            menu_add("Cut", M_CUT, true, false, false);
            menu_add("Paste", M_PASTE, F.nclip > 0 && F.e[target].dir && one, false, false);
            menu_add("Move to Trash", M_TRASH, true, false, true);
        }
    } else if (target == -1) {
        select_none();
        if (trash) {
            menu_add("Empty Trash", M_EMPTY_TRASH, F.n > 0, false, false);
        } else {
            menu_add("New Folder", M_NEW_FOLDER, true, false, false);
            menu_add("Paste", M_PASTE, F.nclip > 0, false, false);
        }
        menu_add("Select All", M_SELECT_ALL, F.n > 0, false, true);
        menu_add("As List", M_VIEW_LIST, true, F.view == V_LIST, true);
        menu_add("As Icons", M_VIEW_GRID, true, F.view == V_GRID, false);
    }
    if (target == -1 || target == -2) {
        menu_add("Sort by Name", M_SORT_NAME, true, F.sort == S_NAME, target == -1);
        menu_add("Sort by Size", M_SORT_SIZE, true, F.sort == S_SIZE, false);
        menu_add("Sort by Date", M_SORT_DATE, true, F.sort == S_DATE, false);
        menu_add("Sort by Kind", M_SORT_KIND, true, F.sort == S_KIND, false);
    }
    font *f = FB(D.body_px - 1);
    int w = 0, lh = font_line_height(f) + 12, h = 10;
    for (int i = 0; i < F.nm; i++) {
        int tw = text_width(f, F.mi[i].label);
        if (tw > w)
            w = tw;
        h += lh + (F.mi[i].sep ? 9 : 0);
    }
    w += 64;
    F.menu_r = (rect){x, y, w, h};
    F.menu_on = F.nm > 0;
    F.menu_target = target;
    LOG("files: menu open (%d items)\n", F.nm);
}

static void sort_by(int s) {
    if (F.sort == s)
        F.desc = !F.desc;
    else
        F.sort = s, F.desc = s == S_DATE || s == S_SIZE; /* newest and largest first */
    resort();
}

static void menu_run(int id, int target) {
    LOG("files: menu %d\n", id);
    char p[PATHN];
    switch (id) {
    case M_OPEN: open_ent(target >= 0 ? target : F.cursor); break;
    case M_QUICKLOOK:
        ent_path(target, p, sizeof p);
        open_viewer(p);
        break;
    case M_OPEN_NOTES:
        ent_path(target, p, sizeof p);
        open_in_notes(p);
        break;
    case M_RENAME: start_rename(target); break;
    case M_COPY: op_copy(false); break;
    case M_CUT: op_copy(true); break;
    case M_PASTE:
        if (target >= 0 && F.e[target].dir) {
            ent_path(target, p, sizeof p);
            int n = bring(F.clip, F.nclip, p, F.clip_cut, NULL, 0);
            if (F.clip_cut)
                F.nclip = 0;
            say("Pasted %d item%s into %s.", n, n == 1 ? "" : "s", F.e[target].name);
        } else {
            op_paste();
        }
        break;
    case M_TRASH: op_trash(); break;
    case M_RESTORE: op_restore(); break;
    case M_ERASE: op_erase(); break;
    case M_EMPTY_TRASH: files_empty_trash(); break;
    case M_NEW_FOLDER: op_new_folder(); break;
    case M_SELECT_ALL: select_range(0, F.n - 1, false); break;
    case M_VIEW_LIST: F.view = V_LIST, F.view_chosen = true; break;
    case M_VIEW_GRID: F.view = V_GRID, F.view_chosen = true; break;
    case M_SORT_NAME: sort_by(S_NAME); break;
    case M_SORT_SIZE: sort_by(S_SIZE); break;
    case M_SORT_DATE: sort_by(S_DATE); break;
    case M_SORT_KIND: sort_by(S_KIND); break;
    }
}

/* ---- thumbnails ---- */

#define NTHUMB 64
static struct thumb {
    char path[PATHN];
    time_t mtime;
    long long size;
    int side;
    image *im;
    bool failed;
    uint64_t used;
} TH[NTHUMB];
static int thumb_budget;

/* A small copy of a picture, decoded at most one per frame so a folder of
 * photos fills in over a moment instead of freezing the screen. */
static image *thumb(const char *path, time_t mt, long long sz, int side) {
    int lru = 0;
    for (int i = 0; i < NTHUMB; i++) {
        struct thumb *t = &TH[i];
        if (t->path[0] && t->mtime == mt && t->size == sz && t->side == side && !strcmp(t->path, path)) {
            t->used = A.now;
            return t->im;
        }
        if (t->used < TH[lru].used)
            lru = i;
    }
    if (thumb_budget <= 0 || sz > 32 * 1024 * 1024)
        return NULL;
    thumb_budget--;
    struct thumb *t = &TH[lru];
    image_free(t->im);
    snprintf(t->path, sizeof t->path, "%s", path);
    t->mtime = mt, t->size = sz, t->side = side, t->used = A.now;
    t->im = image_load(path, side);
    t->failed = !t->im;
    return t->im;
}

/* ---- input ---- */

static bool hov(rect r) { return pt_in(r, I.mx, I.my); }

/* Is the pointer over Files, and not over a window or menu in front of it? */
static bool pointer_mine(rect body, bool in_window) {
    int mx = A.in.mx, my = A.in.my;
    if (!pt_in(body, mx, my))
        return false;
    if (A.ui && (fm_hit_layer(A.ui, 2, mx, my) || fm_hit_layer(A.ui, 1, mx, my)))
        return false;
    int z = in_window ? A.win[WIN_FILES].z : -1;
    for (int w = 0; w < WIN_MAX; w++) {
        if ((in_window && w == WIN_FILES) || !A.win[w].open || A.win[w].minimized)
            continue;
        if (A.win[w].z > z && pt_in(A.win[w].r, mx, my))
            return false;
    }
    return true;
}

static bool keyboard_mine(bool in_window) {
    if (launcher_active() || (A.ui && A.ui->focus) || A.confirm_power)
        return false;
    int top = -1;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized && (top < 0 || A.win[w].z > A.win[top].z))
            top = w;
    return in_window ? top == WIN_FILES : top < 0;
}

struct combos {
    bool copy, cut, all, newfolder, f2, back, fwd, up, ctrl;
};

/* Shortcuts, read from the raw key events in the order they happened, so
 * a quick Ctrl+C is seen even when Ctrl was let go within the same frame. */
static struct combos read_combos(void) {
    static bool ctrl, shift, alt;
    struct combos k = {0};
    if (!A.in.nraw)
        ctrl = A.in.ctrl, shift = A.in.shift, alt = false;
    for (int i = 0; i < A.in.nraw; i++) {
        int code = A.in.raw[i].code;
        bool down = A.in.raw[i].down;
        if (code == RK_LCTRL || code == RK_RCTRL)
            ctrl = down;
        else if (code == RK_LSHIFT || code == RK_RSHIFT)
            shift = down;
        else if (code == RK_LALT || code == RK_RALT)
            alt = down;
        else if (down) {
            if (ctrl && code == RK_C) k.copy = true;
            if (ctrl && code == RK_X) k.cut = true;
            if (ctrl && code == RK_A) k.all = true;
            if (ctrl && shift && code == RK_N) k.newfolder = true;
            if (code == RK_F2) k.f2 = true;
            if (alt && code == RK_LEFT) k.back = true;
            if (alt && code == RK_RIGHT) k.fwd = true;
            if (alt && code == RK_UP) k.up = true;
        }
    }
    k.ctrl = ctrl || A.in.ctrl;
    return k;
}

static void keep_cursor_visible(rect content) {
    if (F.cursor < 0 || F.cursor >= F.n)
        return;
    rect r = F.e[F.cursor].r;
    int top = content.y, bottom = content.y + content.h;
    if (r.y < top)
        F.scroll -= top - r.y + 4;
    else if (r.y + r.h > bottom)
        F.scroll += r.y + r.h - bottom + 4;
}

static void move_cursor(int to, bool extend) {
    if (!F.n)
        return;
    if (to < 0)
        to = 0;
    if (to >= F.n)
        to = F.n - 1;
    if (extend) {
        if (F.anchor < 0)
            F.anchor = F.cursor >= 0 ? F.cursor : to;
        select_range(F.anchor, to, false);
        F.cursor = to;
    } else {
        select_only(to);
    }
}

static void keyboard(rect content, bool kb) {
    struct combos k = read_combos();
    if (!kb)
        return;
    if (F.renaming >= 0) {
        for (int i = 0; i < I.nkeys; i++) {
            int key = I.keys[i];
            if (key == K_ESC)
                F.renaming = -1;
            else if (key == K_ENTER)
                commit_rename();
            else if (key == K_BACKSPACE && F.rn.len) {
                do
                    F.rn.len--;
                while (F.rn.len && (F.rn.buf[F.rn.len] & 0xC0) == 0x80);
                F.rn.buf[F.rn.len] = 0;
            } else if (key == K_PASTE) {
                for (const char *p = clipboard_text(); *p && F.rn.len < 250; p++)
                    if ((unsigned char)*p >= 32)
                        F.rn.buf[F.rn.len++] = *p;
                F.rn.buf[F.rn.len] = 0;
            } else if (!k.ctrl && key >= 32 && key < 0x80 && F.rn.len < 250) {
                F.rn.buf[F.rn.len++] = (char)key;
                F.rn.buf[F.rn.len] = 0;
            } else if (!k.ctrl && key >= 0x80 && key < 0x800 && F.rn.len < 249) {
                F.rn.buf[F.rn.len++] = (char)(0xC0 | key >> 6);
                F.rn.buf[F.rn.len++] = (char)(0x80 | (key & 0x3f));
                F.rn.buf[F.rn.len] = 0;
            }
        }
        return;
    }
    if (k.copy) op_copy(false);
    if (k.cut) op_copy(true);
    if (k.all) select_range(0, F.n - 1, false);
    if (k.newfolder) op_new_folder();
    if (k.f2) start_rename(F.cursor >= 0 ? F.cursor : -1);
    if (k.back) go_back();
    if (k.fwd) go_forward();
    if (k.up) go_up();
    if (k.back || k.fwd || k.up)
        return;
    int cols = F.view == V_GRID ? (F.grid_cols > 0 ? F.grid_cols : 1) : 1;
    bool moved = false;
    for (int i = 0; i < I.nkeys; i++) {
        int key = I.keys[i];
        int cur = F.cursor;
        switch (key) {
        case K_DOWN: move_cursor(cur < 0 ? 0 : cur + cols, I.shift), moved = true; break;
        case K_UP: move_cursor(cur < 0 ? 0 : cur - cols, I.shift), moved = true; break;
        case K_RIGHT:
            if (F.view == V_GRID)
                move_cursor(cur + 1, I.shift), moved = true;
            break;
        case K_LEFT:
            if (F.view == V_GRID)
                move_cursor(cur < 0 ? 0 : cur - 1, I.shift), moved = true;
            break;
        case K_HOME: move_cursor(0, I.shift), moved = true; break;
        case K_END: move_cursor(F.n - 1, I.shift), moved = true; break;
        case K_PGDN: move_cursor(cur + cols * 8, I.shift), moved = true; break;
        case K_PGUP: move_cursor(cur - cols * 8, I.shift), moved = true; break;
        case K_ENTER: open_ent(F.cursor); return;
        case K_BACKSPACE: go_up(); return;
        case K_DELETE:
            if (!sel_count())
                break;
            if (!in_trash())
                op_trash();
            else if (A.now - F.erase_confirm_at < 4000)
                op_erase(), F.erase_confirm_at = 0;
            else {
                F.erase_confirm_at = A.now;
                say("Press Delete again to erase %s for good.", sel_count() == 1 ? "it" : "them");
            }
            break;
        case K_ESC:
            if (F.menu_on)
                F.menu_on = false;
            else
                select_none();
            break;
        case K_PASTE: op_paste(); break;
        case ' ':
            if (!F.ahead[0] || A.now - F.ahead_at > 900) {
                if (F.cursor >= 0 && !F.e[F.cursor].dir && image_name_ok(F.e[F.cursor].name)) {
                    char p[PATHN];
                    ent_path(F.cursor, p, sizeof p);
                    open_viewer(p);
                }
                break;
            }
            __attribute__((fallthrough)); /* a space inside a name being typed */
        default:
            if (k.ctrl || key < 32 || key >= 0x80)
                break;
            /* typing a name jumps to it */
            if (A.now - F.ahead_at > 900)
                F.ahead[0] = 0;
            F.ahead_at = A.now;
            size_t l = strlen(F.ahead);
            if (l < sizeof F.ahead - 1)
                F.ahead[l] = (char)key, F.ahead[l + 1] = 0;
            for (int j = 0; j < F.n; j++)
                if (!strncasecmp(F.e[j].name, F.ahead, strlen(F.ahead))) {
                    select_only(j);
                    moved = true;
                    break;
                }
        }
    }
    if (moved)
        keep_cursor_visible(content);
}

/* ---- drawing pieces ---- */

static color sel_bg(void) { return focused ? D.accent : color_alpha(D.surface_text, 0.16f); }
static color sel_fg(void) { return focused ? readable_on(D.accent) : D.surface_text; }

static bool tool_button(rect r, int glyph, bool enabled, bool on) {
    bool h = enabled && hov(r);
    if (on)
        fill_round(A.frame, r, D.radius * 0.45f + 2, color_alpha(D.surface_text, 0.14f));
    else if (h)
        fill_round(A.frame, r, D.radius * 0.45f + 2, color_alpha(D.surface_text, I.down ? 0.14f : 0.08f));
    rect g = {r.x + (r.w - 18) / 2, r.y + (r.h - 18) / 2, 18, 18};
    ui_glyph(A.frame, g, glyph, 1.6f, color_alpha(D.surface_text, enabled ? 0.9f : 0.28f));
    return h && I.clicked;
}

static bool text_button(rect r, const char *label, font *f, bool primary) {
    bool h = hov(r);
    color bg = primary ? D.accent : color_alpha(D.surface_text, h ? 0.14f : 0.08f);
    if (primary && h)
        bg = color_mix(D.accent, RGBA(255, 255, 255, 255), 0.12f);
    fill_round(A.frame, r, D.radius * 0.45f + 2, bg);
    draw_text_centered(A.frame, f, r, label, primary ? readable_on(D.accent) : D.surface_text);
    return h && I.clicked;
}

/* Rects for tests, logged only when something moved (tests/files_test.py). */
static struct {
    char name[64];
    rect r;
} logged[160];
static int nlogged;
static uint64_t log_hash, log_hash_prev;

static void lrect(const char *fmt, rect r, ...) __attribute__((format(printf, 1, 3)));
static void lrect(const char *fmt, rect r, ...) {
    if (nlogged >= 160)
        return;
    va_list ap;
    va_start(ap, r);
    vsnprintf(logged[nlogged].name, sizeof logged[nlogged].name, fmt, ap);
    va_end(ap);
    logged[nlogged].r = r;
    for (const char *p = logged[nlogged].name; *p; p++)
        log_hash = (log_hash ^ (uint8_t)*p) * 1099511628211ull;
    int v[4] = {r.x, r.y, r.w, r.h};
    for (int i = 0; i < 4; i++)
        log_hash = (log_hash ^ (uint32_t)v[i]) * 1099511628211ull;
    nlogged++;
}

static void flush_rects(void) {
    if (log_hash != log_hash_prev) {
        log_hash_prev = log_hash;
        for (int i = 0; i < nlogged; i++) {
            /* names with spaces would split the log line: dashes instead */
            for (char *p = logged[i].name; *p; p++)
                if (*p == ' ')
                    *p = '-';
            LOGRECT(logged[i].name, logged[i].r);
        }
    }
    nlogged = 0;
    log_hash = 1469598103934665603ull;
}

/* ---- the sidebar ---- */

static const struct {
    const char *label, *path;
    int glyph;
    bool optional;
} places[] = {
    {"Home", HOME_DIR, G_HOME, false},
    {"Documents", HOME_DIR "/Documents", G_DOC, false},
    {"Downloads", HOME_DIR "/Downloads", G_DOWNLOAD, false},
    {"Pictures", HOME_DIR "/Pictures", G_PICTURE, false},
    {"Music", HOME_DIR "/Music", G_MUSIC, true},
    {"Projects", HOME_DIR "/Projects", G_FOLDER, true},
    {"Trash", TRASH_FILES, G_TRASH, false},
};

static void drop_target(const char *dir, rect r);

static void sidebar(rect sb) {
    font *f = FB(D.body_px - 1), *small = FB(D.body_px - 4);
    fill_round(A.frame, sb, D.radius * 0.6f, color_alpha(D.surface_text, 0.035f));
    int x = sb.x + 8, w = sb.w - 16, y = sb.y + 12;
    int rh = font_line_height(f) + 12;
    draw_text(A.frame, small, x + 8, y, "PLACES", D.muted);
    y += font_line_height(small) + 6;
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++) {
        if (places[i].optional && !is_dir(places[i].path))
            continue;
        rect r = {x, y, w, rh};
        bool here = !strcmp(F.path, places[i].path);
        bool target = F.dragging && hov(r);
        lrect("files-place-%s", r, places[i].label);
        if (here)
            fill_round(A.frame, r, D.radius * 0.45f + 2, color_alpha(D.accent, 0.2f));
        else if (hov(r))
            fill_round(A.frame, r, D.radius * 0.45f + 2, color_alpha(D.surface_text, 0.07f));
        if (target) {
            drop_target(places[i].path, r);
        }
        color ink = here ? D.accent : color_alpha(D.surface_text, 0.8f);
        if (here && color_contrast(D.accent, D.surface) < 3)
            ink = D.surface_text;
        ui_glyph(A.frame, (rect){r.x + 8, r.y + (rh - 18) / 2, 18, 18}, places[i].glyph, 1.5f, ink);
        char lab[64];
        fit(f, places[i].label, w - 44, lab, sizeof lab);
        draw_text(A.frame, f, r.x + 34, r.y + (rh - font_line_height(f)) / 2, lab, here ? D.surface_text : color_alpha(D.surface_text, 0.86f));
        if (!F.dragging && hov(r) && I.clicked && !here)
            go(places[i].path, true);
        y += rh + 2;
    }
    /* what they opened lately */
    const struct recent *rec;
    int nr = recent_list(&rec), shown = 0;
    for (int i = 0; i < nr && shown < 6; i++) {
        if (rec[i].kind != 'f' || !exists(rec[i].target))
            continue;
        if (y + rh > sb.y + sb.h - 8)
            break;
        if (!shown) {
            y += 12;
            draw_text(A.frame, small, x + 8, y, "RECENT", D.muted);
            y += font_line_height(small) + 6;
        }
        rect r = {x, y, w, rh};
        if (hov(r))
            fill_round(A.frame, r, D.radius * 0.45f + 2, color_alpha(D.surface_text, 0.07f));
        file_icon(A.frame, (rect){r.x + 7, r.y + (rh - 20) / 2, 20, 20}, file_kind(rec[i].target, false), rec[i].title,
                  false);
        char lab[128];
        fit(f, rec[i].title, w - 44, lab, sizeof lab);
        draw_text(A.frame, f, r.x + 34, r.y + (rh - font_line_height(f)) / 2, lab, color_alpha(D.surface_text, 0.86f));
        lrect("files-recent-%d", r, shown);
        if (!F.dragging && hov(r) && I.clicked) {
            char t[512];
            snprintf(t, sizeof t, "%s", rec[i].target);
            files_open_path(t);
            return;
        }
        y += rh + 2;
        shown++;
    }
}

/* While dragging: is the pointer over somewhere things can be dropped? */
static void drop_target(const char *dir, rect r) {
    for (int i = 0; i < F.ndrag; i++) {
        char from[PATHN];
        dir_name(F.drag[i], from, sizeof from);
        if (inside(dir, F.drag[i]) || !strcmp(from, dir))
            return; /* into itself, or where it already is */
    }
    snprintf(F.drop, sizeof F.drop, "%s", dir);
    fill_round(A.frame, r, D.radius * 0.45f + 2, color_alpha(D.accent, 0.3f));
    stroke_round(A.frame, r, D.radius * 0.45f + 2, 2, D.accent);
}

/* ---- the toolbar and breadcrumbs ---- */

static void toolbar(rect tb) {
    font *f = FB(D.body_px - 1), *hf = FH(D.body_px + 1);
    int bh = 30, y = tb.y + (tb.h - bh) / 2, x = tb.x;
    rect b1 = {x, y, 32, bh}, b2 = {x + 34, y, 32, bh}, b3 = {x + 68, y, 32, bh};
    lrect("files-back", b1);
    lrect("files-forward", b2);
    lrect("files-up", b3);
    if (tool_button(b1, G_BACK, F.nback > 0, false)) go_back();
    if (tool_button(b2, G_FORWARD, F.nfwd > 0, false)) go_forward();
    if (tool_button(b3, G_UP, can_go_up(), false)) go_up();

    /* right side: view, sort, new folder (or Empty in Trash) */
    int rx = tb.x + tb.w;
    bool trash = in_trash();
    if (trash) {
        int w = text_width(f, "Empty Trash") + 28;
        rect eb = {rx - w, y, w, bh};
        if (F.n) {
            rx -= w + 8;
            lrect("files-empty-trash", eb);
            if (text_button(eb, "Empty Trash", f, false))
                files_empty_trash();
        }
        if (sel_count()) {
            int w2 = text_width(f, "Put Back") + 28;
            rect pb = {rx - w2, y, w2, bh};
            rx -= w2 + 8;
            lrect("files-put-back", pb);
            if (text_button(pb, "Put Back", f, true))
                op_restore();
        }
    } else {
        rect nf = {rx - 32, y, 32, bh};
        rx -= 40;
        lrect("files-new-folder", nf);
        if (tool_button(nf, G_NEWFOLDER, true, false))
            op_new_folder();
    }
    rect sb = {rx - 32, y, 32, bh};
    rx -= 40;
    lrect("files-sort", sb);
    if (tool_button(sb, G_SORT, true, false))
        menu_open(sb.x, sb.y + sb.h + 4, -2);
    rect seg = {rx - 66, y, 66, bh};
    rx -= 74;
    fill_round(A.frame, seg, D.radius * 0.45f + 2, color_alpha(D.surface_text, 0.05f));
    rect vl = {seg.x, y, 33, bh}, vg = {seg.x + 33, y, 33, bh};
    lrect("files-view-list", vl);
    lrect("files-view-grid", vg);
    if (tool_button(vl, G_LIST, true, F.view == V_LIST)) F.view = V_LIST, F.view_chosen = true;
    if (tool_button(vg, G_GRID, true, F.view == V_GRID)) F.view = V_GRID, F.view_chosen = true;

    /* breadcrumbs: Home › Documents › Letters, each a place to click or drop on */
    int cx = tb.x + 112, cmax = rx - 8;
    struct {
        char label[128], path[PATHN];
    } seg_[24];
    int ns = 0;
    if (trash) {
        snprintf(seg_[0].label, sizeof seg_[0].label, "Trash");
        snprintf(seg_[0].path, sizeof seg_[0].path, "%s", TRASH_FILES);
        ns = 1;
    } else {
        const char *rest = F.path;
        if (inside(F.path, HOME_DIR)) {
            snprintf(seg_[0].label, sizeof seg_[0].label, "Home");
            snprintf(seg_[0].path, sizeof seg_[0].path, HOME_DIR);
            rest = F.path + strlen(HOME_DIR);
        } else {
            snprintf(seg_[0].label, sizeof seg_[0].label, "Computer");
            snprintf(seg_[0].path, sizeof seg_[0].path, "/");
        }
        ns = 1;
        size_t base = (size_t)(rest - F.path);
        for (const char *p = rest; *p && ns < 24;) {
            while (*p == '/')
                p++;
            if (!*p)
                break;
            const char *e = strchr(p, '/');
            int l = e ? (int)(e - p) : (int)strlen(p);
            snprintf(seg_[ns].label, sizeof seg_[ns].label, "%.*s", l, p);
            snprintf(seg_[ns].path, sizeof seg_[ns].path, "%.*s", (int)(base + (size_t)(p - rest) + (size_t)l), F.path);
            ns++;
            p += l;
        }
    }
    /* drop leading crumbs (behind "…") until the rest fits */
    int first = 0, sep = 22;
    for (;;) {
        int w = first ? text_width(f, "\xE2\x80\xA6") + sep : 0;
        for (int i = first; i < ns; i++)
            w += text_width(i == ns - 1 ? hf : f, seg_[i].label) + 16 + (i < ns - 1 ? sep : 0);
        if (cx + w <= cmax || first >= ns - 1)
            break;
        first++;
    }
    if (first) {
        draw_text(A.frame, f, cx, y + (bh - font_line_height(f)) / 2, "\xE2\x80\xA6", D.muted);
        cx += text_width(f, "\xE2\x80\xA6") + 4;
        ui_glyph(A.frame, (rect){cx, y + bh / 2 - 7, 14, 14}, G_FORWARD, 1.4f, D.muted);
        cx += sep - 4;
    }
    for (int i = first; i < ns; i++) {
        bool last = i == ns - 1;
        font *ff = last ? hf : f;
        char lab[128];
        fit(ff, seg_[i].label, cmax - cx - 16 > 40 ? cmax - cx - 16 : 40, lab, sizeof lab);
        rect r = {cx, y, text_width(ff, lab) + 16, bh};
        lrect("files-crumb-%d", r, i);
        if (F.dragging && hov(r))
            drop_target(seg_[i].path, r);
        else if (!last && hov(r))
            fill_round(A.frame, r, D.radius * 0.45f + 2, color_alpha(D.surface_text, 0.08f));
        draw_text(A.frame, ff, r.x + 8, y + (bh - font_line_height(ff)) / 2, lab,
                  last ? D.surface_text : color_alpha(D.surface_text, 0.66f));
        if (!last && !F.dragging && hov(r) && I.clicked)
            go(seg_[i].path, true);
        cx += r.w;
        if (!last) {
            ui_glyph(A.frame, (rect){cx + 1, y + bh / 2 - 7, 14, 14}, G_FORWARD, 1.4f, color_alpha(D.surface_text, 0.4f));
            cx += sep - 6;
        }
    }
}

/* ---- the folder's contents ---- */

static int list_row_h(void) { return font_line_height(FB(D.body_px - 1)) + 14; }

/* Where everything goes, for drawing and for hit-testing alike. */
static void place_items(rect content, int header) {
    if (F.view == V_LIST) {
        int rh = list_row_h();
        for (int i = 0; i < F.n; i++)
            F.e[i].r = (rect){content.x, content.y + header + i * rh - F.scroll, content.w, rh};
        F.content_h = header + F.n * rh + 8;
        F.grid_cols = 1;
    } else {
        int cw = 112, ch = 122, cols = content.w / cw;
        if (cols < 1)
            cols = 1;
        int spare = (content.w - cols * cw) / (cols + 1);
        if (spare > 24)
            spare = 24;
        for (int i = 0; i < F.n; i++)
            F.e[i].r = (rect){content.x + spare + (i % cols) * (cw + spare), content.y + 8 + (i / cols) * ch - F.scroll,
                              cw, ch - 6};
        F.content_h = 16 + ((F.n + cols - 1) / cols) * ch;
        F.grid_cols = cols;
    }
}

static int item_at(int x, int y, rect content, int header) {
    if (!pt_in((rect){content.x, content.y + header, content.w, content.h - header}, x, y))
        return -1;
    for (int i = 0; i < F.n; i++) {
        rect r = F.e[i].r;
        if (F.view == V_GRID)
            r = rect_inset(r, 4);
        if (pt_in(r, x, y))
            return i;
    }
    return -1;
}

static bool overlaps(rect a, rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* The name being edited, in place of the label. */
static void rename_field(rect r, font *f) {
    fill_round(A.frame, rect_inset(r, -2), 5, D.accent);
    fill_round(A.frame, r, 4, color_mix(D.surface, RGBA(255, 255, 255, 255), color_luma(D.surface) > 0.5f ? 0.6f : 0.08f));
    color ink = color_luma(D.surface) > 0.5f ? RGBA(20, 18, 22, 255) : D.surface_text;
    canvas_clip(A.frame, r);
    int tw = text_width(f, F.rn.buf), x = r.x + 6;
    if (tw > r.w - 14)
        x = r.x + r.w - 8 - tw;
    int ty = r.y + (r.h - font_line_height(f)) / 2;
    draw_text(A.frame, f, x, ty, F.rn.buf, ink);
    if ((A.now / 530) % 2 == 0)
        fill_rect(A.frame, (rect){x + tw + 1, ty + 2, 2, font_line_height(f) - 4}, ink);
    canvas_unclip(A.frame);
    /* restore the window's clip for whatever comes next */
}

static void draw_list(rect content, int header) {
    font *f = FB(D.body_px - 1), *small = FB(D.body_px - 3);
    bool trash = in_trash();
    int wkind = content.w > 560 ? 120 : 0, wdate = content.w > 430 ? 150 : 0, wsize = 86;
    int xname = content.x + 10, xdate = content.x + content.w - wkind - wsize - wdate - 8;
    int xsize = xdate + wdate, xkind = xsize + wsize;
    /* the header: click a column to sort by it, again to reverse */
    rect hr = {content.x, content.y, content.w, header};
    struct {
        const char *label;
        int x, w, sort;
    } cols[4] = {{"Name", xname, xdate - xname, S_NAME},
                 {"Modified", xdate, wdate, S_DATE},
                 {"Size", xsize, wsize, S_SIZE},
                 {trash ? "From" : "Kind", xkind, wkind, S_KIND}};
    for (int c = 0; c < 4; c++) {
        if (!cols[c].w)
            continue;
        rect r = {cols[c].x - 6, hr.y + 2, cols[c].w, header - 4};
        lrect("files-col-%s", r, cols[c].label);
        bool on = F.sort == cols[c].sort;
        if (hov(r))
            fill_round(A.frame, r, 4, color_alpha(D.surface_text, 0.06f));
        draw_text(A.frame, small, cols[c].x, hr.y + (header - font_line_height(small)) / 2, cols[c].label,
                  on ? D.surface_text : D.muted);
        if (on) { /* a small chevron: up for ascending, down for descending */
            int tw = text_width(small, cols[c].label);
            rect a = {cols[c].x + tw + 6, hr.y + header / 2 - 4, 9, 8};
            float t = F.desc ? 0 : 1, bt = 1 - t;
            color ac = color_alpha(D.surface_text, 0.6f);
            ln(A.frame, a, 0, bt, 0.5f, t, 1.4f, ac);
            ln(A.frame, a, 0.5f, t, 1, bt, 1.4f, ac);
        }
        if (hov(r) && I.clicked && !F.dragging)
            sort_by(cols[c].sort);
    }
    fill_rect(A.frame, (rect){content.x, content.y + header - 1, content.w, 1}, color_alpha(D.surface_text, 0.08f));
    canvas_clip(A.frame, (rect){content.x, content.y + header, content.w, content.h - header});
    for (int i = 0; i < F.n; i++) {
        struct ent *e = &F.e[i];
        rect r = e->r;
        if (r.y + r.h < content.y + header || r.y > content.y + content.h)
            continue;
        bool target = F.dragging && e->dir && hov(r) && !e->sel;
        if (e->sel)
            fill_round(A.frame, rect_inset(r, 1), D.radius * 0.35f + 2, sel_bg());
        else if (i % 2)
            fill_rect(A.frame, r, color_alpha(D.surface_text, 0.022f));
        if (!e->sel && hov(r) && !F.banding && !F.dragging)
            fill_round(A.frame, rect_inset(r, 1), D.radius * 0.35f + 2, color_alpha(D.surface_text, 0.06f));
        if (target) {
            char p[PATHN];
            ent_path(i, p, sizeof p);
            drop_target(p, rect_inset(r, 1));
        }
        color ink = e->sel ? sel_fg() : D.surface_text;
        color sub = e->sel ? color_alpha(sel_fg(), 0.8f) : D.muted;
        int icon = r.h - 10;
        file_icon(A.frame, (rect){xname, r.y + 5, icon, icon}, e->kind, e->name, e->sel);
        int ty = r.y + (r.h - font_line_height(f)) / 2, sy = r.y + (r.h - font_line_height(small)) / 2;
        rect namer = {xname + icon + 8, r.y + 3, xdate - xname - icon - 22, r.h - 6};
        if (F.renaming == i) {
            rename_field(namer, f);
            canvas_clip(A.frame, (rect){content.x, content.y + header, content.w, content.h - header});
        } else {
            char nm[300];
            fit(f, e->name, namer.w, nm, sizeof nm);
            draw_text(A.frame, f, namer.x, ty, nm, ink);
        }
        char buf[200];
        if (wdate) {
            fmt_date(e->mtime, buf, sizeof buf);
            draw_text(A.frame, small, xdate, sy, buf, sub);
        }
        if (e->dir)
            snprintf(buf, sizeof buf, "\xE2\x80\x94");
        else
            fmt_size(e->size, buf, sizeof buf);
        draw_text(A.frame, small, xsize, sy, buf, sub);
        if (wkind) {
            char k[200];
            fit(small, trash ? (e->origin[0] ? e->origin : "?") : file_kind_name(e->kind), wkind - 8, k, sizeof k);
            draw_text(A.frame, small, xkind, sy, k, sub);
        }
        if (i < 40)
            lrect("files-item-%s", r, e->name);
    }
    canvas_unclip(A.frame);
}

static void draw_grid(rect content) {
    font *f = FB(D.body_px - 2);
    canvas_clip(A.frame, content);
    for (int i = 0; i < F.n; i++) {
        struct ent *e = &F.e[i];
        rect r = e->r;
        if (r.y + r.h < content.y || r.y > content.y + content.h)
            continue;
        rect ib = {r.x + (r.w - 64) / 2, r.y + 6, 64, 64};
        bool target = F.dragging && e->dir && hov(r) && !e->sel;
        if (e->sel)
            fill_round(A.frame, rect_inset(ib, -6), D.radius * 0.5f + 4, color_alpha(D.surface_text, 0.12f));
        else if (hov(r) && !F.banding && !F.dragging)
            fill_round(A.frame, rect_inset(r, 2), D.radius * 0.5f + 4, color_alpha(D.surface_text, 0.05f));
        if (target) {
            char p[PATHN];
            ent_path(i, p, sizeof p);
            drop_target(p, rect_inset(r, 2));
        }
        image *t = NULL;
        if (e->kind == FK_IMAGE && image_name_ok(e->name)) {
            char p[PATHN];
            ent_path(i, p, sizeof p);
            t = thumb(p, e->mtime, e->size, 64);
        }
        if (t) {
            int tx = ib.x + (64 - t->w) / 2, ty = ib.y + (64 - t->h) / 2;
            if (D.shadows)
                shadow_round(A.frame, (rect){tx, ty + 3, t->w, t->h}, 3, 8, RGBA(0, 0, 0, 70));
            image_draw(A.frame, t, tx, ty, 1);
            stroke_round(A.frame, (rect){tx, ty, t->w, t->h}, 2, 1, RGBA(255, 255, 255, 60));
        } else {
            file_icon(A.frame, ib, e->kind, e->name, e->sel);
        }
        rect namer = {r.x + 4, ib.y + 70, r.w - 8, r.h - 76};
        if (F.renaming == i) {
            rename_field((rect){namer.x - 6, namer.y, namer.w + 12, font_line_height(f) + 8}, f);
            canvas_clip(A.frame, content);
        } else {
            char l1[300], l2[300];
            two_lines(f, e->name, namer.w - 8, l1, l2, sizeof l1);
            int lh = font_line_height(f);
            if (e->sel) { /* the name on a pill of the accent, like Finder */
                int w1 = text_width(f, l1), w2 = l2[0] ? text_width(f, l2) : 0, w = (w1 > w2 ? w1 : w2) + 12;
                fill_round(A.frame, (rect){r.x + (r.w - w) / 2, namer.y - 1, w, lh * (l2[0] ? 2 : 1) + 4}, 6, sel_bg());
            }
            color ink = e->sel ? sel_fg() : D.surface_text;
            draw_text(A.frame, f, r.x + (r.w - text_width(f, l1)) / 2, namer.y + 1, l1, ink);
            if (l2[0])
                draw_text(A.frame, f, r.x + (r.w - text_width(f, l2)) / 2, namer.y + 1 + lh, l2, ink);
        }
        if (i < 40)
            lrect("files-item-%s", r, e->name);
    }
    canvas_unclip(A.frame);
}

static void empty_state(rect content) {
    const char *title, *hint;
    int glyph = G_FOLDER;
    if (!F.readable) {
        title = "This folder can't be opened";
        hint = "It may have been moved or deleted.";
    } else if (in_trash()) {
        glyph = G_TRASH;
        title = "Trash is empty";
        hint = "Things you delete wait here until you empty it.";
    } else if (!strcmp(F.path, HOME_DIR "/Pictures")) {
        glyph = G_PICTURE;
        title = "No pictures yet";
        hint = "Pictures you keep here show up as thumbnails.";
    } else if (!strcmp(F.path, HOME_DIR "/Downloads")) {
        glyph = G_DOWNLOAD;
        title = "Nothing downloaded yet";
        hint = "Drag things here to keep them for later.";
    } else {
        title = "This folder is empty";
        hint = "Drag things here, or right-click to make a new folder.";
    }
    int cy = content.y + content.h / 2 - 60;
    fill_circle(A.frame, content.x + content.w / 2.0f, cy + 28.0f, 46, color_alpha(D.surface_text, 0.05f));
    ui_glyph(A.frame, (rect){content.x + content.w / 2 - 26, cy + 2, 52, 52}, glyph, 2.0f, color_alpha(D.surface_text, 0.45f));
    draw_text_centered(A.frame, FH(D.body_px + 3), (rect){content.x, cy + 76, content.w, 30}, title, D.surface_text);
    draw_text_centered(A.frame, FB(D.body_px - 2), (rect){content.x, cy + 106, content.w, 26}, hint, D.muted);
    lrect("files-empty", content);
}

static void status_bar(rect sb) {
    font *small = FB(D.body_px - 3);
    fill_rect(A.frame, (rect){sb.x, sb.y, sb.w, 1}, color_alpha(D.surface_text, 0.07f));
    char left[120];
    int ns = sel_count();
    if (ns)
        snprintf(left, sizeof left, "%d of %d selected", ns, F.n);
    else
        snprintf(left, sizeof left, "%d item%s", F.n, F.n == 1 ? "" : "s");
    if (F.nclip)
        snprintf(left + strlen(left), sizeof left - strlen(left), "  \xC2\xB7  %d on the clipboard", F.nclip);
    int ty = sb.y + (sb.h - font_line_height(small)) / 2 + 1;
    draw_text(A.frame, small, sb.x + 10, ty, left, D.muted);
    if (F.status[0] && A.now < F.status_until) {
        int x = sb.x + sb.w - 10;
        if (F.nundo) {
            int w = text_width(small, "Undo") + 18;
            rect ub = {x - w, sb.y + 3, w, sb.h - 6};
            lrect("files-undo", ub);
            bool h = hov(ub);
            fill_round(A.frame, ub, ub.h / 2.0f, color_alpha(D.accent, h ? 0.35f : 0.2f));
            draw_text_centered(A.frame, small, ub, "Undo", D.surface_text);
            if (h && I.clicked)
                op_undo_trash();
            x -= w + 10;
        }
        char st[200];
        fit(small, F.status, sb.w / 2, st, sizeof st);
        draw_text(A.frame, small, x - text_width(small, st), ty, st, D.surface_text);
    }
}

static void draw_menu(rect body) {
    int target = F.menu_target >= 0 && F.menu_target < F.n ? F.menu_target : F.menu_target == -2 ? F.cursor : -1;
    rect m = F.menu_r;
    if (m.x + m.w > body.x + body.w - 4)
        m.x = body.x + body.w - 4 - m.w;
    if (m.y + m.h > body.y + body.h - 4)
        m.y = m.y - m.h > body.y ? m.y - m.h : body.y + body.h - 4 - m.h;
    if (m.x < body.x + 4)
        m.x = body.x + 4;
    F.menu_r = m;
    material_panel(m, D.radius * 0.5f + 4);
    font *f = FB(D.body_px - 1);
    int lh = font_line_height(f) + 12, y = m.y + 5;
    for (int i = 0; i < F.nm; i++) {
        struct mitem *it = &F.mi[i];
        if (it->sep && i) {
            fill_rect(A.frame, (rect){m.x + 10, y + 4, m.w - 20, 1}, color_alpha(D.surface_text, 0.1f));
            y += 9;
        }
        rect r = {m.x + 5, y, m.w - 10, lh};
        bool h = it->enabled && hov(r);
        if (h)
            fill_round(A.frame, r, D.radius * 0.35f + 2, D.accent);
        color ink = !it->enabled ? color_alpha(D.surface_text, 0.35f) : h ? readable_on(D.accent) : D.surface_text;
        if (it->checked)
            draw_text(A.frame, f, r.x + 10, r.y + 6, "\xE2\x9C\x93", ink);
        draw_text(A.frame, f, r.x + 30, r.y + 6, it->label, ink);
        lrect("files-menu-%s", r, it->label);
        if (h && I.clicked) {
            F.menu_on = false;
            menu_run(it->id, target);
            break;
        }
        y += lh;
    }
}

static void drag_ghost(void) {
    if (!F.dragging || !F.ndrag)
        return;
    font *f = FB(D.body_px - 2);
    char lab[160];
    if (F.ndrag == 1)
        fit(f, base_name(F.drag[0]), 220, lab, sizeof lab);
    else
        snprintf(lab, sizeof lab, "%d items", F.ndrag);
    int w = text_width(f, lab) + 58, h = 40;
    rect r = {A.in.mx + 14, A.in.my + 10, w, h};
    canvas_unclip(A.frame); /* the ghost may leave the window */
    material_panel(r, D.radius * 0.5f + 4);
    struct stat st;
    bool dir = stat(F.drag[0], &st) == 0 && S_ISDIR(st.st_mode);
    file_icon(A.frame, (rect){r.x + 8, r.y + 6, 28, 28}, file_kind(F.drag[0], dir), base_name(F.drag[0]), false);
    draw_text(A.frame, f, r.x + 44, r.y + (h - font_line_height(f)) / 2, lab, D.surface_text);
    if (F.ndrag > 1) {
        char n[8];
        snprintf(n, sizeof n, "%d", F.ndrag);
        rect badge = {r.x + 24, r.y - 6, 20, 20};
        fill_circle(A.frame, badge.x + 10.0f, badge.y + 10.0f, 10, D.accent);
        draw_text_centered(A.frame, font_get("Inter", 12), badge, n, readable_on(D.accent));
    }
}

/* ---- the viewer ---- */

static void viewer(rect body, bool mine, bool kb) {
    fill_rect(A.frame, body, color_mix(RGBA(12, 12, 14, 255), D.bg, 0.25f));
    font *f = FB(D.body_px - 1), *small = FB(D.body_px - 3);
    color ink = RGBA(240, 238, 236, 255);
    rect top = {body.x, body.y, body.w, 44};
    rect back = {body.x + 10, body.y + 7, 32, 30};
    lrect("files-viewer-back", back);
    bool hb = mine && hov(back);
    if (hb)
        fill_round(A.frame, back, 8, RGBA(255, 255, 255, 30));
    ui_glyph(A.frame, (rect){back.x + 7, back.y + 6, 18, 18}, G_BACK, 1.7f, ink);
    char title[300];
    fit(f, base_name(F.view_path), body.w - 260, title, sizeof title);
    draw_text_centered(A.frame, f, top, title, ink);
    if (F.view_full) {
        char dims[80];
        snprintf(dims, sizeof dims, "%d \xC3\x97 %d", F.view_full->src_w, F.view_full->src_h);
        draw_text(A.frame, small, body.x + body.w - 14 - text_width(small, dims), body.y + 14, dims,
                  RGBA(200, 196, 192, 255));
    }
    rect area = {body.x + 56, body.y + 52, body.w - 112, body.h - 70};
    if (F.view_failed || !F.view_full) {
        draw_text_centered(A.frame, f, area, "This picture can't be shown.", RGBA(200, 196, 192, 255));
    } else {
        float s = fminf((float)area.w / F.view_full->w, (float)area.h / F.view_full->h);
        if (s > 1)
            s = 1; /* never blown up past its own size */
        int w = (int)(F.view_full->w * s), h = (int)(F.view_full->h * s);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (!F.view_fit || F.view_fit->w != w || F.view_fit->h != h) {
            image_free(F.view_fit);
            F.view_fit = image_resized(F.view_full, w, h);
        }
        int x = area.x + (area.w - w) / 2, y = area.y + (area.h - h) / 2;
        shadow_round(A.frame, (rect){x, y + 8, w, h}, 4, 30, RGBA(0, 0, 0, 140));
        image_draw(A.frame, F.view_fit, x, y, 1);
        lrect("files-viewer-image", ((rect){x, y, w, h}));
    }
    rect prev = {body.x + 8, body.y + body.h / 2 - 30, 40, 60}, next = {body.x + body.w - 48, body.y + body.h / 2 - 30, 40, 60};
    lrect("files-viewer-prev", prev);
    lrect("files-viewer-next", next);
    bool hp = mine && hov(prev), hn = mine && hov(next);
    fill_round(A.frame, prev, 10, RGBA(255, 255, 255, hp ? 40 : 14));
    fill_round(A.frame, next, 10, RGBA(255, 255, 255, hn ? 40 : 14));
    ui_glyph(A.frame, (rect){prev.x + 8, prev.y + 18, 24, 24}, G_BACK, 2, ink);
    ui_glyph(A.frame, (rect){next.x + 8, next.y + 18, 24, 24}, G_FORWARD, 2, ink);
    draw_text_centered(A.frame, small, (rect){body.x, body.y + body.h - 22, body.w, 20},
                       "\xE2\x86\x90 \xE2\x86\x92 to browse  \xC2\xB7  Esc to go back", RGBA(170, 166, 162, 255));
    if (mine && I.clicked) {
        if (hb)
            close_viewer();
        else if (hp)
            viewer_step(-1);
        else if (hn)
            viewer_step(1);
    }
    if (kb)
        for (int i = 0; i < I.nkeys && F.viewing; i++) {
            int k = I.keys[i];
            if (k == K_ESC || k == ' ' || k == K_BACKSPACE)
                close_viewer();
            else if (k == K_LEFT || k == K_UP)
                viewer_step(-1);
            else if (k == K_RIGHT || k == K_DOWN)
                viewer_step(1);
        }
}

/* ---- the whole app ---- */

void files_body(rect body) {
    files_setup();
    if (!F.init) {
        F.init = true;
        snprintf(F.path, sizeof F.path, HOME_DIR); /* their home, not the machine's insides */
        F.reload = true;
    }
    if (!F.view_chosen) /* the design's preference, until they choose */
        F.view = D.explorer == EXPLORER_ICONS ? V_GRID : V_LIST;
    bool in_window = A.win[WIN_FILES].open && !A.win[WIN_FILES].minimized && pt_in(A.win[WIN_FILES].r, body.x, body.y);
    bool mine = pointer_mine(body, in_window);
    bool kb = keyboard_mine(in_window);
    focused = kb;
    thumb_budget = 1;
    I = A.in;
    if (!mine) { /* the pointer is someone else's: look, don't touch */
        I.clicked = I.pressed = I.right_clicked = false;
        I.wheel = 0;
        if (!F.dragging && !F.banding)
            I.mx = I.my = -10000;
    }
    if (!kb)
        I.nkeys = 0;

    if (F.reload || strcmp(F.loaded, F.path) || A.now - F.loaded_at > 1500)
        load();
    if (F.viewing) {
        viewer(body, mine, kb);
        if (in_window)
            flush_rects();
        return;
    }

    /* the menu has the pointer while it is open */
    if (F.menu_on && (I.pressed || I.clicked || I.right_clicked)) {
        if (!pt_in(F.menu_r, I.mx, I.my)) {
            F.menu_on = false;
            I.pressed = I.clicked = I.right_clicked = false;
        } else {
            I.pressed = I.right_clicked = false; /* the menu reads clicks itself */
        }
    }
    input_state real = I;
    if (F.menu_on) { /* nothing under the menu reacts */
        I.clicked = false;
        I.mx = I.my = -10000;
    }

    rect tb = {body.x + 4, body.y + 2, body.w - 8, 42};
    int sbw = body.w > 600 ? 184 : 0;
    rect side = {body.x + 2, tb.y + tb.h + 6, sbw, body.h - tb.h - 10};
    rect stat_r = {body.x + sbw + 10, body.y + body.h - 28, body.w - sbw - 12, 28};
    rect content = {body.x + sbw + 12, tb.y + tb.h + 6, body.w - sbw - 16, stat_r.y - tb.y - tb.h - 8};
    int header = F.view == V_LIST ? font_line_height(FB(D.body_px - 3)) + 12 : 0;
    lrect("files-content", content);

    F.drop[0] = 0;
    place_items(content, header);
    int maxs = F.content_h - content.h;
    if (mine && I.wheel)
        F.scroll -= I.wheel * 48;
    if (F.scroll > maxs)
        F.scroll = maxs;
    if (F.scroll < 0)
        F.scroll = 0;
    place_items(content, header);

    keyboard(content, kb);
    if (F.reload || strcmp(F.loaded, F.path)) {
        load();
        place_items(content, header);
    }

    /* the pointer in the content: select, open, drag, rubber-band, menu */
    int over = item_at(I.mx, I.my, content, header);
    bool in_content = pt_in(content, I.mx, I.my);
    if (I.pressed && F.renaming >= 0) {
        rect rr = F.e[F.renaming].r;
        if (!pt_in(rr, I.mx, I.my))
            commit_rename(); /* clicking elsewhere keeps the new name */
    }
    if (I.pressed && in_content && pt_in((rect){content.x, content.y + header, content.w, content.h - header}, I.mx, I.my)) {
        F.press_x = I.mx, F.press_y = I.my;
        F.press_idx = over;
        F.press_defer = false;
        if (over >= 0) {
            if (I.ctrl) {
                F.e[over].sel = !F.e[over].sel;
                F.cursor = F.anchor = over;
            } else if (I.shift) {
                select_range(F.anchor, over, false);
                F.cursor = over;
            } else if (F.e[over].sel) {
                F.press_defer = true; /* maybe the start of dragging them all */
                F.cursor = over;
            } else {
                select_only(over);
            }
        } else {
            F.banding = true;
            F.band_add = I.ctrl || I.shift;
            if (!F.band_add)
                select_none();
            for (int i = 0; i < F.n; i++)
                F.e[i].was_sel = F.e[i].sel;
            F.band_x0 = I.mx;
            F.band_y0 = I.my + F.scroll;
        }
    }
    if (A.in.down && F.press_idx >= 0 && !F.dragging &&
        (abs(A.in.mx - F.press_x) > 6 || abs(A.in.my - F.press_y) > 6) && !in_trash()) {
        F.dragging = true;
        F.ndrag = 0;
        for (int i = 0; i < F.n && F.ndrag < 64; i++)
            if (F.e[i].sel)
                ent_path(i, F.drag[F.ndrag++], PATHN);
        LOG("files: dragging %d\n", F.ndrag);
    }
    if (F.banding) {
        /* the band follows the pointer, scrolling the folder at its edges */
        if (A.in.my < content.y + header + 16)
            F.scroll -= 14;
        else if (A.in.my > content.y + content.h - 16)
            F.scroll += 14;
        if (F.scroll > maxs) F.scroll = maxs;
        if (F.scroll < 0) F.scroll = 0;
        place_items(content, header);
        int x0 = F.band_x0, y0 = F.band_y0 - F.scroll, x1 = A.in.mx, y1 = A.in.my;
        rect band = {x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, abs(x1 - x0), abs(y1 - y0)};
        for (int i = 0; i < F.n; i++) {
            rect r = F.e[i].r;
            if (F.view == V_LIST)
                r = (rect){r.x, r.y, r.w / 2, r.h};
            else
                r = rect_inset(r, 10);
            bool in = overlaps(band, r);
            F.e[i].sel = F.band_add ? (F.e[i].was_sel != in) : in;
        }
    }
    if (F.dragging && (A.in.my < content.y + header + 12 || A.in.my > content.y + content.h - 12) &&
        pt_in((rect){content.x, content.y - 40, content.w, content.h + 80}, A.in.mx, A.in.my)) {
        F.scroll += A.in.my < content.y + header + 12 ? -10 : 10;
        if (F.scroll > maxs) F.scroll = maxs;
        if (F.scroll < 0) F.scroll = 0;
        place_items(content, header);
    }

    /* ---- draw ---- */
    toolbar(tb);
    if (sbw)
        sidebar(side);
    if (!F.n)
        empty_state(content);
    else if (F.view == V_LIST)
        draw_list(content, header);
    else
        draw_grid(content);
    if (F.banding) {
        int x0 = F.band_x0, y0 = F.band_y0 - F.scroll, x1 = A.in.mx, y1 = A.in.my;
        rect band = {x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, abs(x1 - x0), abs(y1 - y0)};
        canvas_clip(A.frame, content);
        fill_rect(A.frame, band, color_alpha(D.accent, 0.16f));
        stroke_round(A.frame, band, 2, 1, color_alpha(D.accent, 0.8f));
        canvas_unclip(A.frame);
    }
    status_bar(stat_r);
    /* restore the body clip for what follows */
    canvas_clip(A.frame, body);

    /* releases: clicks, double clicks, drops */
    if (!A.in.down && (F.dragging || F.banding || F.press_idx >= 0 || F.press_defer)) {
        if (F.dragging) {
            if (F.drop[0] && mine) {
                bool to_trash = !strcmp(F.drop, TRASH_FILES);
                int n = 0;
                if (to_trash) {
                    op_trash();
                } else {
                    n = bring(F.drag, F.ndrag, F.drop, true, NULL, 0);
                    char pretty[PATHN];
                    pretty_dir(F.drop, pretty, sizeof pretty);
                    say("Moved %d item%s to %s.", n, n == 1 ? "" : "s", pretty);
                }
            }
            F.dragging = false;
            F.ndrag = 0;
        } else if (F.banding) {
            F.banding = false;
            int n = sel_count();
            if (n)
                LOG("files: selected %d\n", n);
        } else if (I.clicked && F.press_idx >= 0 && F.press_idx == over) {
            if (F.press_defer && !I.ctrl && !I.shift)
                select_only(over);
            if (F.last_click_idx == over && A.now - F.last_click_at < 450) {
                F.last_click_at = 0;
                open_ent(over);
            } else {
                F.last_click_at = A.now;
                F.last_click_idx = over;
            }
        }
        F.press_idx = -1;
        F.press_defer = false;
    }
    if (I.right_clicked && in_content && !F.dragging)
        menu_open(I.mx, I.my, item_at(I.mx, I.my, content, header));
    if (F.menu_on) {
        I = real;
        draw_menu(body);
    }
    drag_ghost();
    canvas_clip(A.frame, body);
    if (in_window)
        flush_rects();
}
