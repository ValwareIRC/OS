/* Facet 2 apps in windows (docs/facet2.md).
 *
 * Each window runs one app file in the interpreter (fx/eval.c). Its view is
 * turned into a small Facet document and drawn by the desktop's own engine,
 * so it takes the person's design: their colours, fonts and corners (the
 * app's token(...) values are this design's). Text inputs, multiline fields
 * and checkboxes are drawn here over the boxes the engine laid out for them.
 * ask() and prompt() put a question over everything and wait for it
 * (main_modal), the rest of the OS going on underneath. */
#define _GNU_SOURCE
#include "fxwin.h"
#include "appgate.h"
#include "app.h"
#include "files.h"
#include "settings.h"
#include "fm/fm.h"
#include "fx/fx_rt.h"
#include "fx/markup.h"
#include "alive.h"
#include "../lib/json.h"
#include "../lib/http.h"

#include <dirent.h>
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define D (A.d)
#define MAX_FIELDS 32

typedef struct {
    char key[256];
    char *buf;
    size_t len, cap;
    bool used, secret;
    text_edit ed; /* caret, selection, undo (textedit.h) */
    char *app_said; /* the app's value for it last frame: a change is the app's doing (Todo clearing its draft) */
} field;

struct fxwin {
    char path[400];
    char *src;
    fx_app *app;
    fx_rt *rt;
    fm_doc *doc;
    fx_out *view;       /* what doc was made from */
    fx_out **acts;      /* what="fx:N" */
    fx_grants *grants;  /* what the person allowed it (appgate.c); every call is checked against it */
    int nacts;
    field fields[MAX_FIELDS];
    char focus[256];    /* the focused input's key */
    bool autofocused, busy, rebuild;
    int pending;        /* an action clicked this frame, run after the engine is done with the pointer */
    struct {
        char key[256];
        float v;
        bool used;
    } anim[48];         /* widgets easing towards their values */
    int w;
    /* the network: web() data it keeps fresh, its sockets */
    struct {
        char url[512];
        fx_val value;
        double at, every;
        job *fetch;
    } web[8];
    struct fxsock *socks[16];
};

/* ---- files and the world, for the interpreter ---- */

static char *h_read(void *c, const char *p) {
    (void)c;
    FILE *f = fopen(p, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (8 << 20)) {
        fclose(f);
        return NULL;
    }
    char *s = malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    s[n] = 0;
    fclose(f);
    return s;
}
static bool h_write(void *c, const char *p, const char *t) {
    (void)c;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.saving", p);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fputs(t, f) >= 0;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, p) == 0; /* never half a file */
}
static bool h_remove(void *c, const char *p) {
    (void)c;
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
        return rmdir(p) == 0;
    return unlink(p) == 0;
}
static bool h_rename(void *c, const char *a, const char *b) { (void)c; return rename(a, b) == 0; }
static bool h_mkdir(void *c, const char *p) { (void)c; return mkdir(p, 0755) == 0 || errno == EEXIST; }

static fx_val h_files(void *c, const char *dir, const char *ending) {
    (void)c;
    fx_val l = fx_list();
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        size_t nl = strlen(e->d_name), el = ending ? strlen(ending) : 0;
        if (el && (nl < el || strcasecmp(e->d_name + nl - el, ending)))
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st))
            continue;
        fx_val r = fx_rec();
        fx_rec_set(r, "name", fx_text(e->d_name));
        fx_rec_set(r, "path", fx_text(path));
        fx_rec_set(r, "size", fx_num((double)st.st_size));
        fx_rec_set(r, "modified", fx_num((double)st.st_mtime * 1000));
        fx_rec_set(r, "dir", fx_bool(S_ISDIR(st.st_mode)));
        fx_list_push(l, r);
    }
    if (d)
        closedir(d);
    return l;
}

static fx_val h_setting(void *c, const char *k) {
    (void)c;
    if (!strcmp(k, "person.name")) /* who they are, and who it is */
        return their_name()[0] ? fx_text(their_name()) : fx_none();
    if (!strcmp(k, "companion.name"))
        return fx_text(D.ai_name);
    const char *v = settings_get(k, NULL);
    return v ? fx_text(v) : fx_none();
}

static fx_val h_apps(void *c) {
    (void)c;
    fx_val l = fx_list();
    for (int i = 0; i < fm_host_app_count(); i++) {
        const char *id = fm_host_app_id(i);
        if (!id || !strcmp(id, "power") || !strcmp(id, "redesign"))
            continue;
        fx_val r = fx_rec();
        fx_rec_set(r, "id", fx_text(id));
        fx_rec_set(r, "name", fx_text(fm_host_app_label(i)));
        fx_list_push(l, r);
    }
    return l; /* (Facet apps are in that list already) */
}

