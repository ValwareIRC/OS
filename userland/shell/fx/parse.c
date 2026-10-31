/* Reading a Facet 2 file (docs/facet2.md) into its parts.
 *
 * The file is lines. The bodies of `view` and `style` are markup and rules,
 * not code: they are cut out first (kept with their line numbers) and the
 * rest is read as tokens, one statement per line. Every problem is noted
 * (fx_err) and reading goes on, so one pass reports them all. */
#define _GNU_SOURCE
#include "fx.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fx_err(fx_errors *errs, int line, const char *fix, const char *fmt, ...) {
    if (!errs)
        return;
    if (errs->n == errs->cap) {
        errs->cap = errs->cap ? errs->cap * 2 : 16;
        errs->e = realloc(errs->e, sizeof *errs->e * errs->cap);
    }
    fx_error *e = &errs->e[errs->n++];
    memset(e, 0, sizeof *e);
    e->line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    if (fix)
        snprintf(e->fix, sizeof e->fix, "%s", fix);
}

static void *zalloc(size_t n) { return calloc(1, n); }
#define PUSH(arr, n, item)                                                                                         \
    do {                                                                                                           \
        (arr) = realloc((arr), sizeof *(arr) * ((n) + 1));                                                         \
        (arr)[(n)++] = (item);                                                                                     \
    } while (0)

/* ---- tokens ---- */

enum { T_EOF, T_NL, T_NAME, T_NUM, T_STR, T_OP };
typedef struct {
    int t, line;
    char *s;   /* name, op, string contents (raw, escapes kept for templates) */
    double n;
} tok;

typedef struct {
    tok *v;
    int n, i;
    fx_errors *errs;
} toks;

static void lex(const char *src, int line0, toks *out, fx_errors *errs) {
    const char *p = src;
    int line = line0, depth = 0;
    static const char *const ops2[] = {"->", "==", "!=", "<=", ">=", NULL};
    while (*p) {
        if (*p == '\n') {
            if (!depth)
                PUSH(out->v, out->n, ((tok){T_NL, line, NULL, 0}));
            line++, p++;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }
        if (p[0] == '-' && p[1] == '-') { /* a comment to the end of the line */
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '_')
                p++;
            PUSH(out->v, out->n, ((tok){T_NAME, line, strndup(s, (size_t)(p - s)), 0}));
            continue;
        }
        if (isdigit((unsigned char)*p)) {
            char *end;
            double v = strtod(p, &end);
            PUSH(out->v, out->n, ((tok){T_NUM, line, NULL, v}));
            p = end;
            continue;
        }
        if (*p == '"') {
            const char *s = ++p;
            int sl = line;
            while (*p && *p != '"' && *p != '\n') {
                if (*p == '\\' && p[1])
                    p++;
                p++;
            }
            if (*p != '"') {
                fx_err(errs, sl, NULL, "a text that never ends: add the closing \"");
                PUSH(out->v, out->n, ((tok){T_STR, sl, strndup(s, (size_t)(p - s)), 0}));
                continue;
            }
            PUSH(out->v, out->n, ((tok){T_STR, sl, strndup(s, (size_t)(p - s)), 0}));
            p++;
            continue;
        }
        bool two = false;
        for (int i = 0; ops2[i]; i++)
            if (!strncmp(p, ops2[i], 2)) {
                PUSH(out->v, out->n, ((tok){T_OP, line, strndup(p, 2), 0}));
                p += 2;
                two = true;
                break;
            }
        if (two)
            continue;
        if (strchr("=+-*/%()[]{},.:<>", *p)) {
            if (*p == '(' || *p == '[' || *p == '{')
                depth++;
            else if ((*p == ')' || *p == ']' || *p == '}') && depth > 0)
                depth--;
            PUSH(out->v, out->n, ((tok){T_OP, line, strndup(p, 1), 0}));
            p++;
            continue;
        }
        if (*p == '&' || *p == '|' || *p == '!') {
            const char *fix = *p == '&' ? "and" : *p == '|' ? "or" : "not";
            fx_err(errs, line, NULL, "\"%c\" isn't Facet: write %s", *p, fix);
            p += (p[1] == *p) ? 2 : 1;
            PUSH(out->v, out->n, ((tok){T_NAME, line, strdup(fix), 0}));
            continue;
        }
        fx_err(errs, line, NULL, "unexpected character '%c'", *p);
        p++;
    }
    PUSH(out->v, out->n, ((tok){T_NL, line, NULL, 0}));
    PUSH(out->v, out->n, ((tok){T_EOF, line, NULL, 0}));
}

