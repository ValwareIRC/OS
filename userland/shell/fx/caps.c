/* What a Facet app can do, from its code; what it has been allowed; the checks every call goes through
 * (caps.h says the rules). */
#define _GNU_SOURCE
#include "caps.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *private_place(const char *h, const char *p);

const char *const fx_categories[] = {"notes", "todo", "writing", "files", "photos", "music", "video", "chat",
                                     "mail", "news", "weather", "browser", "maps", "game", "clock", "calculator",
                                     "utility", "developer", "finance", "health", "education", "creative", "other",
                                     NULL};

bool fx_category_ok(const char *c) {
    for (int i = 0; c && fx_categories[i]; i++)
        if (!strcmp(c, fx_categories[i]))
            return true;
    return false;
}

/* ---- reading the code ---- */

typedef struct {
    fx_manifest *m;
    bool required;  /* inside the app's data: used as it opens */
    /* paths and addresses made while it runs: confined to the places the code names (or needing all files) */
    bool dyn_read, dyn_write;
    int dyn_read_line, dyn_write_line;
    bool dyn_read_req, dyn_write_req;
} walk;

static void add(walk *w, int kind, const char *what, bool dynamic, int line) {
    fx_manifest *m = w->m;
    for (int i = 0; i < m->n; i++)
        if (m->c[i].kind == kind && !strcmp(m->c[i].what, what)) {
            m->c[i].required |= w->required;
            m->c[i].dynamic |= dynamic;
            return;
        }
    if (m->n >= FX_MAX_CAPS) {
        /* more than can be listed: the widest of its kind instead */
        kind = kind == FXC_HOST ? FXC_INTERNET : kind;
        what = kind == FXC_INTERNET ? "" : "~";
        for (int i = 0; i < m->n; i++)
            if (m->c[i].kind == kind && !strcmp(m->c[i].what, what))
                return;
        m->n = FX_MAX_CAPS - 1;
    }
    fx_cap *c = &m->c[m->n++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    snprintf(c->what, sizeof c->what, "%s", what);
    c->required = w->required, c->dynamic = dynamic, c->line = line;
}

static void forbid(walk *w, int line, const char *why) {
    if (w->m->forbidden)
        return;
    w->m->forbidden = true;
    snprintf(w->m->forbidden_why, sizeof w->m->forbidden_why, "line %d: %s", line, why);
}

/* The text an expression certainly starts with (literal text, the text parts leading a template, the left of a
 * "+" chain); *whole when that is all of it. */
static void prefix_of(const fx_expr *x, char *out, size_t n, bool *whole) {
    out[0] = 0;
    *whole = false;
    if (!x)
        return;
    if (x->k == E_TEXT) {
        snprintf(out, n, "%s", x->s ? x->s : "");
        *whole = true;
    } else if (x->k == E_TEMPLATE) {
        size_t len = 0;
        int i = 0;
        for (; i < x->n && x->items[i]->k == E_TEXT; i++) {
            size_t l = strlen(x->items[i]->s);
            if (len + l + 1 > n)
                return;
            memcpy(out + len, x->items[i]->s, l + 1);
            len += l;
        }
        *whole = i == x->n;
    } else if (x->k == E_BINARY && x->s && !strcmp(x->s, "+")) {
        bool lw;
        prefix_of(x->a, out, n, &lw);
        if (lw) {
            size_t l = strlen(out);
            bool rw;
            prefix_of(x->b, out + l, n - l, &rw);
            *whole = rw;
        }
    }
}

/* a path as the code writes it, made a place: "~", "~/Notes", "~/.water"; "" if it can't be one */
static bool norm_place(const char *p, char *out, size_t n, bool *outside, bool *dotdot) {
    *outside = *dotdot = false;
    char buf[512];
    if (!p[0])
        return false;
    if (strlen(p) + 3 > sizeof buf) { /* never cut short (a shorter path is a wider place) */
        *dotdot = true;
        return false;
    }
    if (p[0] == '~' && (p[1] == '/' || !p[1]))
        snprintf(buf, sizeof buf, "~%s", p + 1);
    else if (p[0] == '/') {
        *outside = true; /* an absolute path: system files (home is only ever written "~") */
        return false;
    } else
        snprintf(buf, sizeof buf, "~/%s", p); /* relative: in home */
    /* clean: no "//", no "/." parts; ".." is never a place */
    char clean[512] = "~";
    size_t len = 1;
    for (char *save, *t = strtok_r(buf + 1, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, "."))
            continue;
        if (!strcmp(t, "..")) {
            *dotdot = true;
            return false;
        }
        int k = snprintf(clean + len, sizeof clean - len, "/%s", t);
        if (k < 0 || (size_t)k >= sizeof clean - len)
            return false;
        len += (size_t)k;
    }
    /* never cut short: a shortened place would be a wider one */
    if (len >= n || len >= sizeof ((fx_cap *)0)->what) {
        *dotdot = true;
        return false;
    }
    memcpy(out, clean, len + 1);
    return true;
}

