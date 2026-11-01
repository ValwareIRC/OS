/* Running Facet 2 (docs/facet2.md): values, statements, the standard
 * library, derived values recomputed when what they read changes, live
 * data that follows state, timers, components with their own state, and
 * the view drawn as a tree of elements carrying their handlers.
 *
 * A fault while running (an index past the end, a field of none, text +
 * number) stops what was running, is reported through the host, and leaves
 * the app as it was: it never takes the desktop down. */
#define _GNU_SOURCE
#include "fx_rt.h"
#include "../../lib/json.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- the running app ---- */

struct fx_scope {
    const char *name;
    fx_val v;
    fx_scope *up;
};

typedef struct {
    char name[64];
    fx_inst *inst;
    char fn[64];
    double due, every;
    bool live;
} timer;

struct fx_inst {
    const fx_app *app;
    fx_rt *rt;
    fx_val *data, *state, *derived, *params;
    uint64_t *dgen, *dpv;
    bool *dbusy;
    char **data_sig;
    double *data_at;
    uint64_t pv; /* bumps when its parameters change */
    /* a component's: where it sits in its parent's view */
    char *path;
    bool seen;
    fx_inst *owner;
    const fx_node *el;
    fx_scope *el_scope;
    fx_inst *next;
};

struct fx_rt {
    const fx_app *app;
    fx_host host;
    fx_inst *root, *comps;
    uint64_t gen;
    bool dirty, refetch;
    char fault[320];
    timer timers[32];
    fx_out *out;
    jmp_buf *jb;
    int depth;
    struct sock { /* its sockets (net.connect) */
        int id;
        fx_inst *inst;
        char fn[64];
        bool bytes, live;
        char *buf; /* a line not yet ended */
        size_t len;
    } socks[16];
    int net_ok; /* 0 not asked yet, 1 allowed, -1 refused */
};

typedef struct {
    const char *names[64];
    fx_val vals[64];
    int n;
    bool returned;
    fx_val ret;
} frame;

typedef struct {
    fx_inst *in;
    fx_scope *sc;
    frame *fr;
    bool in_fn;
} ev;

static fx_val eval(ev *e, const fx_expr *x);

static void fault(fx_rt *rt, int line, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));
static void fault(fx_rt *rt, int line, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(rt->fault, sizeof rt->fault, "%s:%d: %s", rt->app->name, line, msg);
    if (rt->host.error)
        rt->host.error(rt->host.ctx, rt->fault);
    longjmp(*rt->jb, 1);
}

/* ---- names ---- */

static int index_of(const fx_decl *d, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(d[i].name, name))
            return i;
    return -1;
}

static fx_val derived_value(fx_inst *in, int i);

static bool lookup(ev *e, const char *name, fx_val *out) {
    for (fx_scope *s = e->sc; s; s = s->up)
        if (!strcmp(s->name, name)) {
            *out = fx_ref(s->v);
            return true;
        }
    if (e->fr)
        for (int i = e->fr->n - 1; i >= 0; i--)
            if (!strcmp(e->fr->names[i], name)) {
                *out = fx_ref(e->fr->vals[i]);
                return true;
            }
    fx_inst *in = e->in;
    for (int i = 0; i < in->app->nparams; i++)
        if (!strcmp(in->app->params[i], name)) {
            *out = fx_ref(in->params[i]);
            return true;
        }
    int i;
    if ((i = index_of(in->app->data, in->app->ndata, name)) >= 0) {
        *out = fx_ref(in->data[i]);
        return true;
    }
    if ((i = index_of(in->app->state, in->app->nstate, name)) >= 0) {
        *out = fx_ref(in->state[i]);
        return true;
    }
    if ((i = index_of(in->app->derived, in->app->nderived, name)) >= 0) {
        *out = derived_value(in, i);
        return true;
    }
    return false;
}

static fx_val derived_value(fx_inst *in, int i) {
    fx_rt *rt = in->rt;
    if (in->dgen[i] == rt->gen && in->dpv[i] == in->pv)
        return fx_ref(in->derived[i]);
    if (in->dbusy[i])
        fault(rt, in->app->derived[i].line, "%s depends on itself", in->app->derived[i].name);
    in->dbusy[i] = true;
    ev e = {in, NULL, NULL, false};
    fx_val v = eval(&e, in->app->derived[i].e);
    in->dbusy[i] = false;
    fx_unref(in->derived[i]);
    in->derived[i] = v;
    in->dgen[i] = rt->gen;
    in->dpv[i] = in->pv;
    return fx_ref(v);
}

static void changed(fx_rt *rt) {
    rt->gen++;
    rt->dirty = true;
}

/* ---- helpers ---- */

static const char *need_text(fx_rt *rt, fx_val v, int line, const char *what) {
    if (v.t != V_TEXT)
        fault(rt, line, "%s needs a text, got %s", what, fx_type_name(v));
    return v.o->s;
}
static double need_num(fx_rt *rt, fx_val v, int line, const char *what) {
    if (v.t != V_NUM)
        fault(rt, line, "%s needs a number, got %s", what, fx_type_name(v));
    return v.n;
}
static fx_obj *need_list(fx_rt *rt, fx_val v, int line, const char *what) {
    if (v.t != V_LIST)
        fault(rt, line, "%s needs a list, got %s", what, fx_type_name(v));
    return v.o;
}

static char *home_path(const char *p) {
    if (p[0] == '~' && (p[1] == '/' || !p[1])) {
        const char *home = getenv("HOME");
        char *r = malloc(strlen(home ? home : "/root") + strlen(p) + 1);
        sprintf(r, "%s%s", home ? home : "/root", p + 1);
        return r;
    }
    return strdup(p);
}

/* a lambda called with one or two values */
static fx_val call_lambda(ev *e, const fx_expr *lam, fx_val x, int i) {
    fx_scope s1 = {lam->names[0], x, e->sc};
    fx_scope s2 = {lam->n > 1 ? lam->names[1] : "", fx_num(i), &s1};
    ev inner = *e;
    inner.sc = lam->n > 1 ? &s2 : &s1;
    return eval(&inner, lam->a);
}

static int cmp_vals(fx_rt *rt, fx_val a, fx_val b, int line) {
    if (a.t == V_NUM && b.t == V_NUM)
        return a.n < b.n ? -1 : a.n > b.n;
    if (a.t == V_TEXT && b.t == V_TEXT)
        return strcmp(a.o->s, b.o->s);
    fault(rt, line, "can't order %s against %s", fx_type_name(a), fx_type_name(b));
}

/* ---- JSON ---- */

static fx_val from_json(const json *j) {
    if (!j)
        return fx_none();
    switch (j->type) {
    case JSON_NULL: return fx_none();
    case JSON_BOOL: return fx_bool(j->boolean);
    case JSON_NUMBER: return fx_num(j->num);
    case JSON_STRING: return fx_text(j->str ? j->str : "");
    case JSON_ARRAY: {
        fx_val l = fx_list();
        for (const json *c = j->child; c; c = c->next)
            fx_list_push(l, from_json(c));
        return l;
    }
    case JSON_OBJECT: {
        fx_val r = fx_rec();
        for (const json *c = j->child; c; c = c->next)
            fx_rec_set(r, c->key ? c->key : "", from_json(c));
        return r;
    }
    }
    return fx_none();
}

fx_val fx_parse_json(const char *text, size_t len) {
    if (!text)
        return fx_none();
    json *j = json_parse(text, len);
    fx_val v = from_json(j);
    json_free(j);
    return v;
}

