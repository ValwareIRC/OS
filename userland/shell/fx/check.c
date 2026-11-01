/* What Facet 2 checks before anything runs (docs/facet2.md): every name,
 * call, element, attribute and style rule, all reported at once, each with
 * a suggestion or the corrected line where there is one. */
#define _GNU_SOURCE
#include "fx.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- the vocabulary ---- */

static const struct { const char *name; int min, max; } stdlib_fns[] = {
    {"len", 1, 1},    {"lower", 1, 1},   {"upper", 1, 1},   {"trim", 1, 1},     {"split", 2, 2},  {"join", 2, 2},
    {"text", 1, 1},   {"number", 1, 1},  {"pad", 3, 3},     {"round", 1, 1},    {"floor", 1, 1},  {"ceil", 1, 1},
    {"min", 2, 2},    {"max", 2, 2},     {"slice", 2, 3},   {"first", 1, 1},    {"last", 1, 1},   {"sort", 2, 2},
    {"filter", 2, 2}, {"map", 2, 2},     {"find", 2, 2},    {"contains", 2, 2}, {"starts", 2, 2}, {"ends", 2, 2},
    {"date", 2, 2},   {"base", 1, 1},    {"dirname", 1, 1}, {"json", 1, 1},     {"parse", 1, 1},
};
static const struct { const char *name; int min, max; } effect_fns[] = {
    {"fs.read", 1, 1},   {"fs.write", 2, 2}, {"fs.remove", 1, 1}, {"fs.rename", 2, 2}, {"fs.mkdir", 1, 1},
    {"app.open", 1, 2},  {"app.close", 0, 0}, {"app.notify", 1, 1}, {"ask", 1, 1},     {"prompt", 2, 2},
    {"after", 3, 3},     {"every", 3, 3},    {"cancel", 1, 1},
    {"net.get", 1, 1},   {"net.post", 2, 3}, {"net.connect", 4, 4}, {"net.send", 2, 2}, {"net.close", 1, 1},
};
static const struct { const char *name; int min, max; } data_fns[] = {
    {"files", 1, 2}, {"file", 1, 1}, {"setting", 1, 1}, {"apps", 0, 0}, {"clock", 1, 1}, {"web", 1, 2},
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

bool fx_is_stdlib(const char *name, int *mn, int *mx) {
    for (int i = 0; i < COUNT(stdlib_fns); i++)
        if (!strcmp(stdlib_fns[i].name, name)) {
            if (mn) *mn = stdlib_fns[i].min;
            if (mx) *mx = stdlib_fns[i].max;
            return true;
        }
    return false;
}
bool fx_is_effect(const char *name, int *mn, int *mx) {
    for (int i = 0; i < COUNT(effect_fns); i++)
        if (!strcmp(effect_fns[i].name, name)) {
            if (mn) *mn = effect_fns[i].min;
            if (mx) *mx = effect_fns[i].max;
            return true;
        }
    return false;
}
static bool is_data_fn(const char *name, int *mn, int *mx) {
    for (int i = 0; i < COUNT(data_fns); i++)
        if (!strcmp(data_fns[i].name, name)) {
            *mn = data_fns[i].min, *mx = data_fns[i].max;
            return true;
        }
    return false;
}

static const char *const elements[] = {"row",   "column", "grid",  "stack",  "panel",  "text",   "button",
                                       "input", "field",  "checkbox", "icon", "image", "spacer", "divider",
                                       "scroll", "canvas", "popover", "sheet", "each", "if", "else",
                                       "ring", "progress", "chart", "avatar", "badge"};
static const char *const style_props[] = {"background", "color",  "border",  "radius",  "padding",   "margin",
                                          "gap",        "width",  "height",  "grow",    "font",      "size",
                                          "weight",     "shadow", "blur",    "opacity", "align",     "justify",
                                          "direction",  "transition", "gradient", "glow", "depth", "scale",
                                          "move",       "text",   "upper",   "line",    "columns",   "outline"};
const char *const fx_tokens[] = {"accent", "accent2", "background", "surface", "surface2", "text", "muted",
                                 "danger", "success", "border", "font-body", "font-heading", "font-mono", NULL};
static const char *const attrs_any[] = {"id", "class", "disabled", "autofocus", "placeholder", "multiline", "bind",
                                        "checked", "name", "src", "value", "max", "label", "data", "labels", "kind",
                                        "text"};

static int lev(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40)
        return 99;
    int d[41][41];
    for (int i = 0; i <= la; i++)
        d[i][0] = i;
    for (int j = 0; j <= lb; j++)
        d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int c = tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]);
            int v = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < v) v = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + c < v) v = d[i - 1][j - 1] + c;
            d[i][j] = v;
        }
    return d[la][lb];
}