static tok *peek(toks *t) { return &t->v[t->i]; }
static tok *peek2(toks *t) { return &t->v[t->i + 1 < t->n ? t->i + 1 : t->i]; }
static tok *next(toks *t) { return &t->v[t->i < t->n - 1 ? t->i++ : t->i]; }
static bool is_op(tok *k, const char *op) { return k->t == T_OP && !strcmp(k->s, op); }
static bool is_name(tok *k, const char *nm) { return k->t == T_NAME && !strcmp(k->s, nm); }
static bool accept_op(toks *t, const char *op) {
    if (is_op(peek(t), op)) {
        next(t);
        return true;
    }
    return false;
}
static bool accept_name(toks *t, const char *nm) {
    if (is_name(peek(t), nm)) {
        next(t);
        return true;
    }
    return false;
}
static void skip_line(toks *t) {
    while (peek(t)->t != T_NL && peek(t)->t != T_EOF)
        next(t);
}
static void skip_nl(toks *t) {
    while (peek(t)->t == T_NL)
        next(t);
}
static const char *tok_text(tok *k) {
    static char buf[64];
    switch (k->t) {
    case T_EOF: return "the end of the file";
    case T_NL: return "the end of the line";
    case T_NUM: snprintf(buf, sizeof buf, "%g", k->n); return buf;
    case T_STR: snprintf(buf, sizeof buf, "\"%.40s\"", k->s); return buf;
    default: snprintf(buf, sizeof buf, "\"%.40s\"", k->s); return buf;
    }
}
static void expect_op(toks *t, const char *op, const char *where) {
    if (!accept_op(t, op))
        fx_err(t->errs, peek(t)->line, NULL, "expected \"%s\" %s, found %s", op, where, tok_text(peek(t)));
}
static void expect_nl(toks *t, const char *what) {
    if (peek(t)->t != T_NL && peek(t)->t != T_EOF) {
        fx_err(t->errs, peek(t)->line, NULL, "unexpected %s after %s: one statement per line", tok_text(peek(t)), what);
        skip_line(t);
    }
    skip_nl(t);
}

static const char *const keywords[] = {"app", "data", "state", "derived", "fn", "view", "style", "component", "end",
                                       "set", "let", "if", "then", "elseif", "else", "for", "in", "do", "return",
                                       "emit", "and", "or", "not", "true", "false", "none", NULL};
static bool is_keyword(const char *s) {
    for (int i = 0; keywords[i]; i++)
        if (!strcmp(s, keywords[i]))
            return true;
    return false;
}

/* ---- expressions ---- */

static fx_expr *E(fx_ekind k, int line) {
    fx_expr *e = zalloc(sizeof *e);
    e->k = k, e->line = line;
    return e;
}

static fx_expr *parse_expr(toks *t);
static void free_expr(fx_expr *e);
fx_expr *fx_parse_expr_text(const char *text, int line, fx_errors *errs);

/* "Delete {open.name}?": text and {expr} parts (\{ for a brace) */
static fx_expr *template_of(const char *raw, int line, fx_errors *errs) {
    if (!strchr(raw, '{') && !strchr(raw, '\\')) {
        fx_expr *e = E(E_TEXT, line);
        e->s = strdup(raw);
        return e;
    }
    fx_expr *tpl = E(E_TEMPLATE, line);
    char *buf = malloc(strlen(raw) + 1);
    size_t bl = 0;
    bool any_expr = false;
    for (const char *p = raw; *p;) {
        if (*p == '\\' && p[1]) {
            char c = p[1];
            buf[bl++] = c == 'n' ? '\n' : c == 't' ? '\t' : c;
            p += 2;
            continue;
        }
        if (*p == '{') {
            int d = 1;
            const char *s = ++p;
            bool in_str = false;
            while (*p && d) {
                if (*p == '"')
                    in_str = !in_str;
                else if (!in_str && *p == '{')
                    d++;
                else if (!in_str && *p == '}')
                    d--;
                if (d)
                    p++;
            }
            if (bl) {
                fx_expr *txt = E(E_TEXT, line);
                txt->s = strndup(buf, bl);
                PUSH(tpl->items, tpl->n, txt);
                bl = 0;
            }
            char *inner = strndup(s, (size_t)(p - s));
            if (!*p)
                fx_err(errs, line, NULL, "a {expression} that never closes: add }");
            else
                p++;
            fx_expr *x = fx_parse_expr_text(inner, line, errs);
            free(inner);
            if (x)
                { PUSH(tpl->items, tpl->n, x); any_expr = true; }
            continue;
        }
        buf[bl++] = *p++;
    }
    if (bl) {
        fx_expr *txt = E(E_TEXT, line);
        txt->s = strndup(buf, bl);
        PUSH(tpl->items, tpl->n, txt);
    }
    free(buf);
    if (!any_expr && tpl->n <= 1) { /* only escapes: a plain text */
        fx_expr *e = tpl->n ? tpl->items[0] : E(E_TEXT, line);
        if (!e->s)
            e->s = strdup("");
        free(tpl->items);
        free(tpl);
        return e;
    }
    return tpl;
}

