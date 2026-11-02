#pragma once
/* Facet 2: the language the OS's AI writes whole apps in (docs/facet2.md).
 *
 * One file is one app: keyword sections closed by `end` (data, state,
 * derived, fn, view, style, component), markup only inside view. This is
 * its reader (parse.c), its checker (check.c: every load-time error at once,
 * each with the corrected line when there is one), its evaluator (eval.c)
 * and its values (val.c). Drawing goes through the desktop's own Facet
 * engine (view.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- values ---- */

typedef enum { V_NONE, V_BOOL, V_NUM, V_TEXT, V_LIST, V_REC, V_FN } fx_vtype;

typedef struct fx_obj fx_obj;
typedef struct fx_val {
    fx_vtype t;
    union {
        bool b;
        double n;
        fx_obj *o; /* text, list, record (refcounted) */
        struct fx_lambda *fn;
    };
} fx_val;

struct fx_obj {
    int refs;
    fx_vtype t;
    /* text */
    char *s;
    size_t len;
    /* list: items; record: names[i] -> items[i] */
    fx_val *items;
    char **names;
    int n, cap;
};

fx_val fx_none(void);
fx_val fx_bool(bool b);
fx_val fx_num(double n);
fx_val fx_text(const char *s);
fx_val fx_textn(const char *s, size_t n);
fx_val fx_list(void);
fx_val fx_rec(void);
fx_val fx_ref(fx_val v); /* +1 */
void fx_unref(fx_val v);   /* -1 */
void fx_list_push(fx_val list, fx_val item); /* takes item */
void fx_rec_set(fx_val rec, const char *name, fx_val v); /* takes v */
fx_val fx_rec_get(fx_val rec, const char *name);      /* borrowed; none if absent */
bool fx_equal(fx_val a, fx_val b);
bool fx_truthy(fx_val v);
const char *fx_type_name(fx_val v);
/* As text for display ({expr} in markup and text): numbers without a needless ".0". */
char *fx_show(fx_val v); /* malloc'd */

/* ---- syntax ---- */

typedef enum {
    E_NONE, E_BOOL, E_NUM, E_TEXT, E_TEMPLATE, E_NAME, E_MEMBER, E_INDEX, E_CALL, E_LAMBDA, E_LIST, E_REC,
    E_UNARY, E_BINARY, E_IF
} fx_ekind;

typedef struct fx_expr fx_expr;
struct fx_expr {
    fx_ekind k;
    int line;
    double num;
    char *s;         /* text, name, member name, operator, call's name ("fs.write") */
    fx_expr *a, *b, *c;
    fx_expr **items; /* list items, call args, template parts, record values */
    char **names;    /* record field names, lambda params */
    int n;
};

typedef enum { S_SET, S_LET, S_IF, S_FOR, S_RETURN, S_CALL, S_EMIT } fx_skind;
typedef struct fx_stmt fx_stmt;
struct fx_stmt {
    fx_skind k;
    int line;
    char *name;      /* set/let target, for variable, emit event */
    fx_expr *e;      /* value, condition, iterable, call */
    fx_stmt **body;  /* then / loop body */
    int nbody;
    fx_stmt **alt;   /* else (an elseif is an S_IF alone in alt) */
    int nalt;
};

typedef struct fx_fn {
    char *name;
    char **params;
    int nparams;
    fx_stmt **body;
    int nbody;
    int line;
} fx_fn;

typedef struct fx_decl { /* data, state, derived: name = expr */
    char *name;
    fx_expr *e;
    int line;
} fx_decl;

/* markup */
typedef enum { A_LITERAL, A_EXPR, A_TEMPLATE, A_FLAG } fx_akind;
typedef struct fx_attr {
    char *name;
    fx_akind k;
    char *text;  /* literal value, or the raw call for on:... */
    fx_expr *e;  /* expr or template */
    int line;
} fx_attr;

typedef struct fx_node fx_node;
struct fx_node {
    char *tag;          /* NULL for text */
    fx_expr *text;      /* text content: a template */
    fx_attr *attrs;
    int nattrs;
    fx_node **kids;
    int nkids;
    int line;
};

typedef struct fx_app fx_app;
struct fx_app {
    char *name;
    char *category;     /* `app Name category notes`: what kind of app it says it is (for mismatch warnings) */
    char **params;      /* a component's */
    int nparams;
    fx_decl *data, *state, *derived;
    int ndata, nstate, nderived;
    fx_fn *fns;
    int nfns;
    fx_node *view;      /* its one root */
    char *style;        /* the rules as written (tokens resolved when drawn) */
    int style_line;
    fx_app *comps;      /* components */
    int ncomps;
    fx_app *parent;
};

/* ---- reading and checking ---- */

typedef struct fx_error {
    int line;
    char msg[240];
    char fix[240]; /* the corrected line, or empty */
} fx_error;

typedef struct fx_errors {
    fx_error *e;
    int n, cap;
    const char *file;
} fx_errors;

void fx_err(fx_errors *errs, int line, const char *fix, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* Reads a whole file. NULL (and errs filled) when it can't be read at all;
 * otherwise an app, with every problem it found in errs. */
fx_app *fx_parse(const char *src, fx_errors *errs);
/* Everything the language checks before anything runs (names, calls, the
 * view's elements and attributes, style properties and tokens...). */
void fx_check(const fx_app *app, fx_errors *errs);
void fx_free(fx_app *app);
/* Prints the errors as "file:line: message" plus the fix, one per line. */
void fx_print_errors(const fx_errors *errs, void *FILE_out);

/* Names the language knows, for checks and suggestions. */
bool fx_is_stdlib(const char *name, int *min_args, int *max_args);
bool fx_is_effect(const char *name, int *min_args, int *max_args);
const char *fx_nearest(const char *word, const char *const *choices, int n);
/* What the companion is told about writing Facet 2 apps (reference.c). */
const char *fx_reference(void);