const char *fx_nearest(const char *word, const char *const *choices, int n) {
    const char *best = NULL;
    int bd = 99;
    for (int i = 0; i < n && choices[i]; i++) {
        int d = lev(word, choices[i]);
        if (strcasestr(choices[i], word) || strcasestr(word, choices[i]))
            d = d < 2 ? d : 2;
        if (d < bd)
            bd = d, best = choices[i];
    }
    int len = (int)strlen(word);
    return best && bd <= (len <= 4 ? 1 : len <= 8 ? 2 : 3) ? best : NULL;
}

static void arity_err(fx_errors *errs, int line, const char *name, int mn, int mx, int got) {
    if (mn == mx)
        fx_err(errs, line, NULL, "%s takes %d value%s, not %d", name, mn, mn == 1 ? "" : "s", got);
    else
        fx_err(errs, line, NULL, "%s takes %d to %d values, not %d", name, mn, mx, got);
}

/* ---- scopes ---- */

enum { N_DATA, N_STATE, N_DERIVED, N_PARAM, N_LOCAL, N_LOOP };
typedef struct {
    const char *name;
    int kind;
} sym;

typedef struct {
    const fx_app *app, *root;
    fx_errors *errs;
    sym syms[256];
    int nsyms;
    bool in_fn, in_derived, in_data, in_view;
    const fx_fn *fn;
} ctx;

static const sym *lookup(ctx *c, const char *name) {
    for (int i = c->nsyms - 1; i >= 0; i--)
        if (!strcmp(c->syms[i].name, name))
            return &c->syms[i];
    return NULL;
}
static void push(ctx *c, const char *name, int kind) {
    if (c->nsyms < 256)
        c->syms[c->nsyms++] = (sym){name, kind};
}

static const fx_fn *find_fn(const fx_app *a, const char *name) {
    for (int i = 0; i < a->nfns; i++)
        if (!strcmp(a->fns[i].name, name))
            return &a->fns[i];
    return NULL;
}
static const fx_app *find_comp(const fx_app *root, const char *name) {
    for (int i = 0; i < root->ncomps; i++)
        if (!strcmp(root->comps[i].name, name))
            return &root->comps[i];
    return NULL;
}

/* every name that could have been meant, for suggestions */
static const char *suggest(ctx *c, const char *name) {
    const char *all[512];
    int n = 0;
    for (int i = 0; i < c->nsyms && n < 500; i++)
        all[n++] = c->syms[i].name;
    for (int i = 0; i < c->app->nfns && n < 500; i++)
        all[n++] = c->app->fns[i].name;
    for (int i = 0; i < COUNT(stdlib_fns) && n < 500; i++)
        all[n++] = stdlib_fns[i].name;
    for (int i = 0; i < COUNT(effect_fns) && n < 500; i++)
        all[n++] = effect_fns[i].name;
    return fx_nearest(name, all, n);
}

/* ---- purity: fns a derived value may call ---- */

static fx_stmt **pure_fn_body; /* the fn being judged: its own locals may change */
static int pure_fn_n;

static bool stmts_pure(const fx_app *a, fx_stmt **s, int n, int depth);
static bool expr_pure(const fx_app *a, const fx_expr *e, int depth) {
    if (!e)
        return true;
    if (e->k == E_CALL) {
        if (fx_is_effect(e->s, NULL, NULL))
            return false;
        const fx_fn *f = find_fn(a, e->s);
        if (f) {
            fx_stmt **kb = pure_fn_body;
            int kn = pure_fn_n;
            pure_fn_body = f->body, pure_fn_n = f->nbody;
            bool pure = depth <= 16 && stmts_pure(a, f->body, f->nbody, depth + 1);
            pure_fn_body = kb, pure_fn_n = kn;
            if (!pure)
                return false;
        }
    }
    if (!expr_pure(a, e->a, depth) || !expr_pure(a, e->b, depth) || !expr_pure(a, e->c, depth))
        return false;
    for (int i = 0; i < e->n; i++)
        if (e->items && !expr_pure(a, e->items[i], depth))
            return false;
    return true;
}
/* the fn's own locals (let), which it may change freely and stay pure */
static bool is_local(fx_stmt **s, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (s[i]->k == S_LET && !strcmp(s[i]->name, name))
            return true;
        if ((s[i]->k == S_FOR && !strcmp(s[i]->name, name)) || is_local(s[i]->body, s[i]->nbody, name) ||
            is_local(s[i]->alt, s[i]->nalt, name))
            return true;
    }
    return false;
}
static bool stmts_pure(const fx_app *a, fx_stmt **s, int n, int depth) {
    for (int i = 0; i < n; i++) {
        if (s[i]->k == S_EMIT || (s[i]->k == S_SET && !(pure_fn_body && is_local(pure_fn_body, pure_fn_n, s[i]->name))))
            return false;
        if (!expr_pure(a, s[i]->e, depth) || !stmts_pure(a, s[i]->body, s[i]->nbody, depth) ||
            !stmts_pure(a, s[i]->alt, s[i]->nalt, depth))
            return false;
    }
    return true;
}

