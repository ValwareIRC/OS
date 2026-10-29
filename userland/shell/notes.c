#define _GNU_SOURCE
#include "notes.h"
#include "app.h"
#include "theme.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Notes, a small plain-text editor: soft word wrap, a caret and a selection
 * (mouse, Shift+arrows, double click for a word, triple for a paragraph),
 * the clipboard, undo and redo, and quiet saving to /root/Documents under
 * the note's title. Everything is UTF-8 and kept as plain text. */

#define DOCS "/root/Documents"
#define LAST STATE_DIR "/notes-last" /* the note that was open, for next time */
#define LEGACY "/root/notes.txt"     /* where notes lived before they had titles */
#define CAP (256 * 1024)             /* the longest file edited whole */
#define UNDO 64
#define TABW 4

enum { ED_TYPE = 1, ED_DELETE, ED_OTHER };

struct snap {
    char *text;
    int len, caret, anchor;
};

static struct {
    bool loaded;
    char *buf;
    int len;
    int caret, anchor; /* byte offsets; the selection is between them */
    char title[96];
    bool title_set;    /* they (or the first save) named it */
    char path[512];    /* "" until it is first saved */
    bool readonly, dirty, failed;
    uint64_t changed_at;
    int focus; /* 0 the text, 1 the title */
    int scroll, want_x;
    bool follow; /* keep the caret in view after this frame's keys */
    bool dragging;
    uint64_t last_press;
    int presses, press_x, press_y;
    struct snap undo[UNDO], redo[UNDO];
    int nundo, nredo, last_kind;
    uint64_t last_edit;
    /* wrapped lines: start offsets, worked out again when anything changes */
    int *lines, nlines, lcap, lay_w, lay_ver, ver;
    font *lay_f;
    uint64_t saved_flash;
} N;

/* ---- measuring (tabs are TABW spaces wide) ---- */

static int seg_width(font *f, const char *s, int n) {
    int w = 0, i = 0;
    while (i < n) {
        int j = i;
        while (j < n && s[j] != '\t')
            j++;
        if (j > i)
            w += text_width_n(f, s + i, j - i);
        if (j < n)
            w += text_width(f, " ") * TABW, j++;
        i = j;
    }
    return w;
}

static void seg_draw(font *f, int x, int y, const char *s, int n, color col) {
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && s[j] != '\t')
            j++;
        if (j > i)
            x += draw_text_n(A.frame, f, x, y, s + i, j - i, col);
        if (j < n)
            x += text_width(f, " ") * TABW, j++;
        i = j;
    }
}

static int cp_next(int i) {
    if (i >= N.len)
        return N.len;
    i++;
    while (i < N.len && (N.buf[i] & 0xc0) == 0x80)
        i++;
    return i;
}

static int cp_prev(int i) {
    if (i <= 0)
        return 0;
    i--;
    while (i > 0 && (N.buf[i] & 0xc0) == 0x80)
        i--;
    return i;
}