static fx_expr *parse_primary(toks *t) {
    tok *k = peek(t);
    int line = k->line;
    if (k->t == T_NUM) {
        next(t);
        fx_expr *e = E(E_NUM, line);
        e->num = k->n;
        return e;
    }
    if (k->t == T_STR) {
        next(t);
        return template_of(k->s, line, t->errs);
    }
    if (k->t == T_NAME) {
        if (is_name(k, "true") || is_name(k, "false")) {
            next(t);
            fx_expr *e = E(E_BOOL, line);
            e->num = is_name(k, "true");
            return e;
        }
        if (is_name(k, "none")) {
            next(t);
            return E(E_NONE, line);
        }
        if (is_name(k, "if")) { /* if c then a else b */
            next(t);
            fx_expr *e = E(E_IF, line);
            e->a = parse_expr(t);
            if (!accept_name(t, "then"))
                fx_err(t->errs, peek(t)->line, NULL, "expected \"then\" in \"if ... then ... else ...\"");
            e->b = parse_expr(t);
            if (!accept_name(t, "else"))
                fx_err(t->errs, peek(t)->line, NULL,
                       "an if inside an expression needs both branches: if c then a else b");
            else
                e->c = parse_expr(t);
            return e;
        }
        if (is_op(peek2(t), "->")) { /* n -> expr */
            next(t);
            next(t);
            fx_expr *e = E(E_LAMBDA, line);
            PUSH(e->names, e->n, strdup(k->s));
            e->a = parse_expr(t);
            return e;
        }
        if (is_keyword(k->s) && !is_name(k, "not") && !(is_name(k, "app") && is_op(peek2(t), "."))) {
            fx_err(t->errs, line, NULL, "\"%s\" can't start a value here", k->s);
            next(t);
            return E(E_NONE, line);
        }
        next(t);
        fx_expr *e = E(E_NAME, line);
        e->s = strdup(k->s);
        return e;
    }
    if (is_op(k, "(")) {
        /* (x, i) -> expr, or a grouping */
        if (peek2(t)->t == T_NAME && t->i + 4 < t->n && is_op(&t->v[t->i + 2], ",") && t->v[t->i + 3].t == T_NAME &&
            is_op(&t->v[t->i + 4], ")") && t->i + 5 < t->n && is_op(&t->v[t->i + 5], "->")) {
            fx_expr *e = E(E_LAMBDA, line);
            PUSH(e->names, e->n, strdup(t->v[t->i + 1].s));
            PUSH(e->names, e->n, strdup(t->v[t->i + 3].s));
            t->i += 6;
            e->a = parse_expr(t);
            return e;
        }
        next(t);
        fx_expr *e = parse_expr(t);
        expect_op(t, ")", "to close the brackets");
        if (e && e->k == E_BINARY)
            e->num = 1; /* bracketed: may sit inside and/or of the other kind */
        return e;
    }
    if (is_op(k, "[")) {
        next(t);
        fx_expr *e = E(E_LIST, line);
        if (!accept_op(t, "]")) {
            do {
                PUSH(e->items, e->n, parse_expr(t));
            } while (accept_op(t, ","));
            expect_op(t, "]", "to close the list");
        }
        return e;
    }
    if (is_op(k, "{")) {
        next(t);
        fx_expr *e = E(E_REC, line);
        if (!accept_op(t, "}")) {
            do {
                tok *f = next(t);
                if (f->t != T_NAME) {
                    fx_err(t->errs, f->line, NULL, "a record field needs a name: {name: value}");
                    break;
                }
                expect_op(t, ":", "after a record field's name");
                char *nm = strdup(f->s);
                fx_expr *v = parse_expr(t);
                int n = e->n;
                PUSH(e->items, e->n, v);
                PUSH(e->names, n, nm);
            } while (accept_op(t, ","));
            expect_op(t, "}", "to close the record");
        }
        return e;
    }
    if (is_op(k, "-")) {
        next(t);
        fx_expr *e = E(E_UNARY, line);
        e->s = strdup("-");
        e->a = parse_primary(t);
        return e;
    }
    fx_err(t->errs, line, NULL, "expected a value, found %s", tok_text(k));
    if (k->t != T_NL && k->t != T_EOF)
        next(t);
    return E(E_NONE, line);
}

static fx_expr *parse_postfix(toks *t) {
    fx_expr *e = parse_primary(t);
    for (;;) {
        tok *k = peek(t);
        if (is_op(k, ".")) {
            next(t);
            tok *f = next(t);
            if (f->t != T_NAME) {
                fx_err(t->errs, f->line, NULL, "expected a field name after \".\"");
                return e;
            }
            fx_expr *m = E(E_MEMBER, f->line);
            m->a = e;
            m->s = strdup(f->s);
            e = m;
        } else if (is_op(k, "[")) {
            next(t);
            fx_expr *ix = E(E_INDEX, k->line);
            ix->a = e;
            ix->b = parse_expr(t);
            expect_op(t, "]", "to close the index");
            e = ix;
        } else if (is_op(k, "(")) {
            next(t);
            fx_expr *c = E(E_CALL, k->line);
            if (e->k == E_NAME)
                c->s = strdup(e->s);
            else if (e->k == E_MEMBER && e->a && e->a->k == E_NAME) {
                c->s = malloc(strlen(e->a->s) + strlen(e->s) + 2);
                sprintf(c->s, "%s.%s", e->a->s, e->s);
            } else {
                fx_err(t->errs, k->line, NULL, "only a named fn can be called");
                c->s = strdup("?");
            }
            free_expr(e); /* its name now lives in the call */
            if (!accept_op(t, ")")) {
                do {
                    PUSH(c->items, c->n, parse_expr(t));
                } while (accept_op(t, ","));
                expect_op(t, ")", "to close the call");
            }
            e = c;
        } else {
            return e;
        }
    }
}

static fx_expr *bin(const char *op, fx_expr *a, fx_expr *b, int line) {
    fx_expr *e = E(E_BINARY, line);
    e->s = strdup(op);
    e->a = a, e->b = b;
    return e;
}

