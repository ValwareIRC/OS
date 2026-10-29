#define _GNU_SOURCE
/* The shell's side of the portals (see userland/portal/os-portal.c).
 *
 * os-portal answers apps on D-Bus and asks the shell, over a local socket
 * ($XDG_RUNTIME_DIR/os-shell-portal.sock, one JSON object per line), for
 * the parts people see: here, the file chooser (a modal over the desktop,
 * in the person's own design) and notifications. The shell tells it the
 * colour scheme and accent, so apps follow the design. */
#include "portal.h"
#include "app.h"
#include "files.h"
#include "launcher.h"
#include "theme.h"
#include "../lib/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define D (A.d)
#define MAX_CLIENTS 8
#define MAX_ENTS 1024
#define PATHN 1024

void notify(const char *title, const char *body, const char *app);

struct client {
    int fd;
    char buf[16384];
    size_t len;
};

static int listen_fd = -1;
static struct client clients[MAX_CLIENTS];
static int nclients;
static char sent_appearance[128];

struct filter {
    char name[64];
    char pat[8][48];
    int npat;
};

struct pent {
    char name[256];
    bool dir;
    long long size;
    time_t mtime;
    bool sel;
};

/* The open chooser (one at a time; later ones wait their turn). */
static struct {
    bool active;
    int fd;
    unsigned id;
    char mode[8]; /* open, save, saves */
    char title[160], accept[48], app[128];
    bool multiple, directory;
    char dir[PATHN];
    struct textfield name;
    struct filter filters[8];
    int nfilters, filter;
    struct pent e[MAX_ENTS];
    int n, cursor, scroll;
    uint64_t opened_at, click_at;
    int click_i;
    char loaded[PATHN];
    char err[160];
} C;

/* Requests waiting while one is open. */
static struct {
    int fd;
    char *line;
} queue[8];
static int nqueue;

/* ---- the socket ---- */

static const char *sock_path(void) {
    static char p[256];
    const char *dir = getenv("XDG_RUNTIME_DIR");
    snprintf(p, sizeof p, "%s/os-shell-portal.sock", dir && *dir ? dir : SESSION_RUNTIME);
    return p;
}

void portal_init(void) {
    const char *path = sock_path();
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) {
        LOG("portal: cannot listen on %s (errno %d)\n", path, errno);
        close(fd);
        return;
    }
    listen_fd = fd;
    LOG("portal: listening on %s\n", path);
}

int portal_fds(int *out, int max) {
    int n = 0;
    if (listen_fd >= 0 && n < max)
        out[n++] = listen_fd;
    for (int i = 0; i < nclients && n < max; i++)
        out[n++] = clients[i].fd;
    return n;
}

static void send_line(int fd, const char *s) {
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t w = write(fd, s + off, len - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    if (write(fd, "\n", 1) < 0) {
    }
}

static void drop_client(int i) {
    if (C.active && C.fd == clients[i].fd)
        C.active = false; /* nobody to answer any more */
    close(clients[i].fd);
    clients[i] = clients[--nclients];
}

/* ---- the chooser ---- */

static void uri_of(const char *path, sbuf *b) {
    sb_add(b, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p))
            sb_addn(b, (const char *)p, 1);
        else
            sb_printf(b, "%%%02X", *p);
    }
}

static void answer(int response, const char *const *paths, int npaths) {
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"id\":%u,\"response\":%d", C.id, response);
    if (npaths) {
        sb_add(&b, ",\"uris\":[");
        for (int i = 0; i < npaths; i++) {
            sbuf u;
            sb_init(&u);
            uri_of(paths[i], &u);
            if (i)
                sb_add(&b, ",");
            sb_json_str(&b, u.p);
            sb_free(&u);
        }
        sb_add(&b, "]");
    }
    sb_add(&b, "}");
    send_line(C.fd, b.p);
    LOG("portal: chooser answered %d (%d file%s)\n", response, npaths, npaths == 1 ? "" : "s");
    sb_free(&b);
    C.active = false;
}