static double h_now(void *c) {
    (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static bool h_open(void *c, const char *id, const char *path) {
    (void)c;
    if (path && (!strcmp(id, "notes") || !strcmp(id, "files"))) {
        if (!strcmp(id, "notes"))
            open_in_notes(path);
        else
            desktop_open_path(path);
        return true;
    }
    char names[32][64], paths[32][300];
    int n = fxwin_list(names, paths, 32);
    for (int i = 0; i < n; i++)
        if (!strcasecmp(names[i], id))
            return fxwin_open(paths[i]) >= 0;
    char a[96];
    snprintf(a, sizeof a, "open:%s", id);
    fm_host_action(A.ui, NULL, a);
    return true;
}

static void h_close(void *c) {
    struct fxwin *fw = c;
    if (fw->w >= 0)
        A.win[fw->w].open = false; /* the window goes; freed when it is drawn no more (fxwin_draw) */
}

static void h_notify(void *c, const char *t) { (void)c; toast("%s", t); }

static void h_error(void *c, const char *m) {
    (void)c;
    LOG("facet: %s\n", m);
}

/* ---- the network ---- */

static int h_ask(void *c, const char *q);

struct http_job {
    char method[8], url[1024], ctype[96];
    char *body;
};

/* in a child: the request; its result as "status\x1ferror\x1fbody" */
static void http_child(void *arg, char **result) {
    struct http_job *j = arg;
    const char *hdr[3] = {NULL, NULL, NULL};
    char ct[128];
    if (j->ctype[0]) {
        snprintf(ct, sizeof ct, "Content-Type: %s", j->ctype);
        hdr[0] = ct;
    }
    struct http_response r;
    http_request(j->method, j->url, hdr, j->body, j->body ? strlen(j->body) : 0, 30, &r);
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "%d\x1f%s\x1f", r.status, r.error);
    if (r.body)
        sb_addn(&b, r.body, r.body_len);
    http_response_free(&r);
    *result = b.p ? b.p : strdup("0\x1fno answer\x1f");
}

static fx_val response_value(char *res) {
    fx_val r = fx_rec();
    int status = res ? atoi(res) : 0;
    char *e = res ? strchr(res, '\x1f') : NULL, *body = e ? strchr(e + 1, '\x1f') : NULL;
    if (body)
        *body++ = 0;
    const char *err = e ? e + 1 : "no answer";
    bool ok = status >= 200 && status < 300;
    fx_rec_set(r, "ok", fx_bool(ok));
    fx_rec_set(r, "status", fx_num(status));
    fx_rec_set(r, "data", body ? fx_parse_json(body, strlen(body)) : fx_none());
    fx_rec_set(r, "text", fx_text(body ? body : ""));
    fx_rec_set(r, "error", ok ? fx_none() : fx_text(status ? "the server said no" : err));
    if (!ok && status)
        fx_rec_set(r, "error", fx_text(err[0] ? err : "the server said no"));
    return r;
}

static struct {
    job *j;
    bool done;
    char *res;
    char host[128];
} REQ;

static void waiting_draw(void) { /* the request is on its way: a word at the bottom, the OS goes on */
    font *f = fm_host_font(D.font_body, D.body_px - 2);
    char t[200];
    snprintf(t, sizeof t, "Reaching %s...", REQ.host);
    int w = text_width(f, t) + 32, h = 34;
    rect r = {(A.W - w) / 2, A.H - h - 24, w, h};
    fill_round(A.frame, r, h / 2.0f, color_alpha(D.surface | 0xff000000u, 0.92f));
    draw_text_centered(A.frame, f, r, t, D.surface_text);
    if (REQ.j && job_poll(REQ.j, &REQ.res)) {
        job_free(REQ.j);
        REQ.j = NULL;
        REQ.done = true;
    }
    A.in.clicked = A.in.pressed = false; /* (the app is waiting: not now) */
}

static fx_val h_http(void *c, const char *method, const char *url, const char *body, const char *ctype) {
    (void)c;
    struct http_job j = {0};
    snprintf(j.method, sizeof j.method, "%s", method);
    snprintf(j.url, sizeof j.url, "%s", url);
    snprintf(j.ctype, sizeof j.ctype, "%s", ctype ? ctype : "");
    j.body = body ? strdup(body) : NULL;
    memset(&REQ, 0, sizeof REQ);
    const char *h = strstr(url, "://");
    snprintf(REQ.host, sizeof REQ.host, "%.*s", (int)strcspn(h ? h + 3 : url, "/:?"), h ? h + 3 : url);
    REQ.j = job_start(http_child, &j);
    main_modal(waiting_draw, &REQ.done);
    fx_val r = response_value(REQ.res);
    free(REQ.res);
    free(j.body);
    LOG("facet: %s %s: %s\n", method, url, fx_truthy(fx_rec_get(r, "ok")) ? "ok" : "failed");
    return r;
}

/* web(url, every): what came last; a fetch in the background when it's due */
static fx_val h_web(void *c, const char *url, double every) {
    struct fxwin *fw = c;
    double now = A.now;
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++)
        if (fw->web[i].url[0] && !strcmp(fw->web[i].url, url))
            slot = i;
    for (int i = 0; i < 8 && slot < 0; i++)
        if (!fw->web[i].url[0]) {
            slot = i;
            snprintf(fw->web[i].url, sizeof fw->web[i].url, "%s", url);
            fw->web[i].value = fx_none();
            fw->web[i].at = -1e18;
        }
    if (slot < 0)
        return fx_none();
    typeof(fw->web[0]) *w = &fw->web[slot];
    w->every = every;
    char *res;
    if (w->fetch && job_poll(w->fetch, &res)) {
        job_free(w->fetch);
        w->fetch = NULL;
        fx_val r = response_value(res);
        free(res);
        if (fx_truthy(fx_rec_get(r, "ok"))) {
            fx_val d = fx_rec_get(r, "data");
            fx_unref(w->value);
            w->value = d.t != V_NONE ? fx_ref(d) : fx_ref(fx_rec_get(r, "text"));
        }
        fx_unref(r);
    }
    if (!w->fetch && now - w->at >= every) {
        static struct http_job j; /* the child has its copy as soon as it starts */
        memset(&j, 0, sizeof j);
        snprintf(j.method, sizeof j.method, "GET");
        snprintf(j.url, sizeof j.url, "%s", url);
        w->fetch = job_start(http_child, &j);
        w->at = now;
    }
    return fx_ref(w->value);
}