/* ---- expressions ---- */

static void check_expr(ctx *c, const fx_expr *e);

static void check_call(ctx *c, const fx_expr *e) {
    int mn, mx;
    const char *where = c->in_data ? "a data line" : c->in_derived ? "derived" : c->in_view ? "the view" : NULL;
    if (fx_is_stdlib(e->s, &mn, &mx)) {
        if (e->n < mn || e->n > mx)
            arity_err(c->errs, e->line, e->s, mn, mx, e->n), (void)0;
        /* lambdas: map/filter/find take one or two names, sort one */
        if ((!strcmp(e->s, "map") || !strcmp(e->s, "filter") || !strcmp(e->s, "find") || !strcmp(e->s, "sort")) &&
            e->n == 2) {
            const fx_expr *l = e->items[1];
            if (!l || l->k != E_LAMBDA)
                fx_err(c->errs, e->line, NULL, "%s's second value is a lambda: %s(list, x -> ...)", e->s, e->s);
            else if (l->n == 2 && !strcmp(e->s, "sort"))
                fx_err(c->errs, e->line, NULL, "sort's lambda takes one name: sort(list, x -> key)");
        }
    } else if (fx_is_effect(e->s, &mn, &mx)) {
        if (!c->in_fn)
            fx_err(c->errs, e->line, NULL, "%s changes things, so it belongs in a fn, not in %s", e->s,
                   where ? where : "here");
        if (e->n < mn || e->n > mx)
            arity_err(c->errs, e->line, e->s, mn, mx, e->n);
        if ((!strcmp(e->s, "after") || !strcmp(e->s, "every")) && e->n == 3) {
            const fx_expr *f = e->items[2];
            if (f && f->k == E_CALL) {
                char fix[120];
                snprintf(fix, sizeof fix, "%s(..., ..., %s)", e->s, f->s);
                fx_err(c->errs, e->line, fix, "%s's third value is the fn's name, not a call", e->s);
            } else if (f && f->k == E_NAME && !find_fn(c->app, f->s)) {
                fx_err(c->errs, e->line, NULL, "%s: there is no fn \"%s\"", e->s, f->s);
            }
            if (e->items[0] && e->items[0]->k != E_TEXT)
                fx_err(c->errs, e->line, NULL, "%s's first value is the timer's name in quotes: %s(\"save\", ...)", e->s,
                       e->s);
        }
        if (!strcmp(e->s, "net.connect") && e->n == 4) {
            const fx_expr *f = e->items[3];
            const fx_fn *hf = f && f->k == E_NAME ? find_fn(c->app, f->s) : NULL;
            if (f && f->k == E_CALL) {
                char fix[120];
                snprintf(fix, sizeof fix, "net.connect(host, port, \"tls lines\", %s)", f->s);
                fx_err(c->errs, e->line, fix, "net.connect's fourth value is the name of the fn that hears it, not a call");
            } else if (!hf) {
                fx_err(c->errs, e->line, NULL, "net.connect: there is no fn \"%s\" (write fn %s(conn, kind, data))",
                       f && f->s ? f->s : "?", f && f->s ? f->s : "heard");
            } else if (hf->nparams != 3) {
                fx_err(c->errs, e->line, NULL, "%s hears a socket: it takes (conn, kind, data)", hf->name);
            }
        }
        if (!strcmp(e->s, "cancel") && e->n == 1 && e->items[0] && e->items[0]->k != E_TEXT)
            fx_err(c->errs, e->line, NULL, "cancel takes the timer's name in quotes: cancel(\"save\")");
    } else if (is_data_fn(e->s, &mn, &mx)) {
        if (!c->in_data)
            fx_err(c->errs, e->line, NULL, "%s() is live data: declare it as data name = %s(...) at the top", e->s, e->s);
        else if (e->n < mn || e->n > mx)
            arity_err(c->errs, e->line, e->s, mn, mx, e->n);
    } else {
        const fx_fn *f = find_fn(c->app, e->s);
        if (!f && !strcmp(e->s, "?")) {
            /* already reported where it was read */
        } else if (!f) {
            const char *near = suggest(c, e->s);
            if (near)
                fx_err(c->errs, e->line, NULL, "unknown fn \"%s\": did you mean \"%s\"?", e->s, near);
            else
                fx_err(c->errs, e->line, NULL, "unknown fn \"%s\"", e->s);
        } else {
            if (e->n != f->nparams)
                fx_err(c->errs, e->line, NULL, "%s takes %d value%s, not %d", f->name, f->nparams,
                       f->nparams == 1 ? "" : "s", e->n);
            pure_fn_body = f->body, pure_fn_n = f->nbody;
            bool pure = stmts_pure(c->app, f->body, f->nbody, 0);
            pure_fn_body = NULL, pure_fn_n = 0;
            if ((c->in_derived || c->in_view) && !pure)
                fx_err(c->errs, e->line, NULL, "%s changes things (set, emit or an effect), so %s can't call it", f->name,
                       c->in_view ? "the view" : "derived");
        }
    }
    for (int i = 0; i < e->n; i++)
        if (!((!strcmp(e->s, "after") || !strcmp(e->s, "every")) && i == 2) && !(!strcmp(e->s, "net.connect") && i == 3))
            check_expr(c, e->items[i]);
}