static void path_use(walk *w, const fx_expr *arg, bool write, bool listing, int line) {
    char pre[512];
    bool whole;
    prefix_of(arg, pre, sizeof pre, &whole);
    if (!whole) { /* the place is the folder the known start of the path is in */
        char *slash = strrchr(pre, '/');
        if (slash)
            slash[slash == pre ? 1 : 0] = 0;
        else if (strcmp(pre, "~"))
            pre[0] = 0;
    }
    if (listing && whole && !pre[0])
        snprintf(pre, sizeof pre, "~");
    char place[256];
    bool outside, dotdot;
    if (pre[0] && norm_place(pre, place, sizeof place, &outside, &dotdot)) {
        const char *priv = private_place("~", place);
        if (priv) {
            char why[160];
            snprintf(why, sizeof why, "it reaches for ~/%s, which is private (keys, other apps' data or the system's own)", priv);
            forbid(w, line, why);
            return;
        }
        add(w, write ? FXC_WRITE : FXC_READ, place, !whole, line);
        return;
    }
    if (pre[0] && outside) {
        forbid(w, line, "it reaches for files outside your home (system files)");
        return;
    }
    if (pre[0] && dotdot) {
        forbid(w, line, "a path climbs out of its folder with \"..\", or is too long to be a place");
        return;
    }
    /* made entirely while it runs */
    if (write) {
        if (!w->dyn_write)
            w->dyn_write_line = line;
        w->dyn_write = true, w->dyn_write_req |= w->required;
    } else {
        if (!w->dyn_read)
            w->dyn_read_line = line;
        w->dyn_read = true, w->dyn_read_req |= w->required;
    }
}