/* Sockets: opened on a thread of their own (finding the server and the secure handshake wait on the
 * network), then read every frame without waiting. */
struct fxsock {
    int id;
    char host[256];
    int port;
    bool tls;
    tcp_stream *s;
    volatile int state; /* 0 opening, 1 open (not yet told), 2 open, 3 failed (not yet told), 4 done */
    char err[200];
    pthread_t t;
};

static void *sock_open(void *arg) {
    struct fxsock *k = arg;
    k->s = tcp_open(k->host, k->port, k->tls, k->err, sizeof k->err);
    k->state = k->s ? 1 : 3;
    return NULL;
}

static int h_net_connect(void *c, const char *host, int port, bool tls) {
    struct fxwin *fw = c;
    for (int i = 0; i < 16; i++)
        if (!fw->socks[i]) {
            struct fxsock *k = calloc(1, sizeof *k);
            k->id = i;
            snprintf(k->host, sizeof k->host, "%s", host);
            k->port = port;
            k->tls = tls;
            if (pthread_create(&k->t, NULL, sock_open, k)) {
                free(k);
                return -1;
            }
            fw->socks[i] = k;
            LOG("facet: %s connects to %s:%d%s\n", fw->app->name, host, port, tls ? " (tls)" : "");
            return i;
        }
    return -1;
}

static bool h_net_send(void *c, int id, const void *data, size_t len) {
    struct fxwin *fw = c;
    struct fxsock *k = id >= 0 && id < 16 ? fw->socks[id] : NULL;
    return k && k->state == 2 && tcp_write(k->s, data, len);
}

static void sock_free(struct fxwin *fw, int id) {
    struct fxsock *k = fw->socks[id];
    if (!k)
        return;
    pthread_join(k->t, NULL); /* (an opening one finishes first) */
    tcp_close(k->s);
    free(k);
    fw->socks[id] = NULL;
}

static void h_net_close(void *c, int id) {
    struct fxwin *fw = c;
    if (id >= 0 && id < 16)
        sock_free(fw, id);
}