static void check_expr(ctx *c, const fx_expr *e) {
    if (!e)
        return;
    switch (e->k) {
    case E_NAME:
        if (!lookup(c, e->s)) {
            if (find_fn(c->app, e->s))
                fx_err(c->errs, e->line, NULL, "%s is a fn: call it with brackets, %s(...)", e->s, e->s);
            else {
                const char *near = suggest(c, e->s);
                if (near)
                    fx_err(c->errs, e->line, NULL, "unknown name \"%s\": did you mean \"%s\"?", e->s, near);
                else
                    fx_err(c->errs, e->line, NULL, "unknown name \"%s\" (declare it in state, data or derived)", e->s);
            }
        }
        break;
    case E_CALL: check_call(c, e); break;
    case E_LAMBDA: {
        int save = c->nsyms;
        for (int i = 0; i < e->n; i++)
            push(c, e->names[i], N_LOOP);
        check_expr(c, e->a);
        c->nsyms = save;
        break;
    }
    case E_BINARY: /* (mixing and/or is reported where it is read) */
        check_expr(c, e->a), check_expr(c, e->b);
        break;
    default:
        check_expr(c, e->a), check_expr(c, e->b), check_expr(c, e->c);
        if (e->k != E_LAMBDA)
            for (int i = 0; i < e->n; i++)
                if (e->items)
                    check_expr(c, e->items[i]);
        break;
    }
}

/* ---- statements ---- */

static void check_stmts(ctx *c, fx_stmt **s, int n) {
    int save = c->nsyms;
    for (int i = 0; i < n; i++) {
        fx_stmt *st = s[i];
        switch (st->k) {
        case S_LET: {
            check_expr(c, st->e);
            const sym *prev = lookup(c, st->name);
            if (prev && prev->kind != N_LOCAL && prev->kind != N_LOOP)
                fx_err(c->errs, st->line, NULL, "let %s would hide the %s of that name: choose another name", st->name,
                       prev->kind == N_STATE ? "state" : prev->kind == N_DATA ? "data" : prev->kind == N_DERIVED
                                                                                           ? "derived value"
                                                                                           : "parameter");
            else if (prev && prev->kind == N_LOCAL) {
                char fix[120];
                snprintf(fix, sizeof fix, "set %s = ...", st->name);
                fx_err(c->errs, st->line, fix, "%s is already a local: change it with set", st->name);
            }
            push(c, st->name, N_LOCAL);
            break;
        }
        case S_SET: {
            check_expr(c, st->e);
            const sym *t = lookup(c, st->name);
            if (!t) {
                const char *near = suggest(c, st->name);
                char fix[120];
                snprintf(fix, sizeof fix, "let %s = ...", st->name);
                if (near)
                    fx_err(c->errs, st->line, NULL, "set %s: no state or local by that name (did you mean \"%s\"?)",
                           st->name, near);
                else
                    fx_err(c->errs, st->line, fix, "set %s: no state or local by that name (a new local is let)",
                           st->name);
            } else if (t->kind != N_STATE && t->kind != N_LOCAL)
                fx_err(c->errs, st->line, NULL, "%s is %s: only state and locals can be set", st->name,
                       t->kind == N_DATA ? "data (read-only, from the system)"
                       : t->kind == N_DERIVED ? "derived (computed from others)"
                                              : "a parameter");
            break;
        }
        case S_IF:
            check_expr(c, st->e);
            check_stmts(c, st->body, st->nbody);
            check_stmts(c, st->alt, st->nalt);
            break;
        case S_FOR: {
            check_expr(c, st->e);
            int sv = c->nsyms;
            push(c, st->name, N_LOOP);
            check_stmts(c, st->body, st->nbody);
            c->nsyms = sv;
            break;
        }
        case S_RETURN: check_expr(c, st->e); break;
        case S_CALL: check_expr(c, st->e); break;
        case S_EMIT:
            if (!c->app->parent)
                fx_err(c->errs, st->line, NULL, "emit tells a component's parent something; the app has no parent");
            check_expr(c, st->e);
            break;
        }
    }
    c->nsyms = save;
}