static bool host_ok(const char *h) {
    if (!h[0] || strlen(h) > 190)
        return false;
    for (const char *p = h; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-'))
            return false;
    return h[0] != '.' && h[0] != '-';
}

bool fx_url_host(const char *url, char *host, size_t n) {
    host[0] = 0;
    const char *p;
    if (!strncasecmp(url, "https://", 8))
        p = url + 8;
    else if (!strncasecmp(url, "http://", 7))
        p = url + 7;
    else
        return false;
    const char *at = NULL; /* user:password@ is not part of the server */
    const char *end = p + strcspn(p, "/?#");
    for (const char *q = p; q < end; q++)
        if (*q == '@')
            at = q;
    if (at)
        p = at + 1;
    size_t len = strcspn(p, ":/?#");
    if (len == 0 || len >= n)
        return false;
    for (size_t i = 0; i < len; i++)
        host[i] = (char)tolower((unsigned char)p[i]);
    host[len] = 0;
    return host_ok(host);
}

/* a URL argument: its server named in the code if the text fixes it (the host and what ends it are all in the
 * known start), else any server */
static void url_use(walk *w, const fx_expr *arg, int line) {
    char pre[600], host[200];
    bool whole;
    prefix_of(arg, pre, sizeof pre, &whole);
    const char *p = strstr(pre, "://");
    bool fixed = false;
    if (p && fx_url_host(pre, host, sizeof host)) {
        const char *hs = p + 3, *at = strchr(hs, '@');
        if (at && at < hs + strcspn(hs, "/?#"))
            hs = at + 1;
        size_t hl = strcspn(hs, ":/?#");
        /* fixed when something ends the host inside the known text, or the whole URL is known */
        fixed = whole || hs[hl] != 0;
        if (fixed && hs[hl] == ':') { /* a port written after it must be whole too */
            const char *q = hs + hl + 1;
            while (isdigit((unsigned char)*q))
                q++;
            fixed = whole || *q != 0;
        }
    }
    if (fixed)
        add(w, FXC_HOST, host, false, line);
    else
        add(w, FXC_INTERNET, "", true, line);
}

static void socket_use(walk *w, const fx_expr *hostx, const fx_expr *portx, int line) {
    char pre[256], what[220];
    bool whole;
    prefix_of(hostx, pre, sizeof pre, &whole);
    for (char *q = pre; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    if (whole && host_ok(pre)) {
        if (portx && portx->k == E_NUM && portx->num >= 1 && portx->num <= 65535)
            snprintf(what, sizeof what, "%.190s:%d", pre, (int)portx->num);
        else
            snprintf(what, sizeof what, "%.190s:*", pre);
        add(w, FXC_HOST, what, false, line);
    } else {
        add(w, FXC_INTERNET, "", true, line);
    }
}

static void walk_expr(walk *w, const fx_expr *x);

static void call_use(walk *w, const fx_expr *x) {
    const char *f = x->s ? x->s : "";
    const fx_expr *a0 = x->n > 0 ? x->items[0] : NULL, *a1 = x->n > 1 ? x->items[1] : NULL;
    if (!strcmp(f, "fs.read"))
        path_use(w, a0, false, false, x->line);
    else if (!strcmp(f, "fs.write") || !strcmp(f, "fs.remove") || !strcmp(f, "fs.mkdir"))
        path_use(w, a0, true, false, x->line);
    else if (!strcmp(f, "fs.rename")) {
        path_use(w, a0, true, false, x->line);
        path_use(w, a1, true, false, x->line);
    } else if (!strcmp(f, "files"))
        path_use(w, a0, false, true, x->line);
    else if (!strcmp(f, "file"))
        path_use(w, a0, false, false, x->line);
    else if (!strcmp(f, "app.open") && a1) /* a file shown in another app is read through it */
        path_use(w, a1, false, false, x->line);
    else if (!strcmp(f, "web") || !strcmp(f, "net.get") || !strcmp(f, "net.post"))
        url_use(w, a0, x->line);
    else if (!strcmp(f, "net.connect"))
        socket_use(w, a0, a1, x->line);
}

static void walk_expr(walk *w, const fx_expr *x) {
    if (!x)
        return;
    if (x->k == E_CALL)
        call_use(w, x);
    walk_expr(w, x->a), walk_expr(w, x->b), walk_expr(w, x->c);
    for (int i = 0; i < x->n; i++)
        if (x->items)
            walk_expr(w, x->items[i]);
}

static void walk_stmts(walk *w, fx_stmt **s, int n) {
    for (int i = 0; i < n; i++) {
        if (!s[i])
            continue;
        walk_expr(w, s[i]->e);
        walk_stmts(w, s[i]->body, s[i]->nbody);
        walk_stmts(w, s[i]->alt, s[i]->nalt);
    }
}

static void walk_node(walk *w, const fx_node *n) {
    if (!n)
        return;
    walk_expr(w, n->text);
    for (int i = 0; i < n->nattrs; i++)
        walk_expr(w, n->attrs[i].e);
    for (int i = 0; i < n->nkids; i++)
        walk_node(w, n->kids[i]);
}

static void walk_app(walk *w, const fx_app *app) {
    w->required = true; /* what it shows as it opens */
    for (int i = 0; i < app->ndata; i++)
        walk_expr(w, app->data[i].e);
    w->required = false;
    for (int i = 0; i < app->nstate; i++)
        walk_expr(w, app->state[i].e);
    for (int i = 0; i < app->nderived; i++)
        walk_expr(w, app->derived[i].e);
    for (int i = 0; i < app->nfns; i++)
        walk_stmts(w, app->fns[i].body, app->fns[i].nbody);
    walk_node(w, app->view);
    for (int i = 0; i < app->ncomps; i++)
        walk_app(w, &app->comps[i]);
}

static int cap_order(const void *a, const void *b) {
    const fx_cap *x = a, *y = b;
    if (x->kind != y->kind)
        return x->kind - y->kind;
    return strcmp(x->what, y->what);
}

/* is place b inside place a (or the same)? */
static bool within(const char *a, const char *b) {
    size_t l = strlen(a);
    return !strncmp(a, b, l) && (b[l] == 0 || b[l] == '/');
}

void fx_manifest_of(const fx_app *app, fx_manifest *m) {
    memset(m, 0, sizeof *m);
    snprintf(m->app, sizeof m->app, "%s", app && app->name ? app->name : "?");
    snprintf(m->category, sizeof m->category, "%s", app && app->category && fx_category_ok(app->category) ? app->category : "other");
    if (!app)
        return;
    walk w = {.m = m};
    walk_app(&w, app);
    /* paths made while it runs: confined to every place the code names; none named: all your files */
    for (int pass = 0; pass < 2; pass++) {
        bool dyn = pass ? w.dyn_write : w.dyn_read;
        if (!dyn)
            continue;
        int kind = pass ? FXC_WRITE : FXC_READ;
        int line = pass ? w.dyn_write_line : w.dyn_read_line;
        w.required = pass ? w.dyn_write_req : w.dyn_read_req;
        int places = 0;
        int n0 = m->n;
        for (int i = 0; i < n0; i++)
            if (m->c[i].kind == FXC_READ || m->c[i].kind == FXC_WRITE) {
                add(&w, kind, m->c[i].what, true, line);
                places++;
            }
        if (!places)
            add(&w, kind, "~", true, line);
    }
    /* a place inside a wider one of the same kind says nothing more */
    for (int i = 0; i < m->n; i++)
        for (int j = 0; j < m->n; j++)
            if (i != j && m->c[i].kind == m->c[j].kind && (m->c[i].kind == FXC_READ || m->c[i].kind == FXC_WRITE) &&
                strcmp(m->c[i].what, m->c[j].what) && within(m->c[j].what, m->c[i].what)) {
                m->c[j].required |= m->c[i].required;
                m->c[j].dynamic |= m->c[i].dynamic;
                m->c[i] = m->c[--m->n];
                i = -1;
                break;
            }
    /* the general internet covers named servers: they are still listed, so the person sees them */
    qsort(m->c, (size_t)m->n, sizeof m->c[0], cap_order);
}

void fx_cap_key(const fx_cap *c, char *out, size_t n) {
    static const char *const k[] = {"read", "write", "host", "internet"};
    if (c->kind == FXC_INTERNET)
        snprintf(out, n, "internet");
    else
        snprintf(out, n, "%s:%s", k[c->kind], c->what);
}

bool fx_cap_from_key(const char *key, fx_cap *c) {
    memset(c, 0, sizeof *c);
    static const char *const k[] = {"read:", "write:", "host:"};
    if (!strcmp(key, "internet")) {
        c->kind = FXC_INTERNET;
        return true;
    }
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(k[i]);
        if (!strncmp(key, k[i], l) && key[l]) {
            c->kind = i;
            snprintf(c->what, sizeof c->what, "%s", key + l);
            return true;
        }
    }
    return false;
}

/* "~/Notes" -> "Notes (~/Notes)"; "~" -> all of it */
static const char *place_words(const char *p, char *buf, size_t n) {
    const char *last = strrchr(p, '/');
    snprintf(buf, n, "%s (%s)", last ? last + 1 : p, p);
    return buf;
}

void fx_cap_says(const fx_cap *c, char *out, size_t n) {
    char pw[260];
    switch (c->kind) {
    case FXC_READ:
        if (!strcmp(c->what, "~"))
            snprintf(out, n, "can read any of your files, and see what is in every folder of yours");
        else
            snprintf(out, n, "can read %s", place_words(c->what, pw, sizeof pw));
        break;
    case FXC_WRITE:
        if (!strcmp(c->what, "~"))
            snprintf(out, n, "can change, replace and delete any of your files");
        else
            snprintf(out, n, "can change, replace and delete %s", place_words(c->what, pw, sizeof pw));
        break;
    case FXC_HOST: {
        const char *colon = strchr(c->what, ':');
        if (colon)
            snprintf(out, n, "can keep a connection open to %.*s%s%s, sending and receiving anything you type or open in it",
                     (int)(colon - c->what), c->what, strcmp(colon, ":*") ? " on port " : "", strcmp(colon, ":*") ? colon + 1 : "");
        else
            snprintf(out, n, "can send to and receive from %s anything you type or open in it", c->what);
        break;
    }
    default:
        snprintf(out, n, "can connect to any server on the internet, and could send anything you type or open in it to anyone");
    }
}

/* ---- grants ---- */

void fx_grants_set(fx_grants *g, const char *key, bool yes) {
    for (int i = 0; i < g->n; i++)
        if (!strcmp(g->key[i], key)) {
            g->yes[i] = yes;
            return;
        }
    if (g->n >= FX_MAX_CAPS)
        return;
    snprintf(g->key[g->n], sizeof g->key[0], "%s", key);
    g->yes[g->n++] = yes;
}

int fx_grants_get(const fx_grants *g, const char *key) {
    for (int i = 0; g && i < g->n; i++)
        if (!strcmp(g->key[i], key))
            return g->yes[i] ? 1 : 0;
    return -1;
}

void fx_grants_all(fx_grants *g, const fx_manifest *m, bool yes) {
    for (int i = 0; i < m->n; i++) {
        char k[220];
        fx_cap_key(&m->c[i], k, sizeof k);
        fx_grants_set(g, k, yes);
    }
}

char *fx_grants_text(const fx_grants *g) {
    size_t cap = 64 + (size_t)g->n * 240;
    char *s = malloc(cap), *p = s;
    *p = 0;
    for (int i = 0; i < g->n; i++)
        p += snprintf(p, cap - (size_t)(p - s), "%s %s\n", g->yes[i] ? "allow" : "deny", g->key[i]);
    return s;
}

void fx_grants_parse(fx_grants *g, const char *text) {
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[300];
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
        if (!strncmp(line, "allow ", 6))
            fx_grants_set(g, line + 6, true);
        else if (!strncmp(line, "deny ", 5))
            fx_grants_set(g, line + 5, false);
        p = e ? e + 1 : NULL;
    }
}