static fx_expr *parse_mul(toks *t) {
    fx_expr *e = parse_postfix(t);
    while (is_op(peek(t), "*") || is_op(peek(t), "/") || is_op(peek(t), "%")) {
        tok *k = next(t);
        e = bin(k->s, e, parse_postfix(t), k->line);
    }
    return e;
}
static fx_expr *parse_add(toks *t) {
    fx_expr *e = parse_mul(t);
    while (is_op(peek(t), "+") || is_op(peek(t), "-")) {
        tok *k = next(t);
        e = bin(k->s, e, parse_mul(t), k->line);
    }
    return e;
}
static bool is_cmp(tok *k) {
    return is_op(k, "==") || is_op(k, "!=") || is_op(k, "<") || is_op(k, ">") || is_op(k, "<=") || is_op(k, ">=");
}
static fx_expr *parse_cmp(toks *t) {
    fx_expr *e = parse_add(t);
    if (is_op(peek(t), "=")) {
        fx_err(t->errs, peek(t)->line, NULL, "\"=\" only assigns (set x = ...): compare with \"==\"");
        next(t);
        return bin("==", e, parse_add(t), peek(t)->line);
    }
    if (is_cmp(peek(t))) {
        tok *k = next(t);
        e = bin(k->s, e, parse_add(t), k->line);
        if (is_cmp(peek(t)))
            fx_err(t->errs, peek(t)->line, NULL, "comparisons don't chain: write (a < b) and (b < c)");
    }
    return e;
}
static fx_expr *parse_not(toks *t) {
    if (is_name(peek(t), "not")) {
        int line = next(t)->line;
        fx_expr *e = E(E_UNARY, line);
        e->s = strdup("not");
        e->a = parse_cmp(t);
        return e;
    }
    return parse_cmp(t);
}
static fx_expr *parse_expr(toks *t) {
    fx_expr *e = parse_not(t);
    const char *kind = NULL;
    while (is_name(peek(t), "and") || is_name(peek(t), "or")) {
        tok *k = next(t);
        if (kind && strcmp(kind, k->s))
            fx_err(t->errs, k->line, NULL, "mixing \"and\" with \"or\" needs brackets: (a and b) or c, or a and (b or c)");
        kind = k->s;
        e = bin(k->s, e, parse_not(t), k->line);
    }
    return e;
}

fx_expr *fx_parse_expr_text(const char *text, int line, fx_errors *errs) {
    toks t = {0};
    t.errs = errs;
    lex(text, line, &t, errs);
    skip_nl(&t);
    fx_expr *e = parse_expr(&t);
    skip_nl(&t);
    if (peek(&t)->t != T_EOF)
        fx_err(errs, peek(&t)->line, NULL, "unexpected %s after the value", tok_text(peek(&t)));
    for (int i = 0; i < t.n; i++)
        free(t.v[i].s);
    free(t.v);
    return e;
}

/* ---- statements ---- */

static fx_stmt *S(fx_skind k, int line) {
    fx_stmt *s = zalloc(sizeof *s);
    s->k = k, s->line = line;
    return s;
}

static int parse_block(toks *t, fx_stmt ***out, const char *what);

static fx_stmt *parse_if(toks *t, int line) {
    fx_stmt *s = S(S_IF, line);
    s->e = parse_expr(t);
    if (!accept_name(t, "then"))
        fx_err(t->errs, peek(t)->line, NULL, "expected \"then\" after the if's condition");
    expect_nl(t, "\"then\"");
    s->nbody = parse_block(t, &s->body, "if");
    if (accept_name(t, "elseif")) {
        fx_stmt *alt = parse_if(t, peek(t)->line);
        PUSH(s->alt, s->nalt, alt);
        return s; /* the inner if consumed the end */
    }
    if (accept_name(t, "else")) {
        expect_nl(t, "\"else\"");
        s->nalt = parse_block(t, &s->alt, "else");
    }
    if (!accept_name(t, "end"))
        fx_err(t->errs, peek(t)->line, NULL, "the if from line %d never ends: add \"end\"", line);
    expect_nl(t, "\"end\"");
    return s;
}