/* ---- the view ---- */

static bool attr_known(const char *tag, const char *name) {
    for (int i = 0; i < COUNT(attrs_any); i++)
        if (!strcmp(attrs_any[i], name))
            return true;
    if (!strncmp(name, "class:", 6) || !strncmp(name, "on:", 3))
        return true;
    if (!strcmp(tag, "each"))
        return !strcmp(name, "in") || !strcmp(name, "as") || !strcmp(name, "key") || !strcmp(name, "at");
    if (!strcmp(tag, "if"))
        return !strcmp(name, "test");
    return false;
}

static void check_call_attr(ctx *c, const fx_attr *a, const fx_app *target) {
    if (!a->e || a->e->k != E_CALL) {
        fx_err(c->errs, a->line, NULL, "%s=\"...\" is a call of a fn: %s=\"name(...)\"", a->name, a->name);
        return;
    }
    int save = c->nsyms;
    push(c, "value", N_LOOP);
    const fx_app *keep = c->app;
    c->app = target;
    bool in_view = c->in_view;
    c->in_view = false;
    c->in_fn = true; /* its arguments are values; the fn itself is where effects go */
    if (!find_fn(target, a->e->s)) {
        const char *names[64];
        int n = 0;
        for (int i = 0; i < target->nfns && n < 64; i++)
            names[n++] = target->fns[i].name;
        const char *near = fx_nearest(a->e->s, names, n);
        if (near) {
            char fix[200];
            snprintf(fix, sizeof fix, "%s=\"%s(...)\"", a->name, near);
            fx_err(c->errs, a->line, fix, "unknown fn \"%s\" in %s: did you mean \"%s\"?", a->e->s, a->name, near);
        } else {
            fx_err(c->errs, a->line, NULL, "unknown fn \"%s\" in %s (write fn %s(...) ... end)", a->e->s, a->name,
                   a->e->s);
        }
        for (int i = 0; i < a->e->n; i++)
            check_expr(c, a->e->items[i]);
    } else {
        check_expr(c, a->e);
    }
    c->in_fn = false;
    c->in_view = in_view;
    c->app = keep;
    c->nsyms = save;
}