static bool matches_filter(const char *name) {
    if (!C.nfilters)
        return true;
    struct filter *f = &C.filters[C.filter];
    for (int i = 0; i < f->npat; i++) {
        const char *p = f->pat[i];
        if (strchr(p, '/')) { /* a MIME type: by kind, roughly */
            int k = file_kind(name, false);
            if ((!strncmp(p, "text/", 5) && (k == FK_TEXT || k == FK_CODE)) || (!strncmp(p, "image/", 6) && k == FK_IMAGE) ||
                (!strncmp(p, "audio/", 6) && k == FK_AUDIO) || (!strncmp(p, "video/", 6) && k == FK_VIDEO) ||
                !strcmp(p, "application/octet-stream") || !strcmp(p, "*/*"))
                return true;
        } else if (fnmatch(p, name, FNM_CASEFOLD) == 0) {
            return true;
        }
    }
    return false;
}

static int by_name(const void *a, const void *b) {
    const struct pent *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void load(void) {
    C.n = 0;
    C.cursor = -1;
    C.scroll = 0;
    snprintf(C.loaded, sizeof C.loaded, "%s", C.dir);
    DIR *d = opendir(C.dir);
    if (!d) {
        snprintf(C.err, sizeof C.err, "This folder can't be opened.");
        return;
    }
    C.err[0] = 0;
    struct dirent *de;
    while ((de = readdir(d)) && C.n < MAX_ENTS) {
        if (de->d_name[0] == '.')
            continue; /* hidden things stay hidden */
        char full[PATHN];
        snprintf(full, sizeof full, "%s/%s", strcmp(C.dir, "/") ? C.dir : "", de->d_name);
        struct stat st;
        if (stat(full, &st) < 0)
            continue;
        bool dir = S_ISDIR(st.st_mode);
        if (!dir && (C.directory || !matches_filter(de->d_name)))
            continue;
        struct pent *e = &C.e[C.n++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        e->dir = dir;
        e->size = st.st_size;
        e->mtime = st.st_mtime;
    }
    closedir(d);
    qsort(C.e, (size_t)C.n, sizeof C.e[0], by_name);
}

static void go(const char *dir) {
    char real[PATHN];
    if (!realpath(dir, real))
        snprintf(real, sizeof real, "%s", dir);
    snprintf(C.dir, sizeof C.dir, "%s", real);
    load();
}

static void full_path(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", strcmp(C.dir, "/") ? C.dir : "", name);
}

/* Enter or double click: into a folder, or done. */
static void choose(void) {
    bool save = strcmp(C.mode, "open") != 0;
    if (save) {
        if (!C.name.len)
            return;
        if (strcmp(C.mode, "saves") == 0) { /* SaveFiles: the folder they go in */
            const char *p[1] = {C.dir};
            answer(0, p, 1);
            return;
        }
        char full[PATHN];
        full_path(C.name.buf, full, sizeof full);
        const char *p[1] = {full};
        answer(0, p, 1);
        return;
    }
    if (C.directory) {
        char full[PATHN];
        if (C.cursor >= 0 && C.e[C.cursor].dir)
            full_path(C.e[C.cursor].name, full, sizeof full);
        else
            snprintf(full, sizeof full, "%s", C.dir);
        const char *p[1] = {full};
        answer(0, p, 1);
        return;
    }
    static char picked[64][PATHN];
    const char *ptrs[64];
    int n = 0;
    for (int i = 0; i < C.n && n < 64; i++)
        if (C.e[i].sel && !C.e[i].dir) {
            full_path(C.e[i].name, picked[n], PATHN);
            ptrs[n] = picked[n];
            n++;
        }
    if (!n && C.cursor >= 0 && !C.e[C.cursor].dir) {
        full_path(C.e[C.cursor].name, picked[0], PATHN);
        ptrs[n++] = picked[0];
    }
    if (n)
        answer(0, ptrs, n);
}

static void open_entry(int i) {
    if (C.e[i].dir) {
        char full[PATHN];
        full_path(C.e[i].name, full, sizeof full);
        go(full);
        return;
    }
    if (strcmp(C.mode, "open")) { /* saving over a file: its name */
        snprintf(C.name.buf, sizeof C.name.buf, "%s", C.e[i].name);
        C.name.len = (int)strlen(C.name.buf);
        return;
    }
    for (int k = 0; k < C.n; k++)
        C.e[k].sel = k == i;
    C.cursor = i;
    choose();
}

static void start(int fd, json *j) {
    memset(&C, 0, sizeof C);
    C.active = true;
    C.fd = fd;
    C.id = (unsigned)json_num(json_get(j, "id"), 0);
    snprintf(C.mode, sizeof C.mode, "%s", json_str(json_get(j, "mode"), "open"));
    bool save = strcmp(C.mode, "open") != 0;
    snprintf(C.title, sizeof C.title, "%s", json_str(json_get(j, "title"), save ? "Save" : "Open"));
    snprintf(C.accept, sizeof C.accept, "%s", json_str(json_get(j, "accept"), save ? "Save" : "Open"));
    snprintf(C.app, sizeof C.app, "%s", json_str(json_get(j, "app"), ""));
    C.multiple = json_bool(json_get(j, "multiple"), false);
    C.directory = json_bool(json_get(j, "directory"), false);
    json *fl = json_get(j, "filters");
    for (int i = 0; fl && i < json_len(fl) && C.nfilters < 8; i++) {
        json *f = json_at(fl, i);
        struct filter *t = &C.filters[C.nfilters++];
        snprintf(t->name, sizeof t->name, "%s", json_str(json_get(f, "name"), "Files"));
        json *pats = json_get(f, "patterns");
        for (int k = 0; pats && k < json_len(pats) && t->npat < 8; k++)
            snprintf(t->pat[t->npat++], sizeof t->pat[0], "%s", json_str(json_at(pats, k), "*"));
    }
    const char *file = json_str(json_get(j, "file"), NULL), *folder = json_str(json_get(j, "folder"), NULL);
    const char *name = json_str(json_get(j, "name"), NULL);
    char start_dir[PATHN];
    snprintf(start_dir, sizeof start_dir, "%s", HOME_DIR);
    if (folder && *folder)
        snprintf(start_dir, sizeof start_dir, "%s", folder);
    if (file && *file) {
        snprintf(start_dir, sizeof start_dir, "%s", file);
        char *slash = strrchr(start_dir, '/');
        if (slash && slash != start_dir) {
            if (!name)
                name = file + (slash - start_dir) + 1;
            *slash = 0;
        }
    }
    if (name) {
        snprintf(C.name.buf, sizeof C.name.buf, "%s", name);
        C.name.len = (int)strlen(C.name.buf);
    }
    C.opened_at = A.now;
    C.click_i = -1;
    go(start_dir);
    LOG("portal: chooser %s for %s: %s\n", C.mode, C.app[0] ? C.app : "an app", C.title);
}

/* A short, human name for an app id ("org.gnome.TextEditor" -> "TextEditor"). */
static const char *app_label(const char *id) {
    const char *dot = strrchr(id, '.');
    return dot && dot[1] ? dot + 1 : id;
}

static void handle(int fd, char *line) {
    json *j = json_parse(line, strlen(line));
    if (!j)
        return;
    const char *op = json_str(json_get(j, "op"), "");
    if (!strcmp(op, "hello")) {
        sent_appearance[0] = 0; /* everyone hears the look again */
        LOG("portal: os-portal connected\n");
    } else if (!strcmp(op, "file")) {
        if (C.active) {
            if (nqueue < 8) {
                queue[nqueue].fd = fd;
                queue[nqueue++].line = strdup(line);
            }
        } else {
            start(fd, j);
        }
    } else if (!strcmp(op, "nohandler")) {
        /* an app asked to open something nothing installed can open */
        const char *uri = json_str(json_get(j, "uri"), ""), *type = json_str(json_get(j, "type"), "");
        bool web = !strncmp(uri, "http", 4) || strstr(type, "x-scheme-handler/http");
        LOG("portal: nothing opens %s (%s)\n", uri[0] ? uri : type, type);
        if (web) {
            toast("Nothing here opens web links yet. Here are browsers you can get.");
            launcher_open("browser");
        } else {
            toast("Nothing here opens that yet.");
        }
    } else if (!strcmp(op, "notify")) {
        notify(json_str(json_get(j, "title"), ""), json_str(json_get(j, "body"), ""),
               app_label(json_str(json_get(j, "app"), "")));
    }
    json_free(j);
}

void portal_poll(void) {
    if (listen_fd >= 0) {
        int c;
        while ((c = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
            if (nclients == MAX_CLIENTS) {
                close(c);
                continue;
            }
            clients[nclients].fd = c;
            clients[nclients].len = 0;
            nclients++;
        }
    }
    for (int i = 0; i < nclients; i++) {
        struct client *cl = &clients[i];
        for (;;) {
            ssize_t r = read(cl->fd, cl->buf + cl->len, sizeof cl->buf - 1 - cl->len);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                drop_client(i--);
                goto next;
            }
            if (r < 0)
                break;
            cl->len += (size_t)r;
            cl->buf[cl->len] = 0;
            char *nl;
            while ((nl = memchr(cl->buf, '\n', cl->len))) {
                *nl = 0;
                handle(cl->fd, cl->buf);
                size_t used = (size_t)(nl + 1 - cl->buf);
                memmove(cl->buf, nl + 1, cl->len - used);
                cl->len -= used;
            }
            if (cl->len == sizeof cl->buf - 1)
                cl->len = 0; /* a line too long to be ours */
        }
    next:;
    }
    if (!C.active && nqueue) {
        int fd = queue[0].fd;
        char *line = queue[0].line;
        memmove(&queue[0], &queue[1], sizeof queue[0] * (size_t)(nqueue - 1));
        nqueue--;
        json *j = json_parse(line, strlen(line));
        if (j) {
            start(fd, j);
            json_free(j);
        }
        free(line);
    }
    /* the look, to every listener, when it changes */
    char look[128];
    snprintf(look, sizeof look, "{\"op\":\"appearance\",\"scheme\":%d,\"accent\":[%.3f,%.3f,%.3f],\"contrast\":%d}",
             theme_is_dark() ? 1 : 2, C_R(D.accent) / 255.0, C_G(D.accent) / 255.0, C_B(D.accent) / 255.0,
             A.prof.high_contrast ? 1 : 0);
    if (strcmp(look, sent_appearance) && nclients) {
        for (int i = 0; i < nclients; i++)
            send_line(clients[i].fd, look);
        snprintf(sent_appearance, sizeof sent_appearance, "%s", look);
    }
}

/* ---- drawing the chooser ---- */

static input_state saved_in;
static bool took_input;

bool portal_modal(void) { return C.active; }

void portal_take_input(void) {
    took_input = C.active;
    if (!took_input)
        return;
    saved_in = A.in;
    A.in.clicked = A.in.pressed = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.nkeys = 0;
    A.in.nraw = 0;
    A.in.mx = A.in.my = -10000;
}

static font *FB(float px) { return font_get(D.font_body, px); }
static font *FH(float px) { return font_get(D.font_heading, px); }

static bool hit(rect r) { return pt_in(r, A.in.mx, A.in.my); }

static bool button(rect r, const char *label, bool primary, bool enabled) {
    color bg = primary ? D.accent : color_alpha(D.surface_text, 0.08f);
    color fg = primary ? readable_on(D.accent) : D.surface_text;
    return ui_button(r, label, FB(D.body_px), bg, fg, D.radius * 0.5f + 4, enabled);
}

static void fmt_when(time_t t, char *out, size_t n) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%d %b %Y", &tm);
}

void portal_draw(void) {
    if (!took_input)
        return;
    A.in = saved_in;
    took_input = false;
    if (!C.active)
        return;
    if (strcmp(C.loaded, C.dir))
        load();
    bool save = strcmp(C.mode, "open") != 0;
    float e = (float)(A.now - C.opened_at) / 180.0f;
    e = e > 1 ? 1 : e;
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), 0.32f * e));

    int w = A.W - 80 < 900 ? A.W - 80 : 900, h = A.H - 80 < 600 ? A.H - 80 : 600;
    rect r = {(A.W - w) / 2, (A.H - h) / 2 + (int)((1 - e) * 16), w, h};
    float rad = D.radius + 6;
    shadow_round(A.frame, (rect){r.x, r.y + 16, r.w, r.h}, rad, 48, RGBA(0, 0, 0, 110));
    fill_round(A.frame, r, rad, D.surface | 0xff000000u);
    stroke_round(A.frame, r, rad, 1, color_alpha(D.surface_text, 0.12f));
    color ink = theme_fit(D.surface_text, D.surface | 0xff000000u, 4.5f);
    color muted = theme_fit(D.muted, D.surface | 0xff000000u, 4.5f);

    /* title, and who is asking */
    font *ft = FH(D.body_px + 5), *fb = FB(D.body_px), *fs = FB(D.body_px - 2 >= 12 ? D.body_px - 2 : 12);
    draw_text(A.frame, ft, r.x + 24, r.y + 18, C.title, ink);
    if (C.app[0]) {
        char who[160];
        snprintf(who, sizeof who, "asked by %s", app_label(C.app));
        draw_text(A.frame, fs, r.x + 24, r.y + 18 + font_line_height(ft) + 2, who, muted);
    }

    /* places */
    static const struct {
        const char *label, *path;
        int glyph;
    } places[] = {{"Home", HOME_DIR, G_HOME},
                  {"Documents", HOME_DIR "/Documents", G_DOC},
                  {"Downloads", HOME_DIR "/Downloads", G_DOWNLOAD},
                  {"Pictures", HOME_DIR "/Pictures", G_PICTURE},
                  {"Music", HOME_DIR "/Music", G_MUSIC},
                  {"Computer", "/", G_DESKTOP}};
    int side = 170, top = r.y + 76, bottom = r.y + r.h - (save ? 128 : 76);
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++) {
        rect pr = {r.x + 14, top + (int)i * 40, side - 14, 36};
        bool here = !strcmp(C.dir, places[i].path);
        if (here || hit(pr))
            fill_round(A.frame, pr, D.radius * 0.5f + 2, color_alpha(D.accent, here ? 0.18f : 0.08f));
        ui_glyph(A.frame, (rect){pr.x + 10, pr.y + 8, 20, 20}, places[i].glyph, 1.6f, here ? D.accent : ink);
        draw_text(A.frame, fb, pr.x + 40, pr.y + (36 - font_line_height(fb)) / 2, places[i].label, ink);
        if (hit(pr) && A.in.clicked)
            go(places[i].path);
    }

    /* where we are, with a way up */
    rect up = {r.x + side + 14, top, 36, 36};
    if (hit(up))
        fill_round(A.frame, up, 18, color_alpha(D.surface_text, 0.08f));
    ui_glyph(A.frame, (rect){up.x + 8, up.y + 8, 20, 20}, G_UP, 1.8f, ink);
    if (hit(up) && A.in.clicked && strcmp(C.dir, "/")) {
        char parent[PATHN];
        snprintf(parent, sizeof parent, "%s", C.dir);
        char *s = strrchr(parent, '/');
        if (s == parent)
            s[1] = 0;
        else if (s)
            *s = 0;
        go(parent);
    }
    draw_text(A.frame, fb, up.x + 48, up.y + (36 - font_line_height(fb)) / 2, C.dir, ink);

    /* the folder's contents */
    rect list = {r.x + side + 14, top + 48, r.w - side - 38, bottom - top - 56};
    fill_round(A.frame, list, D.radius * 0.5f + 2, color_alpha(D.surface_text, 0.035f));
    int rowh = (int)(D.body_px * 2.3f) < 34 ? 34 : (int)(D.body_px * 2.3f);
    int visible = list.h / rowh;
    if (hit(list) && saved_in.wheel)
        C.scroll -= saved_in.wheel * 3;
    if (C.scroll > C.n - visible)
        C.scroll = C.n - visible;
    if (C.scroll < 0)
        C.scroll = 0;
    canvas_clip(A.frame, list);
    for (int i = C.scroll; i < C.n && i < C.scroll + visible + 1; i++) {
        struct pent *en = &C.e[i];
        rect row = {list.x + 4, list.y + 4 + (i - C.scroll) * rowh, list.w - 8, rowh - 2};
        bool sel = en->sel || i == C.cursor;
        if (sel || hit(row))
            fill_round(A.frame, row, D.radius * 0.4f + 2, color_alpha(D.accent, sel ? 0.20f : 0.07f));
        char full[PATHN];
        full_path(en->name, full, sizeof full);
        file_icon(A.frame, (rect){row.x + 8, row.y + (rowh - 26) / 2, 24, 24}, file_kind(full, en->dir), en->name, sel);
        draw_text(A.frame, fb, row.x + 42, row.y + (rowh - font_line_height(fb)) / 2, en->name, ink);
        char info[48];
        if (en->dir)
            snprintf(info, sizeof info, "folder");
        else if (en->size < 1024)
            snprintf(info, sizeof info, "%lld bytes", en->size);
        else if (en->size < 1024 * 1024)
            snprintf(info, sizeof info, "%lld KB", en->size / 1024);
        else
            snprintf(info, sizeof info, "%.1f MB", en->size / 1048576.0);
        char when[32];
        fmt_when(en->mtime, when, sizeof when);
        int iw = text_width(fs, info);
        draw_text(A.frame, fs, row.x + row.w - 140 - iw, row.y + (rowh - font_line_height(fs)) / 2, info, muted);
        draw_text(A.frame, fs, row.x + row.w - 120, row.y + (rowh - font_line_height(fs)) / 2, when, muted);
        if (hit(row) && A.in.clicked) {
            bool twice = C.click_i == i && A.now - C.click_at < 450;
            C.click_i = i;
            C.click_at = A.now;
            if (twice) {
                open_entry(i);
                break;
            }
            if (C.multiple && A.in.ctrl) {
                en->sel = !en->sel;
            } else {
                for (int k = 0; k < C.n; k++)
                    C.e[k].sel = k == i;
            }
            C.cursor = i;
            if (save && !en->dir) {
                snprintf(C.name.buf, sizeof C.name.buf, "%s", en->name);
                C.name.len = (int)strlen(C.name.buf);
            }
        }
    }
    canvas_unclip(A.frame);
    if (!C.n)
        draw_text_centered(A.frame, fs, list, C.err[0] ? C.err : C.directory ? "No folders here" : "Nothing here",
                           muted);
    if (!C.active)
        return; /* a double click finished it */

    /* the name to save as */
    if (save) {
        rect nr = {r.x + side + 14, bottom + 4, r.w - side - 38, 44};
        draw_text(A.frame, fs, nr.x, nr.y - font_line_height(fs) - 2, "Name", muted);
        ui_textfield(nr, &C.name, true, "file name", fb, color_alpha(D.surface_text, 0.06f), ink, muted,
                     D.radius * 0.5f + 4);
    }

    /* filter, cancel, accept */
    int by = r.y + r.h - 60;
    rect ok = {r.x + r.w - 24 - 130, by, 130, 44};
    rect cancel = {ok.x - 12 - 120, by, 120, 44};
    bool can = save ? C.name.len > 0 : C.directory || C.cursor >= 0;
    if (C.nfilters > 1) {
        char lab[96];
        snprintf(lab, sizeof lab, "%s  \xe2\x96\xbe", C.filters[C.filter].name);
        rect fr = {r.x + 24, by, text_width(fb, lab) + 36, 44};
        if (button(fr, lab, false, true)) {
            C.filter = (C.filter + 1) % C.nfilters;
            load();
        }
    } else if (C.nfilters == 1) {
        draw_text(A.frame, fs, r.x + 24, by + (44 - font_line_height(fs)) / 2, C.filters[0].name, muted);
    }
    if (button(cancel, "Cancel", false, true)) {
        answer(1, NULL, 0);
        return;
    }
    if (button(ok, C.accept, true, can)) {
        choose();
        return;
    }

    /* keys: arrows, Enter, Escape, typing the name */
    if (save)
        ui_type(&C.name);
    for (int i = 0; i < A.in.nkeys && C.active; i++) {
        int k = A.in.keys[i];
        if (k == K_ESC) {
            answer(1, NULL, 0);
        } else if (k == K_ENTER) {
            if (!save && C.cursor >= 0 && C.e[C.cursor].dir)
                open_entry(C.cursor);
            else
                choose();
        } else if (k == K_DOWN || k == K_UP) {
            int c = C.cursor + (k == K_DOWN ? 1 : -1);
            if (c >= 0 && c < C.n) {
                C.cursor = c;
                for (int j = 0; j < C.n; j++)
                    C.e[j].sel = j == c;
                if (c < C.scroll)
                    C.scroll = c;
                if (c >= C.scroll + visible)
                    C.scroll = c - visible + 1;
            }
        } else if (k == K_BACKSPACE && !save && strcmp(C.dir, "/")) {
            char parent[PATHN];
            snprintf(parent, sizeof parent, "%s/..", C.dir);
            go(parent);
        }
    }
}