static void to_json(sbuf *b, fx_val v) {
    switch (v.t) {
    case V_NONE: sb_add(b, "null"); break;
    case V_BOOL: sb_add(b, v.b ? "true" : "false"); break;
    case V_NUM: {
        char *s = fx_show(v);
        sb_add(b, s);
        free(s);
        break;
    }
    case V_TEXT: sb_json_str(b, v.o->s); break;
    case V_LIST:
        sb_add(b, "[");
        for (int i = 0; i < v.o->n; i++) {
            if (i)
                sb_add(b, ",");
            to_json(b, v.o->items[i]);
        }
        sb_add(b, "]");
        break;
    case V_REC:
        sb_add(b, "{");
        for (int i = 0; i < v.o->n; i++) {
            if (i)
                sb_add(b, ",");
            sb_json_str(b, v.o->names[i]);
            sb_add(b, ":");
            to_json(b, v.o->items[i]);
        }
        sb_add(b, "}");
        break;
    default: sb_add(b, "null"); break;
    }
}

char *fx_json_text(fx_val v) {
    sbuf b;
    sb_init(&b);
    to_json(&b, v);
    return b.p ? b.p : strdup("null");
}

/* ---- the standard library ---- */

static fx_val text_slice(const char *s, size_t len, double a, double b) {
    size_t st = a < 0 ? 0 : a > len ? len : (size_t)a, en = b < 0 ? 0 : b > len ? len : (size_t)b;
    return st >= en ? fx_text("") : fx_textn(s + st, en - st);
}