/* ---- the run-time checks ---- */

static const char *home_of(const fx_grants *g) {
    if (g && g->home[0])
        return g->home;
    const char *h = getenv("HOME");
    return h && *h ? h : "/home/user";
}

/* the real path: absolute, "." and ".." gone, links followed as far as what exists; false if it can't be one */
static bool real_path(const char *home, const char *path, char *out, size_t n) {
    char abs[PATH_MAX * 2];
    if (path[0] == '~' && (path[1] == '/' || !path[1]))
        snprintf(abs, sizeof abs, "%s%s", home, path + 1);
    else if (path[0] == '/')
        snprintf(abs, sizeof abs, "%s", path);
    else
        snprintf(abs, sizeof abs, "%s/%s", home, path);
    /* lexically first: a/b/../c is a/c (so a missing folder can't hide a climb) */
    char clean[PATH_MAX * 2] = "";
    size_t len = 0;
    char tmp[PATH_MAX * 2];
    snprintf(tmp, sizeof tmp, "%s", abs);
    for (char *save, *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, "."))
            continue;
        if (!strcmp(t, "..")) {
            char *s = strrchr(clean, '/');
            if (s)
                *s = 0, len = (size_t)(s - clean);
            continue;
        }
        int k = snprintf(clean + len, sizeof clean - len, "/%s", t);
        if (k < 0 || (size_t)k >= sizeof clean - len)
            return false;
        len += (size_t)k;
    }
    if (!clean[0])
        snprintf(clean, sizeof clean, "/");
    /* then links: the deepest part that exists, resolved; the rest added (it can't be a link: it isn't there) */
    char head[PATH_MAX * 2], rest[PATH_MAX * 2] = "";
    snprintf(head, sizeof head, "%s", clean);
    for (;;) {
        char r[PATH_MAX];
        if (realpath(head, r)) {
            int k = snprintf(out, n, "%s%s", strcmp(r, "/") ? r : "", rest);
            if (k < 0 || (size_t)k >= n)
                return false;
            if (!out[0])
                snprintf(out, n, "/");
            return true;
        }
        if (errno != ENOENT && errno != ENOTDIR)
            return false;
        char *s = strrchr(head, '/');
        if (!s || s == head) {
            snprintf(out, n, "%s", clean);
            return true;
        }
        char t2[PATH_MAX * 2];
        snprintf(t2, sizeof t2, "%s%s", s, rest);
        snprintf(rest, sizeof rest, "%s", t2);
        *s = 0;
    }
}