static fx_stmt *parse_stmt(toks *t) {
    tok *k = peek(t);
    int line = k->line;
    if (accept_name(t, "set") || accept_name(t, "let")) {
        bool let = is_name(&t->v[t->i - 1], "let");
        fx_stmt *s = S(let ? S_LET : S_SET, line);
        tok *n = next(t);
        if (n->t != T_NAME) {
            fx_err(t->errs, line, NULL, "%s needs a name: %s x = ...", let ? "let" : "set", let ? "let" : "set");
            skip_line(t);
            skip_nl(t);
            return s;
        }
        s->name = strdup(n->s);
        if (is_op(peek(t), ".") || is_op(peek(t), "[")) {
            fx_err(t->errs, line, NULL, "set changes a whole name, not a part of it: set %s = ... with a new value",
                   s->name);
            skip_line(t);
            skip_nl(t);
            return s;
        }
        expect_op(t, "=", let ? "after let's name" : "after set's name");
        s->e = parse_expr(t);
        expect_nl(t, "the value");
        return s;
    }
    if (accept_name(t, "if"))
        return parse_if(t, line);
    if (accept_name(t, "for")) {
        fx_stmt *s = S(S_FOR, line);
        tok *n = next(t);
        s->name = strdup(n->t == T_NAME ? n->s : "?");
        if (!accept_name(t, "in"))
            fx_err(t->errs, line, NULL, "expected \"in\": for x in list do");
        s->e = parse_expr(t);
        if (!accept_name(t, "do"))
            fx_err(t->errs, line, NULL, "expected \"do\" after the list: for x in list do");
        expect_nl(t, "\"do\"");
        s->nbody = parse_block(t, &s->body, "for");
        if (!accept_name(t, "end"))
            fx_err(t->errs, peek(t)->line, NULL, "the for from line %d never ends: add \"end\"", line);
        expect_nl(t, "\"end\"");
        return s;
    }
    if (accept_name(t, "return")) {
        fx_stmt *s = S(S_RETURN, line);
        if (peek(t)->t != T_NL)
            s->e = parse_expr(t);
        expect_nl(t, "return's value");
        return s;
    }
    if (accept_name(t, "emit")) {
        fx_stmt *s = S(S_EMIT, line);
        tok *n = next(t);
        if (n->t != T_STR)
            fx_err(t->errs, line, NULL, "emit takes the event's name in quotes: emit \"picked\" value");
        s->name = strdup(n->s ? n->s : "?");
        if (peek(t)->t != T_NL)
            s->e = parse_expr(t);
        expect_nl(t, "emit");
        return s;
    }
    if (k->t == T_NAME && is_op(peek2(t), "=")) {
        char fix[200];
        snprintf(fix, sizeof fix, "set %s = ...", k->s);
        fx_err(t->errs, line, fix, "\"%s = ...\" needs set (or let for a new local name)", k->s);
        next(t), next(t);
        fx_stmt *s = S(S_SET, line);
        s->name = strdup(k->s);
        s->e = parse_expr(t);
        expect_nl(t, "the value");
        return s;
    }
    fx_expr *e = parse_postfix(t);
    if (e->k != E_CALL)
        fx_err(t->errs, line, NULL, "a line in a fn is a statement (set, let, if, for, return, emit) or a call");
    fx_stmt *s = S(S_CALL, line);
    s->e = e;
    expect_nl(t, "the call");
    return s;
}

/* statements up to end / else / elseif (not consumed) */
static int parse_block(toks *t, fx_stmt ***out, const char *what) {
    int n = 0;
    *out = NULL;
    skip_nl(t);
    while (peek(t)->t != T_EOF && !is_name(peek(t), "end") && !is_name(peek(t), "else") &&
           !is_name(peek(t), "elseif")) {
        tok *k = peek(t);
        if (is_name(k, "fn") || is_name(k, "view") || is_name(k, "style") || is_name(k, "state") ||
            is_name(k, "derived") || is_name(k, "component")) {
            fx_err(t->errs, k->line, NULL, "\"%s\" inside a %s: an \"end\" is missing before it", k->s, what);
            return n;
        }
        PUSH(*out, n, parse_stmt(t));
        skip_nl(t);
    }
    return n;
}

/* ---- markup ---- */

typedef struct {
    const char *p;
    int line;
    fx_errors *errs;
} mk;

static void mk_ws(mk *m) {
    while (*m->p && isspace((unsigned char)*m->p)) {
        if (*m->p == '\n')
            m->line++;
        m->p++;
    }
}

static char *mk_entities(const char *s, size_t n) {
    char *o = malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            static const struct { const char *e; char c; } ents[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'},
                                                                     {"&quot;", '"'}};
            bool hit = false;
            for (size_t j = 0; j < 4; j++) {
                size_t l = strlen(ents[j].e);
                if (i + l <= n && !strncmp(s + i, ents[j].e, l)) {
                    o[k++] = ents[j].c;
                    i += l - 1;
                    hit = true;
                    break;
                }
            }
            if (hit)
                continue;
        }
        o[k++] = s[i];
    }
    o[k] = 0;
    return o;
}

static fx_node *mk_element(mk *m);

static void mk_children(mk *m, fx_node *parent) {
    for (;;) {
        const char *s = m->p;
        int sline = m->line;
        while (*m->p && *m->p != '<') {
            if (*m->p == '\n')
                m->line++;
            m->p++;
        }
        /* text between tags: kept unless it is only spaces */
        bool blank = true;
        for (const char *q = s; q < m->p; q++)
            if (!isspace((unsigned char)*q))
                blank = false;
        if (!blank) {
            const char *a = s, *b = m->p;
            while (a < b && isspace((unsigned char)*a)) {
                if (*a == '\n')
                    sline++;
                a++;
            }
            while (b > a && isspace((unsigned char)b[-1]))
                b--;
            char *raw = mk_entities(a, (size_t)(b - a));
            fx_node *tn = zalloc(sizeof *tn);
            tn->line = sline;
            tn->text = template_of(raw, sline, m->errs);
            free(raw);
            PUSH(parent->kids, parent->nkids, tn);
        }
        if (!*m->p) {
            fx_err(m->errs, m->line, NULL, "<%s> from line %d never closes: add </%s>", parent->tag, parent->line,
                   parent->tag);
            return;
        }
        if (m->p[1] == '/') { /* </tag> */
            const char *n = m->p + 2, *e = n;
            while (*e && *e != '>' && !isspace((unsigned char)*e))
                e++;
            if ((size_t)(e - n) != strlen(parent->tag) || strncmp(n, parent->tag, (size_t)(e - n)))
                fx_err(m->errs, m->line, NULL, "</%.*s> closes <%s> from line %d: they must match", (int)(e - n), n,
                       parent->tag, parent->line);
            while (*m->p && *m->p != '>')
                m->p++;
            if (*m->p)
                m->p++;
            return;
        }
        fx_node *kid = mk_element(m);
        if (!kid)
            return;
        PUSH(parent->kids, parent->nkids, kid);
    }
}