static fx_val stdlib_call(ev *e, const fx_expr *x) {
    fx_rt *rt = e->in->rt;
    const char *f = x->s;
    int line = x->line;
    /* the lambda takers evaluate their second argument themselves */
    if (!strcmp(f, "map") || !strcmp(f, "filter") || !strcmp(f, "find") || !strcmp(f, "sort")) {
        fx_val list = eval(e, x->items[0]);
        fx_obj *l = need_list(rt, list, line, f);
        const fx_expr *lam = x->items[1];
        fx_val out = !strcmp(f, "find") ? fx_none() : fx_list();
        if (!strcmp(f, "sort")) {
            int n = l->n;
            fx_val *keys = malloc(sizeof *keys * (size_t)(n ? n : 1));
            int *idx = malloc(sizeof *idx * (size_t)(n ? n : 1));
            for (int i = 0; i < n; i++)
                keys[i] = call_lambda(e, lam, l->items[i], i), idx[i] = i;
            for (int i = 1; i < n; i++) /* stable insertion sort */
                for (int j = i; j > 0 && cmp_vals(rt, keys[idx[j - 1]], keys[idx[j]], line) > 0; j--) {
                    int t = idx[j];
                    idx[j] = idx[j - 1];
                    idx[j - 1] = t;
                }
            for (int i = 0; i < n; i++)
                fx_list_push(out, fx_ref(l->items[idx[i]]));
            for (int i = 0; i < n; i++)
                fx_unref(keys[i]);
            free(keys);
            free(idx);
        } else {
            for (int i = 0; i < l->n; i++) {
                fx_val r = call_lambda(e, lam, l->items[i], i);
                if (!strcmp(f, "map")) {
                    fx_list_push(out, r);
                    continue;
                }
                bool yes = fx_truthy(r);
                fx_unref(r);
                if (yes && !strcmp(f, "filter"))
                    fx_list_push(out, fx_ref(l->items[i]));
                if (yes && !strcmp(f, "find")) {
                    out = fx_ref(l->items[i]);
                    break;
                }
            }
        }
        fx_unref(list);
        return out;
    }
    fx_val a[3] = {fx_none(), fx_none(), fx_none()};
    for (int i = 0; i < x->n && i < 3; i++)
        a[i] = eval(e, x->items[i]);
    fx_val r = fx_none();
    if (!strcmp(f, "len")) {
        if (a[0].t == V_TEXT) r = fx_num((double)a[0].o->len);
        else if (a[0].t == V_LIST || a[0].t == V_REC) r = fx_num(a[0].o->n);
        else fault(rt, line, "len needs a text or a list, got %s", fx_type_name(a[0]));
    } else if (!strcmp(f, "lower") || !strcmp(f, "upper")) {
        const char *s = need_text(rt, a[0], line, f);
        r = fx_text(s);
        for (char *p = r.o->s; *p; p++)
            *p = f[0] == 'l' ? (char)tolower((unsigned char)*p) : (char)toupper((unsigned char)*p);
    } else if (!strcmp(f, "trim")) {
        const char *s = need_text(rt, a[0], line, f), *t = s + strlen(s);
        while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
            s++;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\n' || t[-1] == '\r'))
            t--;
        r = fx_textn(s, (size_t)(t - s));
    } else if (!strcmp(f, "split")) {
        const char *s = need_text(rt, a[0], line, f), *sep = need_text(rt, a[1], line, f);
        r = fx_list();
        size_t sl = strlen(sep);
        if (!sl) {
            for (const char *p = s; *p; p++)
                fx_list_push(r, fx_textn(p, 1));
        } else {
            for (const char *p = s;;) {
                const char *q = strstr(p, sep);
                if (!q) {
                    fx_list_push(r, fx_text(p));
                    break;
                }
                fx_list_push(r, fx_textn(p, (size_t)(q - p)));
                p = q + sl;
            }
        }
    } else if (!strcmp(f, "join")) {
        fx_obj *l = need_list(rt, a[0], line, f);
        const char *sep = need_text(rt, a[1], line, f);
        size_t cap = 64, len = 0;
        char *buf = malloc(cap);
        for (int i = 0; i < l->n; i++) {
            char *p = fx_show(l->items[i]);
            size_t pl = strlen(p), sl = i ? strlen(sep) : 0;
            while (len + pl + sl + 1 > cap)
                buf = realloc(buf, cap *= 2);
            memcpy(buf + len, sep, sl);
            len += sl;
            memcpy(buf + len, p, pl);
            len += pl;
            free(p);
        }
        r = fx_textn(buf, len);
        free(buf);
    } else if (!strcmp(f, "text")) {
        char *s = fx_show(a[0]);
        r = fx_text(s);
        free(s);
    } else if (!strcmp(f, "number")) {
        if (a[0].t == V_NUM) r = fx_ref(a[0]);
        else {
            const char *s = need_text(rt, a[0], line, f);
            char *end;
            double v = strtod(s, &end);
            r = end != s && !*end ? fx_num(v) : fx_none();
        }
    } else if (!strcmp(f, "pad")) {
        const char *s = need_text(rt, a[0], line, f), *ch = need_text(rt, a[2], line, f);
        int w = (int)need_num(rt, a[1], line, f), sl = (int)strlen(s);
        if (sl >= w || !*ch)
            r = fx_text(s);
        else {
            char *buf = malloc((size_t)w + 1);
            memset(buf, ch[0], (size_t)(w - sl));
            memcpy(buf + w - sl, s, (size_t)sl + 1);
            r = fx_text(buf);
            free(buf);
        }
    } else if (!strcmp(f, "round")) {
        r = fx_num(round(need_num(rt, a[0], line, f))); /* C round: halves away from zero */
    } else if (!strcmp(f, "floor")) {
        r = fx_num(floor(need_num(rt, a[0], line, f)));
    } else if (!strcmp(f, "ceil")) {
        r = fx_num(ceil(need_num(rt, a[0], line, f)));
    } else if (!strcmp(f, "min") || !strcmp(f, "max")) {
        int c = cmp_vals(rt, a[0], a[1], line);
        r = fx_ref((f[1] == 'i') == (c <= 0) ? a[0] : a[1]);
    } else if (!strcmp(f, "slice")) {
        double st = need_num(rt, a[1], line, f);
        bool has_end = x->n > 2;
        double en = has_end ? need_num(rt, a[2], line, f) : 1e18;
        if (st < 0 || en < 0)
            fault(rt, line, "slice doesn't count from the end: write len(x) - %g", -(st < 0 ? st : en));
        if (a[0].t == V_TEXT)
            r = text_slice(a[0].o->s, a[0].o->len, st, en);
        else {
            fx_obj *l = need_list(rt, a[0], line, f);
            r = fx_list();
            for (int i = (int)(st > l->n ? l->n : st); i < l->n && i < en; i++)
                fx_list_push(r, fx_ref(l->items[i]));
        }
    } else if (!strcmp(f, "first") || !strcmp(f, "last")) {
        fx_obj *l = need_list(rt, a[0], line, f);
        r = l->n ? fx_ref(l->items[f[0] == 'f' ? 0 : l->n - 1]) : fx_none();
    } else if (!strcmp(f, "contains")) {
        if (a[0].t == V_TEXT)
            r = fx_bool(strstr(a[0].o->s, need_text(rt, a[1], line, f)) != NULL);
        else {
            fx_obj *l = need_list(rt, a[0], line, f);
            bool hit = false;
            for (int i = 0; i < l->n && !hit; i++)
                hit = fx_equal(l->items[i], a[1]);
            r = fx_bool(hit);
        }
    } else if (!strcmp(f, "starts") || !strcmp(f, "ends")) {
        const char *s = need_text(rt, a[0], line, f), *p = need_text(rt, a[1], line, f);
        size_t sl = strlen(s), pl = strlen(p);
        r = fx_bool(pl <= sl && !memcmp(f[0] == 's' ? s : s + sl - pl, p, pl));
    } else if (!strcmp(f, "date")) {
        double ms = need_num(rt, a[0], line, f);
        const char *fmt = need_text(rt, a[1], line, f);
        time_t t = (time_t)(ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        char buf[128];
        strftime(buf, sizeof buf, fmt, &tm);
        r = fx_text(buf);
    } else if (!strcmp(f, "json")) {
        char *s = fx_json_text(a[0]);
        r = fx_text(s);
        free(s);
    } else if (!strcmp(f, "parse")) {
        const char *s = need_text(rt, a[0], line, f);
        r = fx_parse_json(s, strlen(s));
    } else if (!strcmp(f, "base") || !strcmp(f, "dirname")) {
        const char *s = need_text(rt, a[0], line, f);
        size_t l = strlen(s);
        while (l > 1 && s[l - 1] == '/')
            l--;
        const char *slash = NULL;
        for (size_t i = 0; i < l; i++)
            if (s[i] == '/')
                slash = s + i;
        if (f[0] == 'b')
            r = slash ? fx_textn(slash + 1, l - (size_t)(slash + 1 - s)) : fx_textn(s, l);
        else
            r = !slash ? fx_text(".") : slash == s ? fx_text("/") : fx_textn(s, (size_t)(slash - s));
    }
    for (int i = 0; i < 3; i++)
        fx_unref(a[i]);
    return r;
}

/* ---- effects (fns only) ---- */

static fx_val call_fn(fx_inst *in, const fx_fn *fn, fx_val *args, int nargs, int line);
static const fx_fn *find_fn(const fx_app *a, const char *name) {
    for (int i = 0; i < a->nfns; i++)
        if (!strcmp(a->fns[i].name, name))
            return &a->fns[i];
    return NULL;
}

static void schedule(fx_inst *in, const char *name, const char *fn, double ms, bool every) {
    fx_rt *rt = in->rt;
    double now = rt->host.now_ms ? rt->host.now_ms(rt->host.ctx) : 0;
    timer *slot = NULL;
    for (int i = 0; i < 32; i++)
        if (rt->timers[i].live && rt->timers[i].inst == in && !strcmp(rt->timers[i].name, name))
            slot = &rt->timers[i]; /* the same name again: it starts over */
    for (int i = 0; i < 32 && !slot; i++)
        if (!rt->timers[i].live)
            slot = &rt->timers[i];
    if (!slot)
        return;
    snprintf(slot->name, sizeof slot->name, "%s", name);
    snprintf(slot->fn, sizeof slot->fn, "%s", fn);
    slot->inst = in;
    slot->due = now + ms;
    slot->every = every ? (ms < 10 ? 10 : ms) : 0;
    slot->live = true;
}

/* The app may use the internet: asked once (the host remembers the answer). */
static bool net_allowed(fx_rt *rt) {
    if (!rt->net_ok)
        rt->net_ok = rt->host.net_allowed && rt->host.net_allowed(rt->host.ctx) ? 1 : -1;
    return rt->net_ok > 0;
}

static fx_val net_refused(void) {
    fx_val r = fx_rec();
    fx_rec_set(r, "ok", fx_bool(false));
    fx_rec_set(r, "status", fx_num(0));
    fx_rec_set(r, "data", fx_none());
    fx_rec_set(r, "text", fx_text(""));
    fx_rec_set(r, "error", fx_text("this app isn't allowed to use the internet"));
    return r;
}

static fx_val net_call(ev *e, const fx_expr *x) {
    fx_inst *in = e->in;
    fx_rt *rt = in->rt;
    const fx_host *h = &rt->host;
    const char *f = x->s;
    int line = x->line;
    if (!strcmp(f, "net.connect")) {
        fx_val host = eval(e, x->items[0]), port = eval(e, x->items[1]), mode = eval(e, x->items[2]);
        const fx_expr *fnx = x->items[3];
        const char *fn = fnx && fnx->k == E_NAME ? fnx->s : "";
        const char *hs = need_text(rt, host, line, f), *ms = need_text(rt, mode, line, f);
        int p = (int)need_num(rt, port, line, f), id = -1;
        if (!find_fn(in->app, fn))
            fault(rt, line, "net.connect: no fn \"%s\" to hear it", fn);
        struct sock *slot = NULL;
        for (int i = 0; i < 16 && !slot; i++)
            if (!rt->socks[i].live)
                slot = &rt->socks[i];
        if (slot && net_allowed(rt) && h->net_connect)
            id = h->net_connect(h->ctx, hs, p, strstr(ms, "tls") != NULL);
        if (id >= 0) {
            memset(slot, 0, sizeof *slot);
            slot->id = id, slot->inst = in, slot->live = true, slot->bytes = strstr(ms, "bytes") != NULL;
            snprintf(slot->fn, sizeof slot->fn, "%s", fn);
        }
        fx_unref(host), fx_unref(port), fx_unref(mode);
        return fx_num(id);
    }
    fx_val a[3] = {fx_none(), fx_none(), fx_none()};
    for (int i = 0; i < x->n && i < 3; i++)
        a[i] = eval(e, x->items[i]);
    fx_val r = fx_none();
    if (!strcmp(f, "net.get") || !strcmp(f, "net.post")) {
        const char *url = need_text(rt, a[0], line, f);
        if (!net_allowed(rt) || !h->http) {
            r = net_refused();
        } else if (f[4] == 'g') {
            r = h->http(h->ctx, "GET", url, NULL, NULL);
        } else {
            char *body = a[1].t == V_TEXT ? strdup(a[1].o->s) : fx_json_text(a[1]);
            const char *ct = x->n > 2 ? need_text(rt, a[2], line, f)
                             : a[1].t == V_TEXT ? "text/plain; charset=utf-8" : "application/json";
            r = h->http(h->ctx, "POST", url, body, ct);
            free(body);
        }
    } else if (!strcmp(f, "net.send")) {
        int id = (int)need_num(rt, a[0], line, f);
        bool ok = false;
        if (a[1].t == V_TEXT) {
            ok = h->net_send && h->net_send(h->ctx, id, a[1].o->s, a[1].o->len);
        } else if (a[1].t == V_LIST) { /* bytes: numbers 0..255 */
            unsigned char *b = malloc((size_t)a[1].o->n + 1);
            for (int i = 0; i < a[1].o->n; i++) {
                fx_val v = a[1].o->items[i];
                if (v.t != V_NUM || v.n < 0 || v.n > 255)
                    fault(rt, line, "net.send: a list to send holds numbers 0 to 255 (bytes)");
                b[i] = (unsigned char)v.n;
            }
            ok = h->net_send && h->net_send(h->ctx, id, b, (size_t)a[1].o->n);
            free(b);
        } else {
            fault(rt, line, "net.send sends a text or a list of bytes, not %s", fx_type_name(a[1]));
        }
        r = fx_bool(ok);
    } else if (!strcmp(f, "net.close")) {
        int id = (int)need_num(rt, a[0], line, f);
        for (int i = 0; i < 16; i++)
            if (rt->socks[i].live && rt->socks[i].id == id) {
                rt->socks[i].live = false;
                free(rt->socks[i].buf);
                rt->socks[i].buf = NULL;
            }
        if (h->net_close)
            h->net_close(h->ctx, id);
    }
    for (int i = 0; i < 3; i++)
        fx_unref(a[i]);
    return r;
}

static fx_val effect_call(ev *e, const fx_expr *x) {
    fx_inst *in = e->in;
    fx_rt *rt = in->rt;
    const fx_host *h = &rt->host;
    const char *f = x->s;
    int line = x->line;
    if (!strncmp(f, "net.", 4))
        return net_call(e, x);
    if (!strcmp(f, "after") || !strcmp(f, "every")) {
        fx_val nm = eval(e, x->items[0]), ms = eval(e, x->items[1]);
        const fx_expr *fnx = x->items[2];
        const char *fn = fnx && fnx->k == E_NAME ? fnx->s : "";
        if (!find_fn(in->app, fn))
            fault(rt, line, "%s: no fn \"%s\"", f, fn);
        schedule(in, need_text(rt, nm, line, f), fn, need_num(rt, ms, line, f), f[0] == 'e');
        fx_unref(nm);
        return fx_none();
    }
    fx_val a[2] = {fx_none(), fx_none()};
    for (int i = 0; i < x->n && i < 2; i++)
        a[i] = eval(e, x->items[i]);
    fx_val r = fx_none();
    char *p0 = NULL, *p1 = NULL;
    if (!strncmp(f, "fs.", 3)) {
        p0 = home_path(need_text(rt, a[0], line, f));
        if (!strcmp(f, "fs.rename"))
            p1 = home_path(need_text(rt, a[1], line, f));
    }
    if (!strcmp(f, "cancel")) {
        const char *nm = need_text(rt, a[0], line, f);
        for (int i = 0; i < 32; i++)
            if (rt->timers[i].live && rt->timers[i].inst == in && !strcmp(rt->timers[i].name, nm))
                rt->timers[i].live = false;
    } else if (!strcmp(f, "fs.read")) {
        char *s = h->read ? h->read(h->ctx, p0) : NULL;
        r = fx_text(s ? s : "");
        free(s);
    } else if (!strcmp(f, "fs.write")) {
        if (!h->write || !h->write(h->ctx, p0, need_text(rt, a[1], line, f)))
            fault(rt, line, "couldn't write %s", p0);
        rt->refetch = true;
    } else if (!strcmp(f, "fs.remove")) {
        if (!h->remove || !h->remove(h->ctx, p0))
            fault(rt, line, "couldn't remove %s", p0);
        rt->refetch = true;
    } else if (!strcmp(f, "fs.rename")) {
        if (!h->rename || !h->rename(h->ctx, p0, p1))
            fault(rt, line, "couldn't rename %s", p0);
        rt->refetch = true;
    } else if (!strcmp(f, "fs.mkdir")) {
        if (!h->mkdir || !h->mkdir(h->ctx, p0))
            fault(rt, line, "couldn't make the folder %s", p0);
        rt->refetch = true;
    } else if (!strcmp(f, "app.open")) {
        const char *id = need_text(rt, a[0], line, f);
        char *path = x->n > 1 ? home_path(need_text(rt, a[1], line, f)) : NULL;
        r = fx_bool(h->open_app && h->open_app(h->ctx, id, path));
        free(path);
    } else if (!strcmp(f, "app.close")) {
        if (h->close_app)
            h->close_app(h->ctx);
    } else if (!strcmp(f, "app.notify")) {
        if (h->notify)
            h->notify(h->ctx, need_text(rt, a[0], line, f));
    } else if (!strcmp(f, "ask")) {
        r = fx_bool(h->ask && h->ask(h->ctx, need_text(rt, a[0], line, f)) == 1);
    } else if (!strcmp(f, "prompt")) {
        char *s = h->prompt ? h->prompt(h->ctx, need_text(rt, a[0], line, f), need_text(rt, a[1], line, f)) : NULL;
        r = s ? fx_text(s) : fx_none();
        free(s);
    }
    free(p0);
    free(p1);
    fx_unref(a[0]);
    fx_unref(a[1]);
    return r;
}

/* ---- expressions ---- */

static fx_val binary(ev *e, const fx_expr *x) {
    fx_rt *rt = e->in->rt;
    const char *op = x->s;
    if (!strcmp(op, "and") || !strcmp(op, "or")) {
        fx_val a = eval(e, x->a);
        bool t = fx_truthy(a);
        fx_unref(a);
        if (op[0] == 'a' ? !t : t)
            return fx_bool(t);
        fx_val b = eval(e, x->b);
        bool u = fx_truthy(b);
        fx_unref(b);
        return fx_bool(u);
    }
    fx_val a = eval(e, x->a), b = eval(e, x->b), r = fx_none();
    if (!strcmp(op, "==") || !strcmp(op, "!=")) {
        bool eq = fx_equal(a, b);
        r = fx_bool(op[0] == '=' ? eq : !eq);
    } else if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) {
        int c = cmp_vals(rt, a, b, x->line);
        r = fx_bool(op[0] == '<' ? (op[1] ? c <= 0 : c < 0) : (op[1] ? c >= 0 : c > 0));
    } else if (!strcmp(op, "+")) {
        if (a.t == V_NUM && b.t == V_NUM)
            r = fx_num(a.n + b.n);
        else if (a.t == V_TEXT && b.t == V_TEXT) {
            char *s = malloc(a.o->len + b.o->len + 1);
            memcpy(s, a.o->s, a.o->len);
            memcpy(s + a.o->len, b.o->s, b.o->len + 1);
            r = fx_textn(s, a.o->len + b.o->len);
            free(s);
        } else if (a.t == V_LIST && b.t == V_LIST) {
            r = fx_list();
            for (int i = 0; i < a.o->n; i++)
                fx_list_push(r, fx_ref(a.o->items[i]));
            for (int i = 0; i < b.o->n; i++)
                fx_list_push(r, fx_ref(b.o->items[i]));
        } else if ((a.t == V_TEXT && b.t == V_NUM) || (a.t == V_NUM && b.t == V_TEXT)) {
            fault(rt, x->line, "text + number: write text(n) to join them, or \"...{n}...\"");
        } else {
            fault(rt, x->line, "can't add %s and %s", fx_type_name(a), fx_type_name(b));
        }
    } else {
        double p = need_num(rt, a, x->line, op), q = need_num(rt, b, x->line, op);
        if ((op[0] == '/' || op[0] == '%') && q == 0)
            fault(rt, x->line, "dividing by zero");
        r = fx_num(op[0] == '-' ? p - q : op[0] == '*' ? p * q : op[0] == '/' ? p / q : fmod(p, q));
    }
    fx_unref(a);
    fx_unref(b);
    return r;
}