static bool inside(const char *root, const char *p) {
    size_t l = strlen(root);
    if (l == 1 && root[0] == '/')
        return true;
    return !strncmp(root, p, l) && (p[l] == 0 || p[l] == '/');
}

/* In home, but never an app's, whatever it asks and whatever it is allowed: keys and passwords, other apps' data
 * (Flatpak apps' homes, browsers' profiles), the trash, the companion's and the system's own. */
static const char *const private_places[] = {".ssh", ".gnupg", ".pki", ".password-store", ".local/share/keyrings",
                                             ".var", ".mozilla", ".config", ".local", ".cache", ".trash", ".netrc",
                                             ".bash_history", ".aws", ".kube", ".docker", NULL};

/* p (real) inside one of them, under home h */
static const char *private_place(const char *h, const char *p) {
    size_t hl = strlen(h);
    if (strncmp(h, p, hl) || p[hl] != '/')
        return NULL;
    const char *rel = p + hl + 1;
    for (int i = 0; private_places[i]; i++) {
        size_t l = strlen(private_places[i]);
        if (!strncmp(rel, private_places[i], l) && (rel[l] == 0 || rel[l] == '/'))
            return private_places[i];
    }
    return NULL;
}

/* any hidden part ("~/.config", ".git") below root: an app's data, not the person's files */
static bool hidden_below(const char *root, const char *p) {
    const char *q = p + strlen(root);
    while (*q) {
        if (*q == '/' && q[1] == '.')
            return true;
        q++;
    }
    return false;
}