static void check_node(ctx *c, const fx_node *n, const fx_node *parent) {
    if (!n)
        return;
    if (!n->tag) {
        check_expr(c, n->text);
        return;
    }
    const fx_app *comp = isupper((unsigned char)n->tag[0]) ? find_comp(c->root, n->tag) : NULL;
    bool known = comp != NULL;
    for (int i = 0; i < COUNT(elements) && !known; i++)
        known = !strcmp(elements[i], n->tag);
    if (!known) {
        const char *near = fx_nearest(n->tag, elements, COUNT(elements));
        if (isupper((unsigned char)n->tag[0]))
            fx_err(c->errs, n->line, NULL, "unknown component <%s> (write component %s(...) ... end)", n->tag, n->tag);
        else if (near)
            fx_err(c->errs, n->line, NULL, "unknown element <%s>: did you mean <%s>?", n->tag, near);
        else
            fx_err(c->errs, n->line, NULL, "unknown element <%s>", n->tag);
    }
    if (!strcmp(n->tag, "else") && (!parent || !parent->tag || strcmp(parent->tag, "if")))
        fx_err(c->errs, n->line, NULL, "<else/> belongs directly inside <if>");
    bool has_bind = false, has_checked = false;
    const char *in_name = NULL, *as = NULL, *at = NULL;
    bool has_key = false, has_test = false;
    for (int i = 0; i < n->nattrs; i++) {
        const fx_attr *a = &n->attrs[i];
        if (comp) { /* a component: its parameters, on:event, id, class */
            bool param = false;
            for (int j = 0; j < comp->nparams; j++)
                param |= !strcmp(comp->params[j], a->name);
            if (!param && strncmp(a->name, "on:", 3) && strcmp(a->name, "id") && strcmp(a->name, "class") &&
                strncmp(a->name, "class:", 6))
                fx_err(c->errs, a->line, NULL, "<%s> has no parameter \"%s\"", n->tag, a->name);
        } else if (!attr_known(n->tag, a->name)) {
            static const char *const all[] = {"id",  "class", "disabled", "autofocus", "placeholder", "multiline",
                                              "bind", "checked", "name", "src", "value", "in", "as", "key", "at",
                                              "test", "max", "label", "data", "labels", "kind", "text"};
            const char *near = fx_nearest(a->name, all, COUNT(all));
            if (!strcmp(a->name, "width") || !strcmp(a->name, "height") || !strcmp(a->name, "gap") ||
                !strcmp(a->name, "grow") || !strcmp(a->name, "padding")) {
                char fix[160];
                snprintf(fix, sizeof fix, "#id { %s: %s; } in style", a->name, a->text ? a->text : "...");
                fx_err(c->errs, a->line, fix, "%s is a style property, not an attribute: give it an id and style it",
                       a->name);
            } else if (near)
                fx_err(c->errs, a->line, NULL, "<%s> has no attribute \"%s\": did you mean \"%s\"?", n->tag, a->name, near);
            else
                fx_err(c->errs, a->line, NULL, "<%s> has no attribute \"%s\"", n->tag, a->name);
        }
        if (!strncmp(a->name, "on:", 3)) {
            check_call_attr(c, a, c->app);
            continue;
        }
        if (!strcmp(a->name, "bind")) {
            has_bind = true;
            const sym *s = a->text ? lookup(c, a->text) : NULL;
            if (a->k != A_LITERAL || !s || s->kind != N_STATE) {
                const char *st[128];
                int nst = 0;
                for (int j = 0; j < c->app->nstate && nst < 128; j++)
                    st[nst++] = c->app->state[j].name;
                const char *near = a->text ? fx_nearest(a->text, st, nst) : NULL;
                if (near) {
                    char fix[120];
                    snprintf(fix, sizeof fix, "bind=\"%s\"", near);
                    fx_err(c->errs, a->line, fix, "bind=\"%s\": no state by that name; did you mean \"%s\"?",
                           a->text, near);
                } else {
                    fx_err(c->errs, a->line, NULL, "bind takes the name of a state, in quotes: bind=\"query\"");
                }
            }
            continue;
        }
        if (!strcmp(a->name, "checked"))
            has_checked = true;
        if (!strcmp(n->tag, "each")) {
            if (!strcmp(a->name, "in")) in_name = "in";
            if (!strcmp(a->name, "as")) as = a->text;
            if (!strcmp(a->name, "at")) at = a->text;
            if (!strcmp(a->name, "key")) has_key = true;
            if (!strcmp(a->name, "as") || !strcmp(a->name, "at"))
                continue; /* names, checked below */
            if ((!strcmp(a->name, "in") || !strcmp(a->name, "key")) && a->k != A_EXPR)
                fx_err(c->errs, a->line, NULL, "%s takes an expression: %s={...}", a->name, a->name);
        }
        if (!strcmp(n->tag, "if") && !strcmp(a->name, "test")) {
            has_test = true;
            if (a->k != A_EXPR)
                fx_err(c->errs, a->line, NULL, "test takes an expression: <if test={...}>");
        }
        if (!strcmp(a->name, "key") && !strcmp(n->tag, "each"))
            continue; /* checked with the item's name in scope */
        if (a->e)
            check_expr(c, a->e);
    }
    if (has_bind && has_checked)
        fx_err(c->errs, n->line, NULL, "a checkbox uses bind or checked={...}, never both");
    if (!strcmp(n->tag, "if") && !has_test)
        fx_err(c->errs, n->line, NULL, "<if> needs test={...}");
    int save = c->nsyms;
    if (!strcmp(n->tag, "each")) {
        if (!in_name)
            fx_err(c->errs, n->line, NULL, "<each> needs in={list}");
        if (!as)
            fx_err(c->errs, n->line, NULL, "<each> needs as=\"name\" for each item");
        if (!has_key)
            fx_err(c->errs, n->line, NULL, "<each> needs key={...}, unique for each item (e.g. key={n.path})");
        if (as)
            push(c, as, N_LOOP);
        if (at)
            push(c, at, N_LOOP);
        for (int i = 0; i < n->nattrs; i++)
            if (!strcmp(n->attrs[i].name, "key") && n->attrs[i].e)
                check_expr(c, n->attrs[i].e);
    }
    for (int i = 0; i < n->nkids; i++)
        check_node(c, n->kids[i], n);
    c->nsyms = save;
}

/* ---- style ---- */