static fx_val eval(ev *e, const fx_expr *x) {
    fx_rt *rt = e->in->rt;
    if (!x)
        return fx_none();
    switch (x->k) {
    case E_NONE: return fx_none();
    case E_BOOL: return fx_bool(x->num != 0);
    case E_NUM: return fx_num(x->num);
    case E_TEXT: return fx_text(x->s);
    case E_TEMPLATE: {
        size_t cap = 64, len = 0;
        char *buf = malloc(cap);
        for (int i = 0; i < x->n; i++) {
            char *p;
            if (x->items[i]->k == E_TEXT)
                p = strdup(x->items[i]->s);
            else {
                fx_val v = eval(e, x->items[i]);
                p = fx_show(v);
                fx_unref(v);
            }
            size_t pl = strlen(p);
            while (len + pl + 1 > cap)
                buf = realloc(buf, cap *= 2);
            memcpy(buf + len, p, pl);
            len += pl;
            free(p);
        }
        fx_val r = fx_textn(buf, len);
        free(buf);
        return r;
    }
    case E_NAME: {
        fx_val v;
        if (!lookup(e, x->s, &v))
            fault(rt, x->line, "unknown name \"%s\"", x->s);
        return v;
    }
    case E_MEMBER: {
        fx_val o = eval(e, x->a);
        if (o.t == V_NONE)
            fault(rt, x->line, "\"%s\" of none: check it isn't none first", x->s);
        if (o.t != V_REC)
            fault(rt, x->line, "\".%s\" needs a record, got %s", x->s, fx_type_name(o));
        bool has = false;
        for (int i = 0; i < o.o->n; i++)
            has |= !strcmp(o.o->names[i], x->s);
        if (!has) {
            char fields[160] = "";
            for (int i = 0; i < o.o->n && strlen(fields) < 120; i++)
                snprintf(fields + strlen(fields), sizeof fields - strlen(fields), "%s%s", i ? ", " : "", o.o->names[i]);
            fault(rt, x->line, "no field \"%s\" here; it has: %s", x->s, fields);
        }
        fx_val v = fx_ref(fx_rec_get(o, x->s));
        fx_unref(o);
        return v;
    }
    case E_INDEX: {
        fx_val o = eval(e, x->a), i = eval(e, x->b);
        double n = need_num(rt, i, x->line, "an index");
        fx_val v;
        if (o.t == V_LIST) {
            if (n < 0 || n >= o.o->n || n != floor(n))
                fault(rt, x->line, "index %g is outside the list (it has %d)", n, o.o->n);
            v = fx_ref(o.o->items[(int)n]);
        } else if (o.t == V_TEXT) {
            if (n < 0 || n >= (double)o.o->len)
                fault(rt, x->line, "index %g is outside the text", n);
            v = fx_textn(o.o->s + (int)n, 1);
        } else {
            fault(rt, x->line, "[...] needs a list, got %s", fx_type_name(o));
        }
        fx_unref(o);
        return v;
    }
    case E_CALL: {
        if (++rt->depth > 200)
            fault(rt, x->line, "fns calling each other too deeply (does one call itself forever?)");
        fx_val r;
        if (fx_is_stdlib(x->s, NULL, NULL))
            r = stdlib_call(e, x);
        else if (fx_is_effect(x->s, NULL, NULL)) {
            if (!e->in_fn)
                fault(rt, x->line, "%s only runs inside a fn", x->s);
            r = effect_call(e, x);
        } else {
            const fx_fn *fn = find_fn(e->in->app, x->s);
            if (!fn)
                fault(rt, x->line, "unknown fn \"%s\"", x->s);
            fx_val args[16];
            int n = x->n < 16 ? x->n : 16;
            for (int i = 0; i < n; i++)
                args[i] = eval(e, x->items[i]);
            r = call_fn(e->in, fn, args, n, x->line);
            for (int i = 0; i < n; i++)
                fx_unref(args[i]);
        }
        rt->depth--;
        return r;
    }
    case E_LAMBDA: fault(rt, x->line, "a lambda (x -> ...) only goes to map, filter, find or sort");
    case E_LIST: {
        fx_val l = fx_list();
        for (int i = 0; i < x->n; i++)
            fx_list_push(l, eval(e, x->items[i]));
        return l;
    }
    case E_REC: {
        fx_val r = fx_rec();
        for (int i = 0; i < x->n; i++)
            fx_rec_set(r, x->names[i], eval(e, x->items[i]));
        return r;
    }
    case E_UNARY: {
        fx_val a = eval(e, x->a);
        fx_val r;
        if (!strcmp(x->s, "not"))
            r = fx_bool(!fx_truthy(a));
        else
            r = fx_num(-need_num(rt, a, x->line, "-"));
        fx_unref(a);
        return r;
    }
    case E_BINARY: return binary(e, x);
    case E_IF: {
        fx_val c = eval(e, x->a);
        bool t = fx_truthy(c);
        fx_unref(c);
        return eval(e, t ? x->b : x->c);
    }
    }
    return fx_none();
}