bool fx_path_allowed(const fx_grants *g, const char *path, bool write, char *out, size_t n, char *why, size_t whyn) {
    const char *home = home_of(g);
    char realhome[PATH_MAX];
    if (!realpath(home, realhome))
        snprintf(realhome, sizeof realhome, "%s", home);
    if (!path || !*path || !real_path(realhome, path, out, n)) {
        snprintf(why, whyn, "\"%s\" isn't a path it may use", path ? path : "");
        return false;
    }
    if (!inside(realhome, out)) {
        snprintf(why, whyn, "%s is outside your home: apps never reach system files", out);
        return false;
    }
    const char *priv = private_place(realhome, out);
    if (priv) {
        snprintf(why, whyn, "~/%s is private (keys, other apps' data or the system's own): no app may use it", priv);
        return false;
    }
    for (int i = 0; g && i < g->n; i++) {
        if (!g->yes[i])
            continue;
        const char *k = g->key[i];
        bool kw = !strncmp(k, "write:", 6), kr = !strncmp(k, "read:", 5);
        if (!(kw || (kr && !write)))
            continue;
        char root[PATH_MAX * 2];
        if (!real_path(realhome, k + (kw ? 6 : 5), root, sizeof root))
            continue;
        if (inside(root, out) && !hidden_below(root, out))
            return true;
    }
    snprintf(why, whyn, "it isn't allowed to %s %s", write ? "change" : "read", out);
    return false;
}