static int h_net_event(void *c, int *id, char **data, size_t *len) {
    struct fxwin *fw = c;
    *data = NULL, *len = 0;
    for (int i = 0; i < 16; i++) {
        struct fxsock *k = fw->socks[i];
        if (!k)
            continue;
        *id = i;
        if (k->state == 1) {
            k->state = 2;
            return 1;
        }
        if (k->state == 3) {
            k->state = 4;
            *data = strdup(k->err[0] ? k->err : "couldn't connect");
            return 4;
        }
        if (k->state != 2)
            continue;
        char buf[16384];
        int n = tcp_read(k->s, buf, sizeof buf);
        if (n > 0) {
            *data = malloc((size_t)n);
            memcpy(*data, buf, (size_t)n);
            *len = (size_t)n;
            return 2;
        }
        if (n < 0) {
            k->state = 4;
            return 3;
        }
    }
    return 0;
}

/* ---- a question over everything ---- */

static struct {
    const char *question;
    bool prompt, secret, done;
    int answer;
    field input;
} Q;

static font *body_font(float px) { return fm_host_font(D.font_body, px); }

static void field_set(field *f, const char *s) {
    size_t n = strlen(s);
    if (n + 1 > f->cap) {
        f->cap = n + 64;
        f->buf = realloc(f->buf, f->cap);
    }
    memcpy(f->buf, s, n + 1);
    f->len = n;
}

/* this frame's typing into a field; true if its text changed */
static bool field_type(field *f, bool multiline) {
    bool changed = false;
    if (!f->buf)
        field_set(f, "");
    f->ed.multiline = multiline;
    for (int i = 0; i < A.in.nkeys; i++) {
        size_t room = f->len + 8192; /* a paste fits */
        if (f->cap < room) {
            f->cap = room;
            f->buf = realloc(f->buf, f->cap);
        }
        bool ch = false;
        te_key(&f->ed, f->buf, (int)f->cap, A.in.keys[i], (A.in.keymod[i] & KM_CTRL) != 0, (A.in.keymod[i] & KM_SHIFT) != 0, f->secret, A.now, &ch);
        f->len = strlen(f->buf);
        changed |= ch;
    }
    return changed;
}

static void draw_field(rect r, field *f, bool focused, bool multiline, const char *placeholder) {
    color bg = color_mix(D.surface, D.surface_text, 0.07f), ink = D.surface_text;
    float rad = D.radius * 0.5f < 4 ? 4 : D.radius * 0.5f;
    fill_round(A.frame, r, rad, bg);
    stroke_round(A.frame, r, rad, focused ? 2 : 1, focused ? D.accent : color_alpha(ink, 0.18f));
    font *fnt = body_font(D.body_px);
    rect in = {r.x + 10, r.y + 6, r.w - 20, r.h - 12};
    const char *text = f->buf ? f->buf : "";
    f->ed.multiline = multiline;
    int ty = multiline ? in.y : in.y + (in.h - font_line_height(fnt)) / 2;
    if (!*text && placeholder && !focused) {
        canvas_clip(A.frame, r);
        draw_text(A.frame, fnt, in.x, ty, placeholder, color_alpha(ink, 0.45f));
        canvas_unclip(A.frame);
    }
    te_mouse(&f->ed, text, fnt, multiline ? in : r, in.x, &A.in, f->secret, A.now);
    te_draw(A.frame, &f->ed, text, fnt, in, in.x, ty, ink, color_alpha(D.accent, 0.3f), focused, f->secret, A.now);
}

static void question_draw(void) {
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(D.bg, 0.45f));
    font *f = body_font(D.body_px + 1), *bf = body_font(D.body_px);
    int w = 440;
    int th = draw_text_wrapped(NULL, f, (rect){0, 0, w - 56, 400}, Q.question, D.surface_text, 1.35f);
    int h = 28 + th + 24 + (Q.prompt ? 44 + 18 : 0) + 44 + 28;
    rect r = {(A.W - w) / 2, (A.H - h) / 2, w, h};
    shadow_round(A.frame, (rect){r.x, r.y + 14, r.w, r.h}, D.radius + 6, 40, RGBA(0, 0, 0, 110));
    fill_round(A.frame, r, D.radius + 6, D.surface | 0xff000000u);
    draw_text_wrapped(A.frame, f, (rect){r.x + 28, r.y + 28, w - 56, th}, Q.question, D.surface_text, 1.35f);
    int y = r.y + 28 + th + 24;
    if (Q.prompt) {
        Q.input.secret = Q.secret;
        field_type(&Q.input, false);
        draw_field((rect){r.x + 28, y, w - 56, 44}, &Q.input, true, false, NULL);
        y += 44 + 18;
    }
    rect yes = {r.x + w - 28 - 130, y, 130, 44}, no = {yes.x - 12 - 130, y, 130, 44};
    float rad = D.radius * 0.6f + 4;
    if (ui_button(no, Q.prompt ? "Cancel" : "No", bf, color_alpha(D.surface_text, 0.1f), D.surface_text, rad, true))
        Q.answer = 0, Q.done = true;
    if (ui_button(yes, Q.prompt ? "OK" : "Yes", bf, D.accent, readable_on(D.accent), rad, true))
        Q.answer = 1, Q.done = true;
    for (int i = 0; i < A.in.nkeys; i++) {
        if (A.in.keys[i] == K_ENTER)
            Q.answer = 1, Q.done = true;
        else if (A.in.keys[i] == K_ESC)
            Q.answer = 0, Q.done = true;
    }
}