/* ---- statements ---- */

static void emit(fx_inst *in, const char *event, fx_val value, int line);

static void exec(ev *e, fx_stmt **s, int n) {
    fx_rt *rt = e->in->rt;
    for (int i = 0; i < n && !e->fr->returned; i++) {
        fx_stmt *st = s[i];
        switch (st->k) {
        case S_LET:
            if (e->fr->n >= 64)
                fault(rt, st->line, "too many locals in one fn");
            e->fr->names[e->fr->n] = st->name;
            e->fr->vals[e->fr->n++] = eval(e, st->e);
            break;
        case S_SET: {
            fx_val v = eval(e, st->e);
            bool done = false;
            for (int k = e->fr->n - 1; k >= 0 && !done; k--)
                if (!strcmp(e->fr->names[k], st->name)) {
                    fx_unref(e->fr->vals[k]);
                    e->fr->vals[k] = v;
                    done = true;
                }
            if (done)
                break;
            int k = index_of(e->in->app->state, e->in->app->nstate, st->name);
            if (k < 0)
                fault(rt, st->line, "set %s: no state by that name", st->name);
            if (!fx_equal(e->in->state[k], v))
                changed(rt);
            fx_unref(e->in->state[k]);
            e->in->state[k] = v;
            break;
        }
        case S_IF: {
            fx_val c = eval(e, st->e);
            bool t = fx_truthy(c);
            fx_unref(c);
            if (t)
                exec(e, st->body, st->nbody);
            else
                exec(e, st->alt, st->nalt);
            break;
        }
        case S_FOR: {
            fx_val list = eval(e, st->e);
            fx_obj *l = need_list(rt, list, st->line, "for");
            for (int k = 0; k < l->n && !e->fr->returned; k++) {
                fx_scope sc = {st->name, l->items[k], e->sc};
                ev inner = *e;
                inner.sc = &sc;
                exec(&inner, st->body, st->nbody);
            }
            fx_unref(list);
            break;
        }
        case S_RETURN:
            e->fr->ret = st->e ? eval(e, st->e) : fx_none();
            e->fr->returned = true;
            break;
        case S_CALL: fx_unref(eval(e, st->e)); break;
        case S_EMIT: {
            fx_val v = st->e ? eval(e, st->e) : fx_none();
            emit(e->in, st->name, v, st->line);
            fx_unref(v);
            break;
        }
        }
    }
}

static fx_val call_fn(fx_inst *in, const fx_fn *fn, fx_val *args, int nargs, int line) {
    if (nargs != fn->nparams)
        fault(in->rt, line, "%s takes %d value%s, not %d", fn->name, fn->nparams, fn->nparams == 1 ? "" : "s", nargs);
    frame fr = {0};
    for (int i = 0; i < nargs && i < 64; i++) {
        fr.names[fr.n] = fn->params[i];
        fr.vals[fr.n++] = fx_ref(args[i]);
    }
    ev e = {in, NULL, &fr, true};
    exec(&e, fn->body, fn->nbody);
    for (int i = 0; i < fr.n; i++)
        fx_unref(fr.vals[i]);
    return fr.returned ? fr.ret : fx_none();
}

/* a call written in markup (on:click="pick(n)"), with the names it saw and the event's value */
static void run_call(fx_inst *in, const fx_expr *call, fx_scope *scope, fx_val value) {
    fx_scope val = {"value", value, scope};
    ev e = {in, &val, NULL, true};
    frame fr = {0};
    e.fr = &fr;
    fx_val r = eval(&e, call);
    fx_unref(r);
}

static void emit(fx_inst *in, const char *event, fx_val value, int line) {
    if (!in->owner || !in->el)
        fault(in->rt, line, "emit \"%s\": nothing is listening", event);
    char attr[80];
    snprintf(attr, sizeof attr, "on:%s", event);
    for (int i = 0; i < in->el->nattrs; i++)
        if (!strcmp(in->el->attrs[i].name, attr) && in->el->attrs[i].e) {
            run_call(in->owner, in->el->attrs[i].e, in->el_scope, value);
            return;
        }
    /* nobody listens: that's allowed */
}

/* ---- live data ---- */