static void check_style(ctx *c, const char *style, int line0) {
    if (!style)
        return;
    int line = line0;
    const char *p = style;
    while (*p) {
        /* selector */
        const char *s = p;
        while (*p && *p != '{') {
            if (*p == '\n')
                line++;
            p++;
        }
        if (!*p) {
            for (const char *q = s; *q; q++)
                if (!isspace((unsigned char)*q)) {
                    fx_err(c->errs, line, NULL, "a style rule is selector { property: value; }");
                    break;
                }
            return;
        }
        p++;
        /* declarations */
        while (*p && *p != '}') {
            while (*p && (isspace((unsigned char)*p) || *p == ';')) {
                if (*p == '\n')
                    line++;
                p++;
            }
            if (!*p || *p == '}')
                break;
            const char *ps = p;
            while (*p && *p != ':' && *p != '}' && *p != ';' && *p != '\n')
                p++;
            char prop[64];
            int pl = (int)(p - ps);
            while (pl > 0 && isspace((unsigned char)ps[pl - 1]))
                pl--;
            snprintf(prop, sizeof prop, "%.*s", pl < 63 ? pl : 63, ps);
            if (*p != ':') {
                fx_err(c->errs, line, NULL, "\"%s\" needs a value: %s: ...;", prop, prop);
                continue;
            }
            p++;
            const char *vs = p;
            while (*p && *p != ';' && *p != '}' && *p != '\n')
                p++;
            char val[160];
            snprintf(val, sizeof val, "%.*s", (int)(p - vs) < 159 ? (int)(p - vs) : 159, vs);
            bool known = false;
            for (int i = 0; i < COUNT(style_props); i++)
                known |= !strcmp(style_props[i], prop);
            if (!known) {
                const char *near = fx_nearest(prop, style_props, COUNT(style_props));
                static const struct { const char *css, *ours; } css[] = {
                    {"font-size", "size"},   {"font-weight", "weight"}, {"border-radius", "radius"},
                    {"flex", "grow"},        {"flex-grow", "grow"},     {"flex-direction", "direction"},
                    {"align-items", "align"}, {"justify-content", "justify"}, {"box-shadow", "shadow"},
                    {"background-color", "background"}, {"font-family", "font"}};
                for (int i = 0; i < COUNT(css); i++)
                    if (!strcmp(css[i].css, prop))
                        near = css[i].ours;
                if (near) {
                    char fix[200];
                    snprintf(fix, sizeof fix, "%s:%s;", near, val);
                    fx_err(c->errs, line, fix, "unknown style property \"%s\": did you mean \"%s\"?", prop, near);
                } else {
                    fx_err(c->errs, line, NULL, "unknown style property \"%s\"", prop);
                }
            }
            /* colours and fonts come from the person's design */
            bool literal_colour = strchr(val, '#') || strstr(val, "rgb") || strstr(val, "hsl");
            static const char *const named[] = {"red", "green", "blue", "white", "black", "gray", "grey", "yellow",
                                                "orange", "purple", "pink"};
            char word[32];
            for (const char *q = val; *q;) {
                while (*q && !isalpha((unsigned char)*q))
                    q++;
                int wl = 0;
                while (isalpha((unsigned char)q[wl]) && wl < 31)
                    wl++;
                snprintf(word, sizeof word, "%.*s", wl, q);
                for (int i = 0; i < COUNT(named) && wl; i++)
                    if (!strcasecmp(word, named[i]))
                        literal_colour = true;
                q += wl ? wl : 1;
                if (!wl)
                    break;
            }
            if (literal_colour && strstr(val, "token(") == NULL) {
                fx_err(c->errs, line, NULL,
                       "\"%s\" is a fixed colour: looks come from the person's design, so use a token: token(accent), "
                       "token(surface), token(text), token(muted), token(danger)...",
                       val);
            }
            if ((!strcmp(prop, "font") || !strcmp(prop, "color") || !strcmp(prop, "background")) && !strstr(val, "token(")) {
                bool blank = true;
                for (const char *q = val; *q; q++)
                    blank &= isspace((unsigned char)*q) != 0;
                if (!literal_colour && !blank && strcmp(prop, "background") != 0)
                    fx_err(c->errs, line, NULL, "%s comes from the design: %s: token(...)", prop, prop);
            }
            for (const char *q = strstr(val, "token("); q; q = strstr(q + 6, "token(")) {
                const char *ts = q + 6, *te = strchr(ts, ')');
                if (!te)
                    break;
                char tk[40];
                snprintf(tk, sizeof tk, "%.*s", (int)(te - ts) < 39 ? (int)(te - ts) : 39, ts);
                bool ok = false;
                for (int i = 0; fx_tokens[i]; i++)
                    ok |= !strcmp(fx_tokens[i], tk);
                if (!ok) {
                    int n = 0;
                    while (fx_tokens[n])
                        n++;
                    const char *near = fx_nearest(tk, fx_tokens, n);
                    if (near)
                        fx_err(c->errs, line, NULL, "no design token \"%s\": did you mean token(%s)?", tk, near);
                    else
                        fx_err(c->errs, line, NULL, "no design token \"%s\" (accent, accent2, background, surface, "
                                                    "surface2, text, muted, danger, success, border, font-body, "
                                                    "font-heading, font-mono)", tk);
                }
            }
        }
        if (*p == '}')
            p++;
    }
}