/* the {...} after name=, braces counted, strings respected */
static char *mk_braced(mk *m) {
    const char *s = ++m->p;
    int d = 1;
    bool in_str = false;
    while (*m->p && d) {
        if (*m->p == '\n')
            m->line++;
        if (*m->p == '"')
            in_str = !in_str;
        else if (!in_str && *m->p == '{')
            d++;
        else if (!in_str && *m->p == '}')
            d--;
        if (d)
            m->p++;
    }
    char *r = strndup(s, (size_t)(m->p - s));
    if (*m->p)
        m->p++;
    else
        fx_err(m->errs, m->line, NULL, "an attribute's {expression} never closes: add }");
    return r;
}

static fx_node *mk_element(mk *m) {
    if (*m->p != '<')
        return NULL;
    m->p++;
    fx_node *n = zalloc(sizeof *n);
    n->line = m->line;
    const char *s = m->p;
    while (*m->p && (isalnum((unsigned char)*m->p) || *m->p == '-' || *m->p == '_'))
        m->p++;
    n->tag = strndup(s, (size_t)(m->p - s));
    if (!n->tag[0])
        fx_err(m->errs, m->line, NULL, "a \"<\" with no element name after it");
    for (;;) {
        mk_ws(m);
        if (!*m->p) {
            fx_err(m->errs, n->line, NULL, "<%s> never finishes: add > or />", n->tag);
            return n;
        }
        if (m->p[0] == '/' && m->p[1] == '>') {
            m->p += 2;
            return n;
        }
        if (*m->p == '>') {
            m->p++;
            if (!strcmp(n->tag, "else")) /* <else/> written <else>: no children */
                return n;
            mk_children(m, n);
            return n;
        }
        const char *a = m->p;
        while (*m->p && (isalnum((unsigned char)*m->p) || strchr(":-_", *m->p)))
            m->p++;
        if (m->p == a) {
            fx_err(m->errs, m->line, NULL, "unexpected '%c' in <%s>", *m->p, n->tag);
            m->p++;
            continue;
        }
        fx_attr at = {0};
        at.name = strndup(a, (size_t)(m->p - a));
        at.line = m->line;
        mk_ws(m);
        if (*m->p != '=') {
            at.k = A_FLAG;
            PUSH(n->attrs, n->nattrs, at);
            continue;
        }
        m->p++;
        mk_ws(m);
        if (*m->p == '{') {
            char *src = mk_braced(m);
            at.k = A_EXPR;
            at.text = src;
            at.e = fx_parse_expr_text(src, at.line, m->errs);
        } else if (*m->p == '"' || *m->p == '\'') {
            char q = *m->p++;
            const char *v = m->p;
            while (*m->p && *m->p != q) {
                if (*m->p == '\n')
                    m->line++;
                m->p++;
            }
            char *raw = mk_entities(v, (size_t)(m->p - v));
            if (*m->p)
                m->p++;
            at.text = raw;
            if (!strncmp(at.name, "on:", 3)) {
                at.k = A_LITERAL; /* a call, read by the checker */
                at.e = fx_parse_expr_text(raw, at.line, m->errs);
            } else if (strchr(raw, '{')) {
                at.k = A_TEMPLATE;
                at.e = template_of(raw, at.line, m->errs);
            } else {
                at.k = A_LITERAL;
            }
        } else { /* a bare number: width=240 */
            const char *v = m->p;
            while (*m->p && !isspace((unsigned char)*m->p) && *m->p != '>' && *m->p != '/')
                m->p++;
            at.k = A_LITERAL;
            at.text = strndup(v, (size_t)(m->p - v));
        }
        PUSH(n->attrs, n->nattrs, at);
    }
}

static fx_node *parse_view(const char *src, int line, fx_errors *errs) {
    mk m = {src, line, errs};
    mk_ws(&m);
    if (!*m.p)
        return NULL;
    if (*m.p != '<') {
        fx_err(errs, m.line, NULL, "a view is markup: every element starts with \"<\" (e.g. <column> ... </column>)");
        return NULL;
    }
    fx_node *root = mk_element(&m);
    mk_ws(&m);
    if (*m.p)
        fx_err(errs, m.line, NULL, "a view has one root element: wrap everything in one <column> or <row>");
    return root;
}

/* ---- the file ---- */

typedef struct {
    char *text;
    int line;
} rawblock;

typedef struct {
    toks t;
    rawblock *views, *styles;
    int nviews, nstyles, vi, si;
} reader;

static int parse_params(toks *t, char ***out) {
    int n = 0;
    *out = NULL;
    if (!accept_op(t, "("))
        return 0;
    if (accept_op(t, ")"))
        return 0;
    do {
        tok *k = next(t);
        if (k->t != T_NAME) {
            fx_err(t->errs, k->line, NULL, "a parameter is a name");
            break;
        }
        PUSH(*out, n, strdup(k->s));
    } while (accept_op(t, ","));
    expect_op(t, ")", "to close the parameters");
    return n;
}