static double data_every(const fx_expr *call, fx_val *args) {
    if (!strcmp(call->s, "clock"))
        return args[0].t == V_NUM && args[0].n >= 10 ? args[0].n : 1000;
    if (!strcmp(call->s, "apps"))
        return 3000;
    if (!strcmp(call->s, "web"))
        return 400; /* the host fetches on its own schedule: this only looks for what has arrived */
    return 1000;
}

static fx_val clock_record(double ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    fx_val r = fx_rec();
    fx_rec_set(r, "time", fx_num(ms));
    fx_rec_set(r, "hour", fx_num(tm.tm_hour));
    fx_rec_set(r, "minute", fx_num(tm.tm_min));
    fx_rec_set(r, "second", fx_num(tm.tm_sec));
    fx_rec_set(r, "ms", fx_num(fmod(ms, 1000)));
    fx_rec_set(r, "day", fx_num(tm.tm_mday));
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    fx_rec_set(r, "weekday", fx_text(days[tm.tm_wday]));
    return r;
}

static bool refresh_data(fx_inst *in, bool force) {
    fx_rt *rt = in->rt;
    const fx_host *h = &rt->host;
    double now = h->now_ms ? h->now_ms(h->ctx) : 0;
    bool any = false;
    for (int i = 0; i < in->app->ndata; i++) {
        const fx_expr *call = in->app->data[i].e;
        if (!call || call->k != E_CALL)
            continue;
        ev e = {in, NULL, NULL, false};
        fx_val args[2] = {fx_none(), fx_none()};
        for (int k = 0; k < call->n && k < 2; k++)
            args[k] = eval(&e, call->items[k]);
        char sig[600] = "";
        for (int k = 0; k < call->n && k < 2; k++) {
            char *s = fx_show(args[k]);
            snprintf(sig + strlen(sig), sizeof sig - strlen(sig), "%s\x1f", s);
            free(s);
        }
        bool due = force || !in->data_sig[i] || strcmp(in->data_sig[i], sig) ||
                   now - in->data_at[i] >= data_every(call, args);
        if (due) {
            fx_val v = fx_none();
            if (!strcmp(call->s, "files")) {
                char *dir = home_path(args[0].t == V_TEXT ? args[0].o->s : "~");
                v = h->files ? h->files(h->ctx, dir, args[1].t == V_TEXT ? args[1].o->s : NULL) : fx_list();
                free(dir);
            } else if (!strcmp(call->s, "file")) {
                const char *p = args[0].t == V_TEXT ? args[0].o->s : "";
                char *path = home_path(p);
                char *text = h->read ? h->read(h->ctx, path) : NULL;
                v = fx_rec();
                const char *slash = strrchr(path, '/');
                fx_rec_set(v, "name", fx_text(slash ? slash + 1 : path));
                fx_rec_set(v, "path", fx_text(path));
                fx_rec_set(v, "text", fx_text(text ? text : ""));
                free(text);
                free(path);
            } else if (!strcmp(call->s, "setting")) {
                v = h->setting && args[0].t == V_TEXT ? h->setting(h->ctx, args[0].o->s) : fx_none();
            } else if (!strcmp(call->s, "apps")) {
                v = h->apps ? h->apps(h->ctx) : fx_list();
            } else if (!strcmp(call->s, "clock")) {
                v = clock_record(now);
            } else if (!strcmp(call->s, "web")) {
                double every = args[1].t == V_NUM ? args[1].n : 600000;
                if (args[0].t == V_TEXT && h->web && net_allowed(rt))
                    v = h->web(h->ctx, args[0].o->s, every < 1000 ? 1000 : every);
            }
            if (!fx_equal(v, in->data[i])) {
                fx_unref(in->data[i]);
                in->data[i] = v;
                any = true;
            } else {
                fx_unref(v);
            }
            free(in->data_sig[i]);
            in->data_sig[i] = strdup(sig);
            in->data_at[i] = now;
        }
        fx_unref(args[0]);
        fx_unref(args[1]);
    }
    if (any)
        changed(rt);
    return any;
}

/* ---- instances ---- */

static fx_inst *inst_new(fx_rt *rt, const fx_app *app) {
    fx_inst *in = calloc(1, sizeof *in);
    in->app = app;
    in->rt = rt;
    int nd = app->ndata, ns = app->nstate, nv = app->nderived, np = app->nparams;
    in->data = calloc((size_t)(nd + 1), sizeof *in->data);
    in->state = calloc((size_t)(ns + 1), sizeof *in->state);
    in->derived = calloc((size_t)(nv + 1), sizeof *in->derived);
    in->params = calloc((size_t)(np + 1), sizeof *in->params);
    in->dgen = calloc((size_t)(nv + 1), sizeof *in->dgen);
    in->dpv = calloc((size_t)(nv + 1), sizeof *in->dpv);
    in->dbusy = calloc((size_t)(nv + 1), sizeof *in->dbusy);
    in->data_sig = calloc((size_t)(nd + 1), sizeof *in->data_sig);
    in->data_at = calloc((size_t)(nd + 1), sizeof *in->data_at);
    for (int i = 0; i < nv; i++)
        in->dgen[i] = (uint64_t)-1;
    return in;
}

static void inst_init_state(fx_inst *in) {
    for (int i = 0; i < in->app->nstate; i++) {
        ev e = {in, NULL, NULL, false};
        in->state[i] = eval(&e, in->app->state[i].e);
    }
}

static void free_scope(fx_scope *s);

static void inst_free(fx_inst *in) {
    if (!in)
        return;
    for (int i = 0; i < in->app->ndata; i++)
        fx_unref(in->data[i]), free(in->data_sig[i]);
    for (int i = 0; i < in->app->nstate; i++)
        fx_unref(in->state[i]);
    for (int i = 0; i < in->app->nderived; i++)
        fx_unref(in->derived[i]);
    for (int i = 0; i < in->app->nparams; i++)
        fx_unref(in->params[i]);
    free(in->data), free(in->state), free(in->derived), free(in->params);
    free(in->dgen), free(in->dpv), free(in->dbusy), free(in->data_sig), free(in->data_at);
    free(in->path);
    free_scope(in->el_scope);
    for (int i = 0; i < 32; i++)
        if (in->rt->timers[i].inst == in)
            in->rt->timers[i].live = false;
    free(in);
}

/* ---- the view ---- */

static fx_scope *keep_scope(fx_scope *s) { /* a copy that outlives the render's stack */
    if (!s)
        return NULL;
    fx_scope *k = malloc(sizeof *k);
    k->name = s->name;
    k->v = fx_ref(s->v);
    k->up = keep_scope(s->up);
    return k;
}
static void free_scope(fx_scope *s) {
    while (s) {
        fx_scope *up = s->up;
        fx_unref(s->v);
        free(s);
        s = up;
    }
}

static void out_free(fx_out *o) {
    if (!o)
        return;
    free(o->tag), free(o->text), free(o->key), free(o->bind);
    for (int i = 0; i < o->nattrs; i++)
        free(o->anames[i]), free(o->avals[i]);
    free(o->anames), free(o->avals);
    for (int i = 0; i < o->nh; i++)
        free(o->h[i].event), free_scope(o->h[i].scope);
    free(o->h);
    for (int i = 0; i < o->nkids; i++)
        out_free(o->kids[i]);
    free(o->kids);
    free(o);
}

static void out_attr(fx_out *o, const char *name, const char *val) {
    for (int i = 0; i < o->nattrs; i++)
        if (!strcmp(o->anames[i], name)) {
            free(o->avals[i]);
            o->avals[i] = strdup(val);
            return;
        }
    o->anames = realloc(o->anames, sizeof *o->anames * (size_t)(o->nattrs + 1));
    o->avals = realloc(o->avals, sizeof *o->avals * (size_t)(o->nattrs + 1));
    o->anames[o->nattrs] = strdup(name);
    o->avals[o->nattrs++] = strdup(val);
}

static void out_kid(fx_out *parent, fx_out *kid) {
    parent->kids = realloc(parent->kids, sizeof *parent->kids * (size_t)(parent->nkids + 1));
    parent->kids[parent->nkids++] = kid;
}