static int h_ask(void *c, const char *q) {
    (void)c;
    memset(&Q, 0, sizeof Q);
    Q.question = q;
    main_modal(question_draw, &Q.done);
    return Q.answer;
}

char *fxwin_secret(const char *q) {
    te_free(&Q.input.ed);
    memset(&Q, 0, sizeof Q);
    Q.question = q;
    Q.prompt = Q.secret = true;
    field_set(&Q.input, "");
    main_modal(question_draw, &Q.done);
    char *r = Q.answer && Q.input.buf && Q.input.buf[0] ? strdup(Q.input.buf) : NULL;
    free(Q.input.buf);
    Q.input.buf = NULL;
    return r;
}

static char *h_prompt(void *c, const char *q, const char *def) {
    (void)c;
    te_free(&Q.input.ed);
    memset(&Q, 0, sizeof Q);
    Q.question = q;
    Q.prompt = true;
    field_set(&Q.input, def ? def : "");
    te_select_all(&Q.input.ed, Q.input.buf); /* what is offered, ready to be typed over */
    main_modal(question_draw, &Q.done);
    char *r = Q.answer ? strdup(Q.input.buf ? Q.input.buf : "") : NULL;
    free(Q.input.buf);
    Q.input.buf = NULL;
    return r;
}

/* ---- the view as a Facet document ---- */

static int field_index(struct fxwin *fw, const char *key) {
    for (int i = 0; i < MAX_FIELDS; i++)
        if (fw->fields[i].used && !strcmp(fw->fields[i].key, key))
            return i;
    for (int i = 0; i < MAX_FIELDS; i++)
        if (!fw->fields[i].used) {
            fw->fields[i].used = true;
            snprintf(fw->fields[i].key, sizeof fw->fields[i].key, "%s", key);
            field_set(&fw->fields[i], "");
            return i;
        }
    return -1;
}

static void rebuild(struct fxwin *fw) {
    fw->view = fx_render(fw->rt);
    free(fw->acts);
    fw->acts = NULL;
    char *markup = fx_markup(fw->view, fw->app, &D, &fw->acts, &fw->nacts);
    fm_doc *doc = fm_parse(markup);
    free(markup);
    if (fw->doc)
        fm_free(fw->doc);
    fw->doc = doc;
    fw->rebuild = false;
}

/* the engine's node for input n (fxin="n") */
static fm_node *input_node(fm_node *n, int k) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        const char *v = fm_attr_get(n, "fxin");
        if (v && atoi(v) == k)
            return n;
        fm_node *f = input_node(n->first, k);
        if (f)
            return f;
    }
    return NULL;
}

static const char *key_name(int k, char *buf) {
    switch (k) {
    case K_ENTER: return "enter";
    case K_ESC: return "esc";
    case K_TAB: return "tab";
    case K_UP: return "up";
    case K_DOWN: return "down";
    case K_LEFT: return "left";
    case K_RIGHT: return "right";
    case K_BACKSPACE: return "backspace";
    case K_DELETE: return "delete";
    case K_HOME: return "home";
    case K_END: return "end";
    case ' ': return "space";
    }
    if (k > ' ' && k < 127) {
        buf[0] = (char)k, buf[1] = 0;
        return buf;
    }
    return NULL;
}

static void run_event(struct fxwin *fw, fx_out *o, const char *ev, fx_val v) {
    fw->busy = true;
    fx_event(fw->rt, o, ev, v);
    fw->busy = false;
    if (fx_dirty(fw->rt))
        fw->rebuild = true;
}

static bool has_handler(const fx_out *o, const char *ev) {
    for (int i = 0; i < o->nh; i++)
        if (!strcmp(o->h[i].event, ev))
            return true;
    return false;
}