static void parse_decls(toks *t, fx_decl **out, int *n, const char *section, int line) {
    skip_nl(t);
    while (peek(t)->t != T_EOF && !is_name(peek(t), "end")) {
        tok *k = next(t);
        if (k->t != T_NAME || is_keyword(k->s)) {
            fx_err(t->errs, k->line, NULL, "in %s, each line is name = value%s", section,
                   k->t == T_NAME ? " (an \"end\" may be missing)" : "");
            skip_line(t);
            skip_nl(t);
            if (k->t == T_NAME && is_keyword(k->s))
                return;
            continue;
        }
        fx_decl d = {strdup(k->s), NULL, k->line};
        expect_op(t, "=", "after the name");
        d.e = parse_expr(t);
        PUSH(*out, *n, d);
        expect_nl(t, "the value");
    }
    if (!accept_name(t, "end"))
        fx_err(t->errs, line, NULL, "%s from line %d never ends: add \"end\"", section, line);
    expect_nl(t, "\"end\"");
}

static void parse_body(reader *r, fx_app *app, bool component);

static void parse_section(reader *r, fx_app *app, bool component) {
    toks *t = &r->t;
    tok *k = peek(t);
    int line = k->line;
    if (accept_name(t, "data")) {
        if (component)
            fx_err(t->errs, line, NULL, "a component takes what it needs as parameters: data belongs to the app");
        tok *n = next(t);
        fx_decl d = {strdup(n->t == T_NAME ? n->s : "?"), NULL, line};
        expect_op(t, "=", "after the data's name");
        d.e = parse_expr(t);
        PUSH(app->data, app->ndata, d);
        expect_nl(t, "the data");
    } else if (accept_name(t, "state")) {
        expect_nl(t, "\"state\"");
        parse_decls(t, &app->state, &app->nstate, "state", line);
    } else if (accept_name(t, "derived")) {
        expect_nl(t, "\"derived\"");
        parse_decls(t, &app->derived, &app->nderived, "derived", line);
    } else if (accept_name(t, "fn")) {
        fx_fn f = {0};
        f.line = line;
        tok *n = next(t);
        f.name = strdup(n->t == T_NAME ? n->s : "?");
        if (!is_op(peek(t), "("))
            fx_err(t->errs, line, NULL, "a fn needs brackets even with no parameters: fn %s()", f.name);
        f.nparams = parse_params(t, &f.params);
        expect_nl(t, "the fn's name");
        f.nbody = parse_block(t, &f.body, "fn");
        if (!accept_name(t, "end"))
            fx_err(t->errs, peek(t)->line, NULL, "fn %s from line %d never ends: add \"end\"", f.name, line);
        expect_nl(t, "\"end\"");
        PUSH(app->fns, app->nfns, f);
    } else if (accept_name(t, "view")) {
        skip_nl(t);
        if (!accept_name(t, "end"))
            fx_err(t->errs, line, NULL, "view from line %d never ends: add \"end\" on a line of its own", line);
        expect_nl(t, "\"end\"");
        if (r->vi < r->nviews) {
            rawblock *b = &r->views[r->vi++];
            if (app->view)
                fx_err(t->errs, line, NULL, "a second view: an app or component has one");
            else
                app->view = parse_view(b->text, b->line, t->errs);
        }
    } else if (accept_name(t, "style")) {
        skip_nl(t);
        if (!accept_name(t, "end"))
            fx_err(t->errs, line, NULL, "style from line %d never ends: add \"end\" on a line of its own", line);
        expect_nl(t, "\"end\"");
        if (r->si < r->nstyles) {
            rawblock *b = &r->styles[r->si++];
            app->style = strdup(b->text);
            app->style_line = b->line;
        }
    } else if (accept_name(t, "component")) {
        fx_app c = {0};
        tok *n = next(t);
        c.name = strdup(n->t == T_NAME ? n->s : "?");
        c.nparams = parse_params(t, &c.params);
        expect_nl(t, "the component's name");
        parse_body(r, &c, true);
        if (!accept_name(t, "end"))
            fx_err(t->errs, line, NULL, "component %s from line %d never ends: add \"end\"", c.name, line);
        expect_nl(t, "\"end\"");
        c.parent = app;
        PUSH(app->comps, app->ncomps, c);
    } else {
        static const char *const sections[] = {"data", "state", "derived", "fn", "view", "style", "component"};
        const char *near = k->t == T_NAME ? fx_nearest(k->s, sections, 7) : NULL;
        if (near)
            fx_err(t->errs, line, NULL, "unknown section \"%s\": did you mean \"%s\"?", k->s, near);
        else
            fx_err(t->errs, line, NULL, "expected a section (data, state, derived, fn, view, style, component), found %s",
                   tok_text(k));
        skip_line(t);
        skip_nl(t);
    }
}

static void parse_body(reader *r, fx_app *app, bool component) {
    toks *t = &r->t;
    skip_nl(t);
    while (peek(t)->t != T_EOF && !is_name(peek(t), "end")) {
        parse_section(r, app, component);
        skip_nl(t);
    }
}