static fx_inst *comp_inst(fx_rt *rt, const fx_app *comp, const char *path) {
    for (fx_inst *c = rt->comps; c; c = c->next)
        if (c->app == comp && !strcmp(c->path, path)) {
            c->seen = true;
            return c;
        }
    fx_inst *c = inst_new(rt, comp);
    c->path = strdup(path);
    c->seen = true;
    c->next = rt->comps;
    rt->comps = c;
    return c; /* its state starts once its parameters are known */
}

static void render_kids(ev *e, const fx_node *n, int from, int to, fx_out *parent, const char *path);

static void render_node(ev *e, const fx_node *n, fx_out *parent, const char *path, int index) {
    fx_rt *rt = e->in->rt;
    if (!n->tag) {
        fx_val v = eval(e, n->text);
        fx_out *o = calloc(1, sizeof *o);
        o->text = fx_show(v);
        fx_unref(v);
        o->line = n->line;
        out_kid(parent, o);
        return;
    }
    char key[512];
    if (!strcmp(n->tag, "each")) {
        const fx_attr *in_a = NULL, *key_a = NULL;
        const char *as = NULL, *at = NULL;
        for (int i = 0; i < n->nattrs; i++) {
            const fx_attr *a = &n->attrs[i];
            if (!strcmp(a->name, "in")) in_a = a;
            else if (!strcmp(a->name, "key")) key_a = a;
            else if (!strcmp(a->name, "as")) as = a->text;
            else if (!strcmp(a->name, "at")) at = a->text;
        }
        fx_val list = in_a ? eval(e, in_a->e) : fx_list();
        fx_obj *l = need_list(rt, list, n->line, "<each in>");
        for (int i = 0; i < l->n; i++) {
            fx_scope s_as = {as ? as : "", l->items[i], e->sc};
            fx_scope s_at = {at ? at : "", fx_num(i), &s_as};
            ev inner = *e;
            inner.sc = at ? &s_at : &s_as;
            fx_val k = key_a ? eval(&inner, key_a->e) : fx_num(i);
            char *ks = fx_show(k);
            fx_unref(k);
            snprintf(key, sizeof key, "%s/%s", path, ks);
            free(ks);
            render_kids(&inner, n, 0, n->nkids, parent, key);
        }
        fx_unref(list);
        return;
    }
    if (!strcmp(n->tag, "if")) {
        int else_at = n->nkids;
        for (int i = 0; i < n->nkids; i++)
            if (n->kids[i]->tag && !strcmp(n->kids[i]->tag, "else"))
                else_at = i;
        bool t = false;
        for (int i = 0; i < n->nattrs; i++)
            if (!strcmp(n->attrs[i].name, "test")) {
                fx_val v = eval(e, n->attrs[i].e);
                t = fx_truthy(v);
                fx_unref(v);
            }
        snprintf(key, sizeof key, "%s/if%d%s", path, index, t ? "y" : "n");
        if (t)
            render_kids(e, n, 0, else_at, parent, key);
        else
            render_kids(e, n, else_at + 1, n->nkids, parent, key);
        return;
    }
    if (!strcmp(n->tag, "else"))
        return;
    const fx_app *root = rt->app, *comp = NULL;
    for (int i = 0; i < root->ncomps; i++)
        if (!strcmp(root->comps[i].name, n->tag))
            comp = &root->comps[i];
    snprintf(key, sizeof key, "%s/%s%d", path, n->tag, index);
    if (comp) {
        fx_inst *c = comp_inst(rt, comp, key);
        bool fresh = c->owner == NULL;
        bool params_changed = false;
        for (int p = 0; p < comp->nparams; p++)
            for (int i = 0; i < n->nattrs; i++)
                if (!strcmp(n->attrs[i].name, comp->params[p])) {
                    const fx_attr *a = &n->attrs[i];
                    fx_val v = a->e ? eval(e, a->e) : fx_text(a->text ? a->text : "");
                    if (!fx_equal(v, c->params[p]))
                        params_changed = true;
                    fx_unref(c->params[p]);
                    c->params[p] = v;
                }
        if (params_changed)
            c->pv++;
        c->owner = e->in;
        c->el = n;
        free_scope(c->el_scope);
        c->el_scope = keep_scope(e->sc);
        if (fresh)
            inst_init_state(c);
        if (comp->view) {
            ev ce = {c, NULL, NULL, false};
            size_t before = (size_t)parent->nkids;
            render_node(&ce, comp->view, parent, key, 0);
            /* the id and class written on the component's tag go on its root */
            if ((size_t)parent->nkids > before) {
                fx_out *r = parent->kids[parent->nkids - 1];
                for (int i = 0; i < n->nattrs; i++)
                    if ((!strcmp(n->attrs[i].name, "id") || !strcmp(n->attrs[i].name, "class")) && n->attrs[i].text)
                        out_attr(r, n->attrs[i].name, n->attrs[i].text);
            }
        }
        return;
    }
    fx_out *o = calloc(1, sizeof *o);
    o->tag = strdup(n->tag);
    o->line = n->line;
    char classes[256] = "";
    for (int i = 0; i < n->nattrs; i++) {
        const fx_attr *a = &n->attrs[i];
        if (!strncmp(a->name, "on:", 3)) {
            o->h = realloc(o->h, sizeof *o->h * (size_t)(o->nh + 1));
            o->h[o->nh++] = (fx_handler){strdup(a->name + 3), e->in, a->e, keep_scope(e->sc)};
            continue;
        }
        if (!strcmp(a->name, "bind")) {
            o->bind = strdup(a->text ? a->text : "");
            o->bind_inst = e->in;
            fx_val v;
            if (a->text && lookup(e, a->text, &v)) {
                char *s = fx_show(v);
                out_attr(o, v.t == V_BOOL ? "checked" : "value", s);
                free(s);
                fx_unref(v);
            }
            continue;
        }
        if (!strncmp(a->name, "class:", 6)) {
            fx_val v = eval(e, a->e);
            if (fx_truthy(v))
                snprintf(classes + strlen(classes), sizeof classes - strlen(classes), " %s", a->name + 6);
            fx_unref(v);
            continue;
        }
        if (!strcmp(a->name, "class")) {
            snprintf(classes + strlen(classes), sizeof classes - strlen(classes), " %s", a->text ? a->text : "");
            continue;
        }
        if (a->k == A_FLAG) {
            out_attr(o, a->name, "true");
        } else if (a->k == A_LITERAL) {
            out_attr(o, a->name, a->text ? a->text : "");
        } else {
            fx_val v = eval(e, a->e);
            if (v.t == V_BOOL || v.t == V_NONE) {
                if (fx_truthy(v))
                    out_attr(o, a->name, "true");
            } else {
                char *s = fx_show(v);
                out_attr(o, a->name, s);
                free(s);
            }
            fx_unref(v);
        }
    }
    if (classes[0])
        out_attr(o, "class", classes + 1);
    const char *id = fx_get(o, "id");
    snprintf(key, sizeof key, "%s/%s%s%s", path, n->tag, id ? "#" : "", id ? id : "");
    if (!id)
        snprintf(key + strlen(key), sizeof key - strlen(key), "%d", index);
    o->key = strdup(key);
    out_kid(parent, o);
    render_kids(e, n, 0, n->nkids, o, key);
}

static void render_kids(ev *e, const fx_node *n, int from, int to, fx_out *parent, const char *path) {
    for (int i = from; i < to; i++)
        render_node(e, n->kids[i], parent, path, i);
}

/* ---- the outside ---- */

fx_rt *fx_start(const fx_app *app, const fx_host *host) {
    fx_rt *rt = calloc(1, sizeof *rt);
    rt->app = app;
    rt->host = *host;
    rt->root = inst_new(rt, app);
    jmp_buf jb;
    rt->jb = &jb;
    if (!setjmp(jb)) {
        /* data first (state may start from it), then state, then data again (it may follow state) */
        refresh_data(rt->root, true);
        inst_init_state(rt->root);
        refresh_data(rt->root, true);
    }
    rt->jb = NULL;
    rt->dirty = true;
    return rt;
}