/* inputs, fields and checkboxes over their boxes */
static void draw_inputs(struct fxwin *fw, bool focused, bool pointer_here) {
    for (int k = 0; k < fw->nacts; k++) {
        fx_out *o = fw->acts[k];
        bool input = !strcmp(o->tag, "input"), multi = !strcmp(o->tag, "field"), check = !strcmp(o->tag, "checkbox");
        bool widget = fx_is_widget(o->tag);
        if (!input && !multi && !check && !widget)
            continue;
        fm_node *n = input_node(fw->doc->root, k);
        if (!n || n->vis.w <= 0)
            continue;
        rect r = n->vis;
        if (widget) {
            /* a chart rises from nothing; rings and bars ease from where they were */
            bool chart = !strcmp(o->tag, "chart");
            const char *vs = fx_get(o, "value");
            float target = chart ? 1.0f : vs ? strtof(vs, NULL) : 0;
            int slot = -1;
            for (int i = 0; i < 48 && slot < 0; i++)
                if (fw->anim[i].used && o->key && !strcmp(fw->anim[i].key, o->key))
                    slot = i;
            for (int i = 0; i < 48 && slot < 0; i++)
                if (!fw->anim[i].used) {
                    slot = i;
                    fw->anim[i].used = true;
                    snprintf(fw->anim[i].key, sizeof fw->anim[i].key, "%s", o->key ? o->key : "");
                    fw->anim[i].v = 0;
                }
            float shown = target;
            if (slot >= 0) {
                float *v = &fw->anim[slot].v;
                if (A.prof.reduce_motion || D.motion < 0.05f)
                    *v = target;
                else
                    *v += (target - *v) * (0.12f + 0.18f * D.motion);
                if (fabsf(target - *v) < 0.002f * (fabsf(target) + 1))
                    *v = target;
                shown = *v;
                if (shown != target)
                    A.dirty = true; /* keep drawing while it moves */
            }
            fx_widget_draw(A.frame, r, o, &D, chart ? (shown < 1 ? shown : -1) : shown);
            continue;
        }
        bool over = pointer_here && pt_in(r, A.in.mx, A.in.my);
        bool disabled = fx_get(o, "disabled") != NULL;
        if (check) {
            const char *c = fx_get(o, "checked");
            bool on = c && !strcmp(c, "true");
            float rad = 5;
            fill_round(A.frame, r, rad, on ? D.accent : color_mix(D.surface, D.surface_text, 0.07f));
            stroke_round(A.frame, r, rad, 1.5f, on ? D.accent : color_alpha(D.surface_text, over ? 0.6f : 0.3f));
            if (on) {
                color ink = readable_on(D.accent);
                draw_line(A.frame, r.x + r.w * 0.25f, r.y + r.h * 0.52f, r.x + r.w * 0.43f, r.y + r.h * 0.7f, 2.2f, ink);
                draw_line(A.frame, r.x + r.w * 0.43f, r.y + r.h * 0.7f, r.x + r.w * 0.76f, r.y + r.h * 0.3f, 2.2f, ink);
            }
            if (over && A.in.clicked && !disabled) {
                A.in.clicked = false;
                fx_val v = fx_bool(!on);
                fw->busy = true;
                if (fx_bind(fw->rt, o, v))
                    fw->rebuild = true;
                fw->busy = false;
                run_event(fw, o, "change", v);
                return; /* the view changes: the rest draws next frame */
            }
            continue;
        }
        int fi = field_index(fw, o->key ? o->key : "?");
        if (fi < 0)
            continue;
        field *f = &fw->fields[fi];
        bool is_focus = fw->focus[0] && o->key && !strcmp(fw->focus, o->key);
        if (!fw->autofocused && fx_get(o, "autofocus") && o->key) {
            snprintf(fw->focus, sizeof fw->focus, "%s", o->key);
            fw->autofocused = is_focus = true;
        }
        const char *app_value = fx_get(o, "value") ? fx_get(o, "value") : "";
        bool app_changed = !f->app_said || strcmp(f->app_said, app_value);
        if (app_changed) {
            free(f->app_said);
            f->app_said = strdup(app_value);
        }
        if (!is_focus) { /* not being typed in: it shows the app's value */
            field_set(f, app_value);
        } else if (app_changed && f->buf && strcmp(f->buf, app_value)) {
            /* typed in, and the app changed it itself (not the echo of the typing, which is the same text) */
            field_set(f, app_value);
            te_reset(&f->ed, f->buf);
        }
        if (over && A.in.clicked && !disabled) {
            snprintf(fw->focus, sizeof fw->focus, "%s", o->key ? o->key : "");
            is_focus = true;
        }
        draw_field(r, f, is_focus && focused, multi, fx_get(o, "placeholder"));
        if (!is_focus || !focused || disabled)
            continue;
        /* keys first (the app may act on Enter, arrows, Esc), then the typing */
        if (has_handler(o, "key")) {
            for (int i = 0; i < A.in.nkeys; i++) {
                char b[2];
                const char *name = key_name(A.in.keys[i], b);
                if (name)
                    run_event(fw, o, "key", fx_text(name));
            }
        }
        if (field_type(f, multi)) {
            fx_val v = fx_text(f->buf);
            fw->busy = true;
            if (fx_bind(fw->rt, o, v))
                fw->rebuild = true;
            fw->busy = false;
            run_event(fw, o, "input", v);
            fx_unref(v);
        }
        A.in.nkeys = 0; /* the window took them */
    }
}