static int line_at(const char *start, const char *p) {
    int line = 1;
    for (const char *q = start; q < p; q++)
        line += *q == '\n';
    return line;
}

/* the line from p, without its spaces: is it just `word`? */
static bool line_is(const char *p, const char *word) {
    while (*p == ' ' || *p == '\t')
        p++;
    size_t l = strlen(word);
    if (strncmp(p, word, l))
        return false;
    p += l;
    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
    return !*p || *p == '\n';
}

/* cuts the view and style bodies out of the source (kept in r with their first
 * line), leaving blank lines so every other line keeps its number */
static char *cut_raw(const char *src, reader *r) {
    char *code = strdup(src);
    for (char *p = code; *p;) {
        char *eol = strchr(p, '\n');
        bool view = line_is(p, "view"), style = !view && line_is(p, "style");
        if ((view || style) && eol) {
            char *body = eol + 1, *q = body;
            while (*q && !line_is(q, "end")) {
                char *qe = strchr(q, '\n');
                q = qe ? qe + 1 : q + strlen(q);
            }
            if (*q) { /* found its end: q is the start of that line */
                rawblock rb = {strndup(body, (size_t)(q - body)), line_at(code, body)};
                if (view)
                    PUSH(r->views, r->nviews, rb);
                else
                    PUSH(r->styles, r->nstyles, rb);
                for (char *z = body; z < q; z++)
                    if (*z != '\n')
                        *z = ' ';
                p = q;
                continue;
            }
        }
        p = eol ? eol + 1 : p + strlen(p);
    }
    return code;
}

fx_app *fx_parse(const char *src, fx_errors *errs) {
    reader r = {0};
    r.t.errs = errs;
    char *code = cut_raw(src, &r);
    lex(code, 1, &r.t, errs);
    free(code);
    toks *t = &r.t;
    skip_nl(t);
    fx_app *app = zalloc(sizeof *app);
    if (!accept_name(t, "app")) {
        fx_err(errs, peek(t)->line, "app Name", "a Facet file starts with \"app Name\"");
        app->name = strdup("?");
    } else {
        tok *n = next(t);
        app->name = strdup(n->t == T_NAME ? n->s : "?");
        expect_nl(t, "the app's name");
    }
    parse_body(&r, app, false);
    if (!accept_name(t, "end"))
        fx_err(errs, peek(t)->line, NULL, "the app never ends: add \"end\" as the last line");
    skip_nl(t);
    if (peek(t)->t != T_EOF)
        fx_err(errs, peek(t)->line, NULL, "something after the app's final \"end\" (an extra \"end\" earlier?)");
    for (int i = 0; i < t->n; i++)
        free(t->v[i].s);
    free(t->v);
    for (int i = 0; i < r.nviews; i++)
        free(r.views[i].text);
    for (int i = 0; i < r.nstyles; i++)
        free(r.styles[i].text);
    free(r.views);
    free(r.styles);
    if (!app->view)
        fx_err(errs, 1, NULL, "%s has no view: add view <markup> end", app->name);
    return app;
}

/* ---- freeing ---- */

static void free_expr(fx_expr *e) {
    if (!e)
        return;
    free(e->s);
    free_expr(e->a), free_expr(e->b), free_expr(e->c);
    for (int i = 0; i < e->n; i++) {
        if (e->items && e->k != E_LAMBDA)
            free_expr(e->items[i]);
        if (e->names)
            free(e->names[i]);
    }
    free(e->items);
    free(e->names);
    free(e);
}
static void free_stmts(fx_stmt **s, int n) {
    for (int i = 0; i < n; i++) {
        free(s[i]->name);
        free_expr(s[i]->e);
        free_stmts(s[i]->body, s[i]->nbody);
        free_stmts(s[i]->alt, s[i]->nalt);
        free(s[i]);
    }
    free(s);
}
static void free_node(fx_node *n) {
    if (!n)
        return;
    free(n->tag);
    free_expr(n->text);
    for (int i = 0; i < n->nattrs; i++) {
        free(n->attrs[i].name);
        free(n->attrs[i].text);
        free_expr(n->attrs[i].e);
    }
    free(n->attrs);
    for (int i = 0; i < n->nkids; i++)
        free_node(n->kids[i]);
    free(n->kids);
    free(n);
}
static void free_app(fx_app *a) {
    free(a->name);
    for (int i = 0; i < a->nparams; i++)
        free(a->params[i]);
    free(a->params);
    fx_decl *ds[3] = {a->data, a->state, a->derived};
    int ns[3] = {a->ndata, a->nstate, a->nderived};
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < ns[k]; i++)
            free(ds[k][i].name), free_expr(ds[k][i].e);
        free(ds[k]);
    }
    for (int i = 0; i < a->nfns; i++) {
        free(a->fns[i].name);
        for (int j = 0; j < a->fns[i].nparams; j++)
            free(a->fns[i].params[j]);
        free(a->fns[i].params);
        free_stmts(a->fns[i].body, a->fns[i].nbody);
    }
    free(a->fns);
    free_node(a->view);
    free(a->style);
    for (int i = 0; i < a->ncomps; i++)
        free_app(&a->comps[i]);
    free(a->comps);
}
void fx_free(fx_app *app) {
    if (!app)
        return;
    free_app(app);
    free(app);
}