static bool wordch(char c) { return (c & 0x80) || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/* ---- wrapping ---- */

static void push_line(int start) {
    if (N.nlines == N.lcap) {
        N.lcap = N.lcap ? N.lcap * 2 : 256;
        N.lines = realloc(N.lines, sizeof *N.lines * (size_t)N.lcap);
    }
    N.lines[N.nlines++] = start;
}

static void layout(font *f, int width) {
    if (N.lay_f == f && N.lay_w == width && N.lay_ver == N.ver && N.nlines)
        return;
    N.lay_f = f, N.lay_w = width, N.lay_ver = N.ver;
    N.nlines = 0;
    int p = 0;
    for (;;) {
        push_line(p);
        int end = p;
        while (end < N.len && N.buf[end] != '\n')
            end++;
        /* break the paragraph [p, end) into lines no wider than width */
        while (p < end) {
            int x = 0, i = p, last_space = -1;
            while (i < end) {
                int n = cp_next(i) - i;
                int cw = seg_width(f, N.buf + i, n);
                if (x + cw > width && i > p)
                    break;
                x += cw;
                if (N.buf[i] == ' ' || N.buf[i] == '\t')
                    last_space = i;
                i += n;
            }
            if (i >= end)
                break;
            int brk = last_space >= p ? last_space + 1 : i; /* after the space, or mid-word if it must */
            push_line(brk);
            p = brk;
        }
        if (end >= N.len)
            break;
        p = end + 1;
    }
}

/* The line holding offset o (the later one where a wrap makes it ambiguous). */
static int line_of(int o) {
    int lo = 0, hi = N.nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (N.lines[mid] <= o)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* Where line i ends, not counting its newline. */
static int line_end(int i) {
    int e = i + 1 < N.nlines ? N.lines[i + 1] : N.len;
    if (e > N.lines[i] && e <= N.len && N.buf[e - 1] == '\n')
        e--;
    return e;
}

static int offset_at_x(font *f, int line, int x) {
    int s = N.lines[line], e = line_end(line);
    int i = s;
    while (i < e) {
        int n = cp_next(i) - i;
        int a = seg_width(f, N.buf + s, i - s), b = seg_width(f, N.buf + s, i - s + n);
        if (x < (a + b) / 2)
            break;
        i += n;
    }
    return i;
}

/* ---- undo ---- */

static void snap_free(struct snap *s) {
    free(s->text);
    s->text = NULL;
}

static struct snap snap_take(void) {
    struct snap s = {malloc((size_t)N.len + 1), N.len, N.caret, N.anchor};
    memcpy(s.text, N.buf, (size_t)N.len + 1);
    return s;
}

static void snap_push(struct snap *stack, int *n, struct snap s) {
    if (*n == UNDO) {
        snap_free(&stack[0]);
        memmove(stack, stack + 1, sizeof *stack * (UNDO - 1));
        (*n)--;
    }
    stack[(*n)++] = s;
}

static void snap_restore(struct snap s) {
    memcpy(N.buf, s.text, (size_t)s.len + 1);
    N.len = s.len;
    N.caret = s.caret;
    N.anchor = s.anchor;
    free(s.text);
    N.ver++;
    N.dirty = true;
    N.changed_at = A.now;
    N.follow = true;
}

static void undo_step(bool redo) {
    struct snap *from = redo ? N.redo : N.undo, *to = redo ? N.undo : N.redo;
    int *nf = redo ? &N.nredo : &N.nundo, *nt = redo ? &N.nundo : &N.nredo;
    if (!*nf || N.readonly)
        return;
    snap_push(to, nt, snap_take());
    snap_restore(from[--*nf]);
    N.last_kind = 0;
}

/* ---- editing ---- */

static int sel_lo(void) { return N.caret < N.anchor ? N.caret : N.anchor; }
static int sel_hi(void) { return N.caret > N.anchor ? N.caret : N.anchor; }

/* Replaces [a, b) with s. Typing coalesces into one undo step per word. */
static void replace(int a, int b, const char *s, int n, int kind) {
    if (N.readonly)
        return;
    if (N.len - (b - a) + n > CAP) {
        toast("This note is as long as Notes can hold.");
        return;
    }
    bool boundary = kind == ED_TYPE && n == 1 && (s[0] == ' ' || s[0] == '\n') && a > 0 && N.buf[a - 1] != ' ' &&
                    N.buf[a - 1] != '\n';
    if (kind != N.last_kind || kind == ED_OTHER || A.now - N.last_edit > 1200 || boundary || a != b) {
        snap_push(N.undo, &N.nundo, snap_take());
        for (int i = 0; i < N.nredo; i++)
            snap_free(&N.redo[i]);
        N.nredo = 0;
    }
    N.last_kind = kind;
    N.last_edit = A.now;
    memmove(N.buf + a + n, N.buf + b, (size_t)(N.len - b) + 1);
    memcpy(N.buf + a, s, (size_t)n);
    N.len += n - (b - a);
    N.caret = N.anchor = a + n;
    N.ver++;
    N.dirty = true;
    N.changed_at = A.now;
    N.follow = true;
}

static void insert(const char *s, int n, int kind) { replace(sel_lo(), sel_hi(), s, n, kind); }

static void copy_selection(void) {
    int a = sel_lo(), b = sel_hi();
    if (a == b)
        return;
    char *t = strndup(N.buf + a, (size_t)(b - a));
    clipboard_set(t);
    free(t);
}

/* ---- files ---- */

static void remember(void) {
    FILE *f = fopen(LAST, "w");
    if (f) {
        fputs(N.path, f);
        fclose(f);
    }
}

static void clear_history(void) {
    for (int i = 0; i < N.nundo; i++)
        snap_free(&N.undo[i]);
    for (int i = 0; i < N.nredo; i++)
        snap_free(&N.redo[i]);
    N.nundo = N.nredo = 0;
    N.last_kind = 0;
}

static void title_from_path(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(N.title, sizeof N.title, "%s", base);
    char *dot = strrchr(N.title, '.');
    if (dot && !strcasecmp(dot, ".txt"))
        *dot = 0;
    N.title_set = true;
}

/* Reads a file into the editor; false if it is not text. */
static bool load(const char *path, bool quiet) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char *b = malloc(CAP + 1);
    size_t n = fread(b, 1, CAP, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    if (memchr(b, 0, n)) {
        free(b);
        if (!quiet) {
            const char *name = strrchr(path, '/');
            toast("There's no app here for %s.", name ? name + 1 : path);
        }
        return false;
    }
    /* line ends as typed here: \r\n becomes \n */
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
        if (!(b[i] == '\r' && i + 1 < n && b[i + 1] == '\n'))
            b[o++] = b[i];
    memcpy(N.buf, b, o);
    N.buf[o] = 0;
    free(b);
    N.len = (int)o;
    N.caret = N.anchor = 0;
    N.scroll = 0;
    N.readonly = more;
    N.dirty = N.failed = false;
    N.ver++;
    clear_history();
    snprintf(N.path, sizeof N.path, "%s", path);
    title_from_path(path);
    if (more && !quiet)
        toast("Showing the beginning of a long file; it won't be changed.");
    return true;
}

static void sanitize(char *out, size_t n, const char *in) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n')
            break;
        out[o++] = c < 32 || c == '/' || c == '\\' || c == ':' ? ' ' : (char)c;
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
    size_t lead = strspn(out, " .");
    memmove(out, out + lead, strlen(out + lead) + 1);
}

/* Where a note with this title lives: beside the file it came from, or in
 * Documents. Never over another file. */
static void path_for(char *out, size_t n, const char *title) {
    char dir[512] = DOCS, ext[16] = ".txt";
    if (N.path[0]) {
        snprintf(dir, sizeof dir, "%s", N.path);
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = 0;
        const char *base = strrchr(N.path, '/'), *dot = strrchr(base ? base : N.path, '.');
        snprintf(ext, sizeof ext, "%s", dot && strlen(dot) < sizeof ext ? dot : "");
    }
    for (int k = 1; k < 100; k++) {
        if (k == 1)
            snprintf(out, n, "%s/%s%s", dir, title, ext);
        else
            snprintf(out, n, "%s/%s %d%s", dir, title, k, ext);
        if (!strcmp(out, N.path) || access(out, F_OK) != 0)
            return;
    }
}

static void save(void) {
    if (N.readonly || (!N.dirty && N.path[0]))
        return;
    if (!N.path[0] && !N.len && !N.title[0])
        return; /* nothing to keep yet */
    if (!N.title[0]) {
        /* named after its first words, like a letter is by its opening */
        char first[60];
        sanitize(first, sizeof first, N.buf + strspn(N.buf, " \n\t"));
        if (strlen(first) > 40) {
            char *sp = strrchr(first + 20, ' ');
            *(sp && sp - first < 41 ? sp : first + 40) = 0;
        }
        snprintf(N.title, sizeof N.title, "%s", first[0] ? first : "Untitled");
    }
    char clean[96], want[600];
    sanitize(clean, sizeof clean, N.title);
    if (!clean[0])
        snprintf(clean, sizeof clean, "Untitled");
    path_for(want, sizeof want, clean);
    mkdir(DOCS, 0755);
    /* renamed: the file moves with its title */
    if (N.path[0] && strcmp(want, N.path) && access(N.path, F_OK) == 0 && rename(N.path, want) != 0)
        snprintf(want, sizeof want, "%s", N.path);
    char tmp[620];
    snprintf(tmp, sizeof tmp, "%s.saving", want);
    FILE *f = fopen(tmp, "w");
    bool ok = f && fwrite(N.buf, 1, (size_t)N.len, f) == (size_t)N.len;
    if (f && fclose(f) != 0)
        ok = false;
    if (ok && rename(tmp, want) != 0)
        ok = false;
    if (!ok) {
        unlink(tmp);
        if (!N.failed)
            LOG("notes: can't save %s: %s\n", want, strerror(errno));
        N.failed = true;
        N.changed_at = A.now; /* try again in a moment */
        return;
    }
    if (strcmp(want, N.path))
        LOG("notes: saved as %s\n", want);
    snprintf(N.path, sizeof N.path, "%s", want);
    N.dirty = N.failed = false;
    N.title_set = true;
    remember();
}

static void ensure_loaded(void) {
    if (N.loaded)
        return;
    N.loaded = true;
    N.buf = calloc(CAP + 1, 1);
    char last[512] = "";
    FILE *f = fopen(LAST, "r");
    if (f) {
        if (!fgets(last, sizeof last, f))
            last[0] = 0;
        fclose(f);
        last[strcspn(last, "\r\n")] = 0;
    }
    if (last[0] && load(last, true))
        return;
    /* notes from before titles: carried over as "Notes" */
    if (load(LEGACY, true)) {
        N.path[0] = 0;
        snprintf(N.title, sizeof N.title, "Notes");
        N.title_set = false;
        N.dirty = N.len > 0;
    }
}

bool notes_open(const char *path) {
    ensure_loaded();
    if (N.dirty)
        save();
    if (!load(path, false))
        return false;
    remember();
    N.focus = 0;
    return true;
}

static void new_note(void) {
    if (N.dirty)
        save();
    N.len = 0;
    N.buf[0] = 0;
    N.caret = N.anchor = N.scroll = 0;
    N.path[0] = N.title[0] = 0;
    N.title_set = N.readonly = N.dirty = N.failed = false;
    N.ver++;
    N.focus = 0;
    clear_history();
}

void notes_forget(void) {
    ensure_loaded();
    unlink(LEGACY);
    unlink(LAST);
    new_note();
    N.dirty = false;
}

/* ---- keys ---- */

static void move_to(int o, bool extend) {
    N.caret = o < 0 ? 0 : o > N.len ? N.len : o;
    if (!extend)
        N.anchor = N.caret;
    N.follow = true;
    N.last_kind = 0;
}

static void title_keys(void) {
    for (int i = 0; i < A.in.nkeys; i++) {
        int k = A.in.keys[i];
        size_t tl = strlen(N.title);
        if (k == K_ENTER || k == K_TAB || k == K_DOWN) {
            N.focus = 0;
        } else if (k == K_BACKSPACE && tl) {
            do
                tl--;
            while (tl && (N.title[tl] & 0xc0) == 0x80);
            N.title[tl] = 0;
            N.dirty = true, N.changed_at = A.now;
        } else if (k == K_PASTE || (k >= 32 && k < 127 && !A.in.ctrl)) {
            char add[96];
            if (k == K_PASTE)
                sanitize(add, sizeof add, clipboard_text());
            else
                add[0] = (char)k, add[1] = 0;
            snprintf(N.title + tl, sizeof N.title - tl, "%s", add);
            N.dirty = true, N.changed_at = A.now;
        }
        if (N.dirty)
            N.title_set = true;
    }
}

static void text_keys(font *f, int view_lines) {
    for (int i = 0; i < A.in.nkeys; i++) {
        int k = A.in.keys[i];
        bool sh = A.in.shift, ctrl = A.in.ctrl, sel = N.caret != N.anchor;
        layout(f, N.lay_w);
        int line = line_of(N.caret);
        int cx = seg_width(f, N.buf + N.lines[line], N.caret - N.lines[line]);
        bool vertical = false;
        switch (k) {
        case K_LEFT:
            if (sel && !sh)
                move_to(sel_lo(), false);
            else if (ctrl) {
                int o = cp_prev(N.caret);
                while (o > 0 && !wordch(N.buf[o]))
                    o = cp_prev(o);
                while (o > 0 && wordch(N.buf[o - 1]))
                    o = cp_prev(o);
                move_to(o, sh);
            } else
                move_to(cp_prev(N.caret), sh);
            break;
        case K_RIGHT:
            if (sel && !sh)
                move_to(sel_hi(), false);
            else if (ctrl) {
                int o = N.caret;
                while (o < N.len && !wordch(N.buf[o]))
                    o = cp_next(o);
                while (o < N.len && wordch(N.buf[o]))
                    o = cp_next(o);
                move_to(o, sh);
            } else
                move_to(cp_next(N.caret), sh);
            break;
        case K_UP:
        case K_DOWN:
        case K_PGUP:
        case K_PGDN: {
            int step = k == K_UP ? -1 : k == K_DOWN ? 1 : k == K_PGUP ? -view_lines : view_lines;
            if (N.last_kind != -1)
                N.want_x = cx;
            int to = line + step;
            if (to < 0)
                move_to(0, sh);
            else if (to >= N.nlines)
                move_to(N.len, sh);
            else
                move_to(offset_at_x(f, to, N.want_x), sh);
            N.last_kind = -1; /* keep the column across a run of these */
            vertical = true;
            break;
        }
        case K_HOME: move_to(ctrl ? 0 : N.lines[line], sh); break;
        case K_END: move_to(ctrl ? N.len : line_end(line), sh); break;
        case K_BACKSPACE:
            if (sel)
                replace(sel_lo(), sel_hi(), "", 0, ED_OTHER);
            else if (N.caret > 0) {
                int a = cp_prev(N.caret);
                if (ctrl) { /* the word before the caret, and the gap after it */
                    a = N.caret;
                    while (a > 0 && !wordch(N.buf[a - 1]))
                        a = cp_prev(a);
                    while (a > 0 && wordch(N.buf[a - 1]))
                        a = cp_prev(a);
                }
                replace(a, N.caret, "", 0, ED_DELETE);
            }
            break;
        case K_DELETE:
            if (sel)
                replace(sel_lo(), sel_hi(), "", 0, ED_OTHER);
            else if (N.caret < N.len)
                replace(N.caret, cp_next(N.caret), "", 0, ED_DELETE);
            break;
        case K_ENTER: {
            /* a new line keeps the indent of this one */
            int p = N.caret;
            while (p > 0 && N.buf[p - 1] != '\n')
                p--;
            char nl[64] = "\n";
            int n = 1;
            while (n < 60 && p < N.len && (N.buf[p] == ' ' || N.buf[p] == '\t'))
                nl[n++] = N.buf[p++];
            insert(nl, n, ED_TYPE);
            break;
        }
        case K_TAB: insert("    ", 4, ED_TYPE); break;
        case K_ESC: move_to(N.caret, false); break;
        case K_PASTE: {
            const char *clip = clipboard_text();
            char *t = malloc(strlen(clip) + 1);
            int n = 0;
            for (const char *p = clip; *p; p++)
                if (*p != '\r')
                    t[n++] = *p;
            if (n)
                insert(t, n, ED_OTHER);
            free(t);
            break;
        }
        default:
            if (ctrl && k < 0x100) {
                switch (k) {
                case 'a': case 'A': N.anchor = 0; N.caret = N.len; break;
                case 'c': case 'C': copy_selection(); break;
                case 'x': case 'X':
                    if (sel) {
                        copy_selection();
                        replace(sel_lo(), sel_hi(), "", 0, ED_OTHER);
                    }
                    break;
                case 'z': undo_step(false); break;
                case 'Z': case 'y': case 'Y': undo_step(true); break;
                case 's': case 'S': N.dirty = true; save(); N.saved_flash = A.now; break;
                case 'n': case 'N': new_note(); break;
                }
            } else if (k >= 32 && k < 127) {
                char c = (char)k;
                insert(&c, 1, ED_TYPE);
            } else if (k > K_TALK && k < 0x110000) {
                char u[4];
                int n = 0;
                if (k < 0x800) u[n++] = (char)(0xc0 | (k >> 6)), u[n++] = (char)(0x80 | (k & 0x3f));
                else if (k < 0x10000) u[n++] = (char)(0xe0 | (k >> 12)), u[n++] = (char)(0x80 | ((k >> 6) & 0x3f)), u[n++] = (char)(0x80 | (k & 0x3f));
                else u[n++] = (char)(0xf0 | (k >> 18)), u[n++] = (char)(0x80 | ((k >> 12) & 0x3f)), u[n++] = (char)(0x80 | ((k >> 6) & 0x3f)), u[n++] = (char)(0x80 | (k & 0x3f));
                insert(u, n, ED_TYPE);
            }
        }
        if (!vertical && N.last_kind == -1)
            N.last_kind = 0;
    }
}

/* ---- drawing ---- */

static void word_count(char *out, size_t n) {
    int words = 0;
    bool in = false;
    for (int i = 0; i < N.len; i++) {
        bool w = !(N.buf[i] == ' ' || N.buf[i] == '\n' || N.buf[i] == '\t');
        if (w && !in)
            words++;
        in = w;
    }
    snprintf(out, n, "%d word%s", words, words == 1 ? "" : "s");
}

void notes_draw(rect r) {
    ensure_loaded();
    design *D = &A.d;
    /* Writing faces: the design's own, unless it is a wide display face. */
    const char *face = D->font_body;
    if (!strcasecmp(face, "Unbounded") || !strcasecmp(face, "Syne"))
        face = "Inter";
    float px = D->body_px < 15 ? 15 : D->body_px;
    font *f = font_get(face, px), *small = font_get(face, px - 3 < 12 ? 12 : px - 3);
    font *tf = font_get(D->font_heading, px * 1.55f);
    color surf = theme_under((rect){r.x, r.y, r.w, 40});
    color ink = theme_fit(D->surface_text, surf, 4.5f), muted = theme_fit(D->muted, surf, 3.0f);
    color accent = theme_fit(D->accent, surf, 3.0f);
    int lh = (int)(font_line_height(f) * 1.45f);

    /* the title, the line under it, and a quiet footer */
    int th = font_line_height(tf);
    rect title = {r.x + 6, r.y + 4, r.w - 150, th + 8};
    rect newb = {r.x + r.w - 76, r.y + 6 + (th - 26) / 2, 70, 28};
    int top = title.y + title.h + 14;
    rect foot = {r.x + 6, r.y + r.h - font_line_height(small) - 2, r.w - 12, font_line_height(small)};
    rect text = {r.x + 6, top + 10, r.w - 18, foot.y - 12 - (top + 10)};
    if (text.h < lh)
        text.h = lh;

    /* pointer */
    bool over_text = pt_in(text, A.in.mx, A.in.my);
    if (A.in.pressed && pt_in(title, A.in.mx, A.in.my))
        N.focus = 1;
    if (ui_button(newb, "New", small, color_alpha(D->surface_text, 0.08f), ink, newb.h / 2.0f, true))
        new_note();
    layout(f, text.w);
    if (A.in.pressed && over_text) {
        N.focus = 0;
        int line = (A.in.my - text.y + N.scroll) / lh;
        line = line < 0 ? 0 : line >= N.nlines ? N.nlines - 1 : line;
        int o = offset_at_x(f, line, A.in.mx - text.x);
        bool again = A.now - N.last_press < 450 && abs(A.in.mx - N.press_x) < 6 && abs(A.in.my - N.press_y) < 6;
        N.presses = again ? N.presses + 1 : 1;
        N.last_press = A.now, N.press_x = A.in.mx, N.press_y = A.in.my;
        if (N.presses == 2) { /* a word */
            int a = o, b = o;
            while (a > 0 && wordch(N.buf[a - 1]))
                a--;
            while (b < N.len && wordch(N.buf[b]))
                b++;
            N.anchor = a, N.caret = b;
        } else if (N.presses >= 3) { /* a paragraph */
            int a = o, b = o;
            while (a > 0 && N.buf[a - 1] != '\n')
                a--;
            while (b < N.len && N.buf[b] != '\n')
                b++;
            N.anchor = a, N.caret = b < N.len ? b + 1 : b;
        } else {
            move_to(o, A.in.shift);
            N.dragging = true;
        }
        N.last_kind = 0;
    }
    if (N.dragging) {
        if (!A.in.down) {
            N.dragging = false;
        } else {
            /* past the edges, the text scrolls under the pointer */
            if (A.in.my < text.y)
                N.scroll -= lh / 2;
            else if (A.in.my > text.y + text.h)
                N.scroll += lh / 2;
            int line = (A.in.my - text.y + N.scroll) / lh;
            line = line < 0 ? 0 : line >= N.nlines ? N.nlines - 1 : line;
            N.caret = offset_at_x(f, line, A.in.mx - text.x);
        }
    }
    if (pt_in(r, A.in.mx, A.in.my) && A.in.wheel)
        N.scroll -= A.in.wheel * lh * 3;

    /* keys */
    if (N.focus == 1)
        title_keys();
    else
        text_keys(f, text.h / lh > 1 ? text.h / lh - 1 : 1);
    layout(f, text.w);
    int content = N.nlines * lh;
    if (N.follow) {
        N.follow = false;
        int cy = line_of(N.caret) * lh;
        if (cy < N.scroll)
            N.scroll = cy;
        if (cy + lh > N.scroll + text.h)
            N.scroll = cy + lh - text.h;
    }
    if (N.scroll > content - text.h)
        N.scroll = content - text.h;
    if (N.scroll < 0)
        N.scroll = 0;

    /* title */
    if (N.title[0])
        draw_text(A.frame, tf, title.x, title.y + 4, N.title, ink);
    else
        draw_text(A.frame, tf, title.x, title.y + 4, "Untitled", color_alpha(muted, 0.7f));
    if (N.focus == 1 && (A.now / 530) % 2 == 0)
        fill_rect(A.frame, (rect){title.x + text_width(tf, N.title) + 2, title.y + 8, 2, th - 8}, accent);
    fill_rect(A.frame, (rect){r.x + 6, top, r.w - 12, 1}, color_alpha(ink, 0.12f));

    /* the text, its selection and the caret: only the lines in view */
    canvas_clip(A.frame, (rect){text.x - 4, text.y, text.w + 8, text.h});
    int lo = sel_lo(), hi = sel_hi();
    color selc = color_alpha(D->accent, color_luma(surf) < 0.4f ? 0.45f : 0.28f);
    int first = N.scroll / lh;
    int ascent_pad = (lh - font_line_height(f)) / 2;
    for (int i = first; i < N.nlines; i++) {
        int y = text.y + i * lh - N.scroll;
        if (y > text.y + text.h)
            break;
        int s = N.lines[i], e = line_end(i);
        if (hi > lo && hi > s && lo <= e) {
            int a = lo > s ? lo : s, b = hi < e ? hi : e;
            int x0 = text.x + seg_width(f, N.buf + s, a - s), x1 = text.x + seg_width(f, N.buf + s, b - s);
            if (hi > e && e < N.len) /* the selection runs on past this line */
                x1 += text_width(f, " ");
            if (x1 > x0)
                fill_rect(A.frame, (rect){x0, y, x1 - x0, lh}, selc);
        }
        seg_draw(f, text.x, y + ascent_pad, N.buf + s, e - s, ink);
    }
    if (!N.len && N.focus == 0)
        draw_text(A.frame, f, text.x, text.y + ascent_pad,
                  N.readonly ? "" : "Write anything. It's kept in Documents, on this computer.", color_alpha(muted, 0.8f));
    if (N.focus == 0 && !N.readonly) {
        int line = line_of(N.caret);
        int cx = text.x + seg_width(f, N.buf + N.lines[line], N.caret - N.lines[line]);
        int cy = text.y + line * lh - N.scroll;
        /* steady while typing, blinking at rest */
        bool on = A.now - N.last_edit < 600 || (A.now / 530) % 2 == 0;
        if (on)
            fill_rect(A.frame, (rect){cx, cy + 3, 2, lh - 6}, accent);
    }
    canvas_unclip(A.frame);
    canvas_clip(A.frame, r);
    /* a scroll hint when there is more than fits */
    if (content > text.h) {
        int bh = text.h * text.h / content, by = text.y + (int)((long)(text.h - bh) * N.scroll / (content - text.h));
        fill_round(A.frame, (rect){r.x + r.w - 6, by, 4, bh < 24 ? 24 : bh}, 2, color_alpha(ink, 0.18f));
    }

    /* save a moment after the last change */
    if ((N.dirty || (N.failed && !N.readonly)) && A.now - N.changed_at > 1200)
        save();

    /* footer: how long, where, and whether it's kept */
    char words[32], status[64], where[600];
    word_count(words, sizeof words);
    if (N.readonly)
        snprintf(status, sizeof status, "Read only");
    else if (N.failed)
        snprintf(status, sizeof status, "Can't save here");
    else if (N.dirty)
        snprintf(status, sizeof status, "Editing");
    else if (N.path[0])
        snprintf(status, sizeof status, A.now - N.saved_flash < 1500 ? "Saved just now" : "Saved");
    else
        status[0] = 0;
    const char *shown = N.path;
    if (!strncmp(shown, "/root/", 6))
        shown += 6;
    snprintf(where, sizeof where, "%s%s%s", words, shown[0] ? "  \xc2\xb7  " : "", shown);
    draw_text(A.frame, small, foot.x, foot.y, where, muted);
    int sw = text_width(small, status);
    draw_text(A.frame, small, foot.x + foot.w - sw, foot.y, status, N.failed ? theme_fit(RGBA(220, 70, 60, 255), surf, 3.0f) : muted);
}