/* ---- the window ---- */

int fxwin_open(const char *path) {
    /* already open: to the front */
    for (int w = WIN_COUNT; w < WIN_MAX; w++)
        if (A.win[w].open && A.win[w].fx && !strcmp(A.win[w].fx->path, path)) {
            A.win[w].z = ++A.ztop;
            A.win[w].minimized = false;
            return w;
        }
    int w = -1;
    for (int i = WIN_COUNT; i < WIN_MAX; i++)
        if (!A.win[i].open && !A.win[i].view && !A.win[i].fx) {
            w = i;
            break;
        }
    if (w < 0) {
        toast("Too many windows open to start another.");
        return -1;
    }
    struct fxwin *fw = calloc(1, sizeof *fw);
    snprintf(fw->path, sizeof fw->path, "%s", path);
    fw->src = h_read(NULL, path);
    if (!fw->src) {
        toast("Couldn't read %s.", path);
        free(fw);
        return -1;
    }
    fx_errors errs = {0};
    errs.file = path;
    fw->app = fx_parse(fw->src, &errs);
    fx_check(fw->app, &errs);
    if (errs.n) {
        for (int i = 0; i < errs.n; i++)
            LOG("facet: %s:%d: %s%s%s\n", path, errs.e[i].line, errs.e[i].msg, errs.e[i].fix[0] ? " | fix: " : "",
                errs.e[i].fix);
        toast("%s has %d problem%s: %s (line %d)", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, errs.n, errs.n == 1 ? "" : "s", errs.e[0].msg,
              errs.e[0].line);
        free(errs.e);
        fx_free(fw->app);
        free(fw->src);
        free(fw);
        return -1;
    }
    free(errs.e);
    /* nothing runs before they have allowed what its code can do */
    fw->grants = calloc(1, sizeof *fw->grants);
    if (!fw->grants || !appgate_admit(path, fw->app, fw->src, fw->grants)) {
        if (fw->grants)
            toast("%s wasn't allowed to run.", fw->app->name);
        free(fw->grants);
        fx_free(fw->app);
        free(fw->src);
        free(fw);
        return -1;
    }
    fx_host h = {fw,       h_read, h_write, h_remove, h_rename, h_mkdir,  h_files,  h_setting,
                 h_apps,   h_now,  h_open,  h_close,  h_notify, h_ask,    h_prompt, h_error};
    h.grants = fw->grants;
    h.http = h_http, h.web = h_web, h.net_connect = h_net_connect;
    h.net_send = h_net_send, h.net_close = h_net_close, h.net_event = h_net_event;
    fw->rt = fx_start(fw->app, &h);
    fw->rebuild = true;
    fw->pending = -1;
    fw->w = w;
    struct window *win = &A.win[w];
    memset(win, 0, sizeof *win);
    win->fx = fw;
    rect wk = A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H};
    int ww = 1040 < wk.w - 80 ? 1040 : wk.w - 80, wh = 760 < wk.h - 40 ? 760 : wk.h - 40;
    int off = (w - WIN_COUNT) * 28;
    win->r = (rect){wk.x + (wk.w - ww) / 2 + off, wk.y + (wk.h - wh) / 2 + off / 2, ww, wh};
    win->open = true;
    win->opened_at = A.now;
    win->z = ++A.ztop;
    LOG("facet: opened %s as window %d\n", path, w);
    return w;
}