bool fx_net_allowed(const fx_grants *g, const char *host, int port, char *why, size_t whyn) {
    char h[200];
    snprintf(h, sizeof h, "%s", host ? host : "");
    for (char *q = h; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    if (!host_ok(h)) {
        snprintf(why, whyn, "\"%s\" isn't a server name", host ? host : "");
        return false;
    }
    if (fx_grants_get(g, "internet") == 1)
        return true;
    char k[260];
    if (port == 0) {
        snprintf(k, sizeof k, "host:%s", h);
        if (fx_grants_get(g, k) == 1)
            return true;
    } else {
        snprintf(k, sizeof k, "host:%s:%d", h, port);
        if (fx_grants_get(g, k) == 1)
            return true;
        snprintf(k, sizeof k, "host:%s:*", h);
        if (fx_grants_get(g, k) == 1)
            return true;
    }
    snprintf(why, whyn, "it isn't allowed to reach %s", h);
    return false;
}

/* ---- before it runs ---- */

void fx_gate_of(const fx_manifest *m, const fx_grants *g, fx_gate *out) {
    memset(out, 0, sizeof *out);
    out->can_run = !m->forbidden;
    for (int i = 0; i < m->n; i++) {
        char k[220];
        fx_cap_key(&m->c[i], k, sizeof k);
        int v = fx_grants_get(g, k);
        if (v < 0) {
            out->newcaps[out->nnew++] = i;
            out->can_run = false; /* anything never decided waits for them */
        } else if (v == 1) {
            out->held[out->nheld++] = i;
        } else if (m->c[i].required) {
            out->refused_required++;
            out->can_run = false;
        }
    }
}

/* ---- mismatches ---- */

/* what a kind of app is expected to reach: 'n' the network (named servers), 'i' any server, 'f' all files */
static const struct {
    const char *cat;
    const char *expects;
} expect[] = {
    {"notes", ""},       {"todo", ""},          {"writing", ""},     {"calculator", ""}, {"clock", ""},
    {"game", "n"},       {"files", "f"},        {"photos", ""},      {"music", "n"},     {"video", "n"},
    {"chat", "ni"},      {"mail", "ni"},        {"news", "n"},       {"weather", "n"},   {"browser", "nif"},
    {"maps", "n"},       {"utility", ""},       {"developer", "nif"}, {"finance", "n"},  {"health", ""},
    {"education", "n"},  {"creative", ""},      {"other", ""},
};

int fx_mismatch(const fx_manifest *m, char *out, size_t n) {
    out[0] = 0;
    const char *ex = "";
    for (size_t i = 0; i < sizeof expect / sizeof *expect; i++)
        if (!strcmp(expect[i].cat, m->category))
            ex = expect[i].expects;
    bool any_net = false, named = false, all_read = false, all_write = false;
    for (int i = 0; i < m->n; i++) {
        any_net |= m->c[i].kind == FXC_INTERNET;
        named |= m->c[i].kind == FXC_HOST;
        all_read |= m->c[i].kind == FXC_READ && !strcmp(m->c[i].what, "~");
        all_write |= m->c[i].kind == FXC_WRITE && !strcmp(m->c[i].what, "~");
    }
    int level = 0;
    size_t len = 0;
#define SAY(lv, ...)                                                                                         \
    do {                                                                                                     \
        if ((lv) > level)                                                                                    \
            level = (lv);                                                                                    \
        int k_ = snprintf(out + len, n - len, "%s", len ? " " : "");                                         \
        len += k_ > 0 ? (size_t)k_ : 0;                                                                      \
        k_ = snprintf(out + len, n - len, __VA_ARGS__);                                                      \
        len += k_ > 0 && (size_t)k_ < n - len ? (size_t)k_ : 0;                                              \
    } while (0)
    bool other = !strcmp(m->category, "other"); /* it didn't say what it is: only what is wrong for any app */
    const char *A = strchr("aeiou", m->category[0]) ? "An" : "A";
    bool net_expected = strchr(ex, 'n') || strchr(ex, 'i');
    /* the pattern that steals: reading everything you have, and a way to send it away */
    if ((all_read || all_write) && (any_net || named) && !(strchr(ex, 'f') && net_expected))
        SAY(2, "It can read all of your files and send them to %s.",
            any_net ? "any server on the internet" : "the servers listed");
    else if (any_net && !strchr(ex, 'i') && !other)
        SAY(2, "%s %s app has no need to reach any server on the internet, and this one can.", A, m->category);
    else if (named && !net_expected && !other)
        SAY(1, "%s %s app usually doesn't use the internet; this one talks to the servers listed.", A, m->category);
    if (all_write && !strchr(ex, 'f')) {
        if (other)
            SAY(1, "It can change or delete any of your files.");
        else
            SAY(2, "%s %s app has no need to change or delete any of your files, and this one can.", A, m->category);
    } else if (all_read && !strchr(ex, 'f') && !any_net && !named && !other) {
        SAY(1, "It can read any of your files, more than %s %s app usually needs.", A[1] ? "an" : "a", m->category);
    }
#undef SAY
    return level;
}