/* ---- the whole app ---- */

static void check_unit(ctx *c, const fx_app *a) {
    c->app = a;
    c->nsyms = 0;
    /* duplicate names */
    const char *seen[256];
    int ns = 0;
    fx_decl *ds[3] = {a->data, a->state, a->derived};
    int nd[3] = {a->ndata, a->nstate, a->nderived};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < nd[k]; i++) {
            for (int j = 0; j < ns; j++)
                if (!strcmp(seen[j], ds[k][i].name))
                    fx_err(c->errs, ds[k][i].line, NULL, "\"%s\" is declared twice", ds[k][i].name);
            if (ns < 256)
                seen[ns++] = ds[k][i].name;
            if (fx_is_stdlib(ds[k][i].name, NULL, NULL) || find_fn(a, ds[k][i].name))
                fx_err(c->errs, ds[k][i].line, NULL, "\"%s\" is already a fn's name: choose another", ds[k][i].name);
        }
    for (int i = 0; i < a->nfns; i++)
        for (int j = 0; j < i; j++)
            if (!strcmp(a->fns[i].name, a->fns[j].name))
                fx_err(c->errs, a->fns[i].line, NULL, "fn %s is written twice", a->fns[i].name);
    for (int i = 0; i < a->nfns; i++)
        if (fx_is_stdlib(a->fns[i].name, NULL, NULL) || fx_is_effect(a->fns[i].name, NULL, NULL)) {
            char fix[120];
            snprintf(fix, sizeof fix, "fn %s_now(...)", a->fns[i].name);
            fx_err(c->errs, a->fns[i].line, fix, "%s is already the library's %s: give your fn another name",
                   a->fns[i].name, a->fns[i].name);
        }
    for (int i = 0; i < a->nparams; i++)
        push(c, a->params[i], N_PARAM);
    /* data lines may read state (and each other) */
    for (int i = 0; i < a->ndata; i++)
        push(c, a->data[i].name, N_DATA);
    for (int i = 0; i < a->nstate; i++)
        push(c, a->state[i].name, N_STATE);
    for (int i = 0; i < a->nderived; i++)
        push(c, a->derived[i].name, N_DERIVED);
    c->in_data = true;
    for (int i = 0; i < a->ndata; i++) {
        const fx_expr *e = a->data[i].e;
        int mn, mx;
        if (!e || e->k != E_CALL || !is_data_fn(e->s, &mn, &mx))
            fx_err(c->errs, a->data[i].line, NULL, "data comes from files(), file(), setting(), apps() or clock()");
        check_expr(c, e);
    }
    c->in_data = false;
    for (int i = 0; i < a->nstate; i++)
        check_expr(c, a->state[i].e);
    c->in_derived = true;
    for (int i = 0; i < a->nderived; i++)
        check_expr(c, a->derived[i].e);
    c->in_derived = false;
    for (int i = 0; i < a->nfns; i++) {
        int save = c->nsyms;
        for (int j = 0; j < a->fns[i].nparams; j++)
            push(c, a->fns[i].params[j], N_PARAM);
        c->in_fn = true;
        c->fn = &a->fns[i];
        check_stmts(c, a->fns[i].body, a->fns[i].nbody);
        c->in_fn = false;
        c->nsyms = save;
    }
    c->in_view = true;
    check_node(c, a->view, NULL);
    c->in_view = false;
    check_style(c, a->style, a->style_line);
}

void fx_check(const fx_app *app, fx_errors *errs) {
    if (!app)
        return;
    ctx c = {0};
    c.errs = errs;
    c.root = app;
    check_unit(&c, app);
    for (int i = 0; i < app->ncomps; i++)
        check_unit(&c, &app->comps[i]);
}

void fx_print_errors(const fx_errors *errs, void *out) {
    FILE *f = out;
    for (int i = 0; i < errs->n; i++) {
        fprintf(f, "%s:%d: %s\n", errs->file ? errs->file : "app", errs->e[i].line, errs->e[i].msg);
        if (errs->e[i].fix[0])
            fprintf(f, "    fix: %s\n", errs->e[i].fix);
    }
}