void fxwin_draw(struct fxwin *fw, rect r, bool focused, bool pointer_here) {
    if (fw->busy) { /* waiting on a question it asked: as it was */
        if (fw->doc)
            fm_render_layer(fw->doc, A.frame, 0);
        return;
    }
    fw->busy = true;
    fx_tick(fw->rt);
    fw->busy = false;
    if (fx_dirty(fw->rt))
        fw->rebuild = true;
    if (fw->rebuild || !fw->doc)
        rebuild(fw);
    static input_state none = {.mx = -10000, .my = -10000};
    fw->doc->in = pointer_here ? &A.in : &none;
    fm_layout(fw->doc, r);
    fm_animate(fw->doc, 16);
    fw->pending = -1;
    if (pointer_here)
        fm_input(fw->doc, 1);
    if (pointer_here && A.in.right_pressed && fw->doc->hovered) { /* what is under the pointer in it (desktop.c) */
        extern void desktop_context_menu_in_app(fm_node *n);
        desktop_context_menu_in_app(fw->doc->hovered);
    }
    fm_render_layer(fw->doc, A.frame, 0);
    draw_inputs(fw, focused, pointer_here);
    if (fw->pending >= 0 && fw->pending < fw->nacts) {
        int k = fw->pending;
        fw->pending = -1;
        run_event(fw, fw->acts[k], "click", fx_none());
    }
    const char *fault = fx_fault(fw->rt);
    if (fault) { /* a fault while running: said at the bottom of the window, the app carries on */
        font *f = body_font(D.body_px - 3);
        rect bar = {r.x + 8, r.y + r.h - 34, r.w - 16, 26};
        fill_round(A.frame, bar, 8, color_alpha(RGBA(200, 60, 60, 255), 0.85f));
        canvas_clip(A.frame, bar);
        draw_text(A.frame, f, bar.x + 10, bar.y + (bar.h - font_line_height(f)) / 2, fault, RGBA(255, 255, 255, 255));
        canvas_unclip(A.frame);
    }
}

bool fxwin_action(fm_doc *d, const char *a) {
    for (int w = WIN_COUNT; w < WIN_MAX; w++) {
        struct fxwin *fw = A.win[w].fx;
        if (fw && fw->doc == d) {
            fw->pending = atoi(a);
            return true;
        }
    }
    return false;
}

const char *fxwin_title(const struct fxwin *fw) { return fw && fw->app ? fw->app->name : "App"; }

void fxwin_close_path(const char *path) {
    for (int w = WIN_COUNT; w < WIN_MAX; w++)
        if (A.win[w].fx && !strcmp(A.win[w].fx->path, path))
            fxwin_close(A.win[w].fx);
}

double fxwin_wake_in(void) {
    double best = 1e18;
    for (int w = WIN_COUNT; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized && A.win[w].fx && A.win[w].fx->rt) {
            double d = fx_wake_in(A.win[w].fx->rt);
            if (d < best)
                best = d;
        }
    return best;
}

void fxwin_reap(void) {
    for (int w = WIN_COUNT; w < WIN_MAX; w++)
        if (A.win[w].fx && !A.win[w].open && !A.win[w].fx->busy)
            fxwin_close(A.win[w].fx);
}

void fxwin_close(struct fxwin *fw) {
    if (!fw)
        return;
    if (fw->busy) { /* it is running: the window goes now, the app once it is done (fxwin_reap) */
        if (fw->w >= 0)
            A.win[fw->w].open = false;
        return;
    }
    if (fw->w >= 0 && A.win[fw->w].fx == fw)
        A.win[fw->w].fx = NULL, A.win[fw->w].open = false;
    fx_stop(fw->rt); /* (closes its sockets through h_net_close) */
    for (int i = 0; i < 16; i++)
        sock_free(fw, i);
    for (int i = 0; i < 8; i++) {
        if (fw->web[i].fetch)
            job_free(fw->web[i].fetch);
        if (fw->web[i].url[0])
            fx_unref(fw->web[i].value);
    }
    fx_free(fw->app);
    if (fw->doc)
        fm_free(fw->doc);
    for (int i = 0; i < MAX_FIELDS; i++) {
        free(fw->fields[i].buf);
        free(fw->fields[i].app_said);
    }
    free(fw->acts);
    free(fw->src);
    free(fw->grants);
    free(fw);
}

int fxwin_list(char names[][64], char paths[][300], int max) {
    int n = 0;
    for (int pass = 0; pass < 2; pass++) { /* the system's apps, then theirs */
    char dir[300];
    snprintf(dir, sizeof dir, "%s", pass ? home_path("/Apps") : "/usr/share/os/apps");
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l < 7 || strcmp(e->d_name + l - 6, ".facet") || e->d_name[0] == '.')
            continue;
        snprintf(names[n], 64, "%.*s", (int)(l - 6) < 63 ? (int)(l - 6) : 63, e->d_name);
        snprintf(paths[n], 300, "%s/%s", dir, e->d_name);
        n++;
    }
    if (d)
        closedir(d);
    }
    return n;
}