void fx_stop(fx_rt *rt) {
    if (!rt)
        return;
    for (int i = 0; i < 16; i++)
        if (rt->socks[i].live) {
            if (rt->host.net_close)
                rt->host.net_close(rt->host.ctx, rt->socks[i].id);
            free(rt->socks[i].buf);
        }
    out_free(rt->out);
    while (rt->comps) {
        fx_inst *n = rt->comps->next;
        inst_free(rt->comps);
        rt->comps = n;
    }
    inst_free(rt->root);
    free(rt);
}

static struct sock *sock_of(fx_rt *rt, int id) {
    for (int i = 0; i < 16; i++)
        if (rt->socks[i].live && rt->socks[i].id == id)
            return &rt->socks[i];
    return NULL;
}

static void sock_tell(struct sock *s, const char *kind, fx_val data) {
    const fx_fn *fn = find_fn(s->inst->app, s->fn);
    if (!fn)
        return;
    fx_val args[3] = {fx_num(s->id), fx_text(kind), data};
    fx_val r = call_fn(s->inst, fn, args, fn->nparams < 3 ? fn->nparams : 3, fn->line);
    fx_unref(r);
    fx_unref(args[1]);
}

/* everything that happened on its sockets since the last frame */
static void net_events(fx_rt *rt) {
    const fx_host *h = &rt->host;
    if (!h->net_event)
        return;
    int id, kind, guard = 0;
    char *data;
    size_t len;
    while (guard++ < 256 && (kind = h->net_event(h->ctx, &id, &data, &len)) > 0) {
        struct sock *s = sock_of(rt, id);
        if (!s) {
            free(data);
            continue;
        }
        if (kind == 1) {
            sock_tell(s, "open", fx_none());
        } else if (kind == 2 && s->bytes) {
            fx_val l = fx_list();
            for (size_t i = 0; i < len; i++)
                fx_list_push(l, fx_num((unsigned char)data[i]));
            sock_tell(s, "bytes", l);
            fx_unref(l);
        } else if (kind == 2) { /* lines: whole ones only, without their \r\n */
            s->buf = realloc(s->buf, s->len + len + 1);
            memcpy(s->buf + s->len, data, len);
            s->len += len;
            size_t start = 0;
            for (size_t i = 0; i < s->len; i++)
                if (s->buf[i] == '\n') {
                    size_t end = i;
                    if (end > start && s->buf[end - 1] == '\r')
                        end--;
                    fx_val t = fx_textn(s->buf + start, end - start);
                    sock_tell(s, "line", t);
                    fx_unref(t);
                    start = i + 1;
                    if (!s->live) /* it closed itself while hearing */
                        break;
                }
            if (s->live && s->buf) {
                memmove(s->buf, s->buf + start, s->len - start);
                s->len -= start;
            }
        } else if (kind == 3 || kind == 4) {
            fx_val why = kind == 4 ? fx_text(data ? data : "") : fx_none();
            s->live = false;
            sock_tell(s, kind == 3 ? "closed" : "error", why);
            fx_unref(why);
            free(s->buf);
            s->buf = NULL;
        }
        free(data);
    }
}

bool fx_tick(fx_rt *rt) {
    bool before = rt->dirty;
    jmp_buf jb, *outer = rt->jb;
    rt->jb = &jb;
    if (!setjmp(jb)) {
        net_events(rt);
        refresh_data(rt->root, rt->refetch);
        rt->refetch = false;
        double now = rt->host.now_ms ? rt->host.now_ms(rt->host.ctx) : 0;
        for (int i = 0; i < 32; i++) {
            timer *t = &rt->timers[i];
            if (!t->live || now < t->due)
                continue;
            if (t->every)
                t->due = now + t->every;
            else
                t->live = false;
            const fx_fn *fn = find_fn(t->inst->app, t->fn);
            if (fn) {
                fx_val r = call_fn(t->inst, fn, NULL, 0, fn->line);
                fx_unref(r);
            }
        }
        if (rt->refetch) {
            refresh_data(rt->root, true);
            rt->refetch = false;
        }
    }
    rt->jb = outer;
    (void)before;
    return rt->dirty;
}

fx_out *fx_render(fx_rt *rt) {
    out_free(rt->out);
    rt->out = calloc(1, sizeof *rt->out);
    rt->out->tag = strdup("app");
    for (fx_inst *c = rt->comps; c; c = c->next)
        c->seen = false;
    jmp_buf jb, *outer = rt->jb;
    rt->jb = &jb;
    if (!setjmp(jb)) {
        ev e = {rt->root, NULL, NULL, false};
        if (rt->app->view)
            render_node(&e, rt->app->view, rt->out, rt->app->name, 0);
    } else { /* the view itself faulted: say so where it would have been */
        for (int i = 0; i < rt->out->nkids; i++)
            out_free(rt->out->kids[i]);
        rt->out->nkids = 0;
        fx_out *o = calloc(1, sizeof *o);
        o->tag = strdup("text");
        o->key = strdup("fault");
        out_attr(o, "class", "fault");
        fx_out *t = calloc(1, sizeof *t);
        t->text = strdup(rt->fault);
        out_kid(o, t);
        out_kid(rt->out, o);
    }
    rt->jb = outer;
    /* components no longer drawn go, with their state */
    for (fx_inst **p = &rt->comps; *p;) {
        if (!(*p)->seen) {
            fx_inst *dead = *p;
            *p = dead->next;
            inst_free(dead);
        } else {
            p = &(*p)->next;
        }
    }
    rt->dirty = false;
    return rt->out;
}

bool fx_event(fx_rt *rt, fx_out *el, const char *event, fx_val value) {
    if (!el)
        return false;
    for (int i = 0; i < el->nh; i++)
        if (!strcmp(el->h[i].event, event)) {
            uint64_t g = rt->gen;
            jmp_buf jb, *outer = rt->jb;
            rt->jb = &jb;
            if (!setjmp(jb)) {
                run_call(el->h[i].inst, el->h[i].call, el->h[i].scope, value);
                refresh_data(rt->root, rt->refetch); /* data that follows state follows it now */
                rt->refetch = false;
            }
            rt->jb = outer;
            rt->depth = 0;
            return rt->gen != g;
        }
    return false;
}

bool fx_bind(fx_rt *rt, fx_out *el, fx_val value) {
    if (!el || !el->bind || !el->bind_inst)
        return false;
    fx_inst *in = el->bind_inst;
    int k = index_of(in->app->state, in->app->nstate, el->bind);
    if (k < 0)
        return false;
    if (fx_equal(in->state[k], value))
        return false;
    fx_unref(in->state[k]);
    in->state[k] = fx_ref(value);
    changed(rt);
    jmp_buf jb, *outer = rt->jb;
    rt->jb = &jb;
    if (!setjmp(jb))
        refresh_data(rt->root, false);
    rt->jb = outer;
    return true;
}

bool fx_dirty(fx_rt *rt) { return rt->dirty; }
const char *fx_fault(fx_rt *rt) { return rt->fault[0] ? rt->fault : NULL; }

const char *fx_get(const fx_out *el, const char *name) {
    for (int i = 0; el && i < el->nattrs; i++)
        if (!strcmp(el->anames[i], name))
            return el->avals[i];
    return NULL;
}

fx_out *fx_find(fx_out *root, const char *id) {
    if (!root)
        return NULL;
    const char *mine = fx_get(root, "id");
    if (mine && !strcmp(mine, id))
        return root;
    for (int i = 0; i < root->nkids; i++) {
        fx_out *f = fx_find(root->kids[i], id);
        if (f)
            return f;
    }
    return NULL;
}

fx_val fx_state_get(fx_rt *rt, const char *name) {
    int k = index_of(rt->app->state, rt->app->nstate, name);
    return k >= 0 ? rt->root->state[k] : fx_none();
}
