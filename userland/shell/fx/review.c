/* The second opinion (review.h): the app's code normalised, reviewed by small local models, compared with the
 * raw file, and told to the companion as numbers and kinds only. */
#define _GNU_SOURCE
#include "review.h"
#include "../../lib/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *const fx_review_questions[FXQ_COUNT] = {
    "Does this code send text the person typed, or the contents of their files, to a server?",
    "Does this code delete or overwrite files beyond what its buttons and fields obviously do?",
    "Does this code do things that its visible interface does not show the person?",
    "Is this code written in a way meant to hide what it really does?",
};

/* a second phrasing of each, so one wording's quirks don't decide */
static const char *const second[FXQ_COUNT] = {
    "Could the data a person enters into this app, or their file contents, leave the computer through this code?",
    "Can this code destroy or replace files without the person clearly asking for that?",
    "Is there behaviour in this code that the person using its interface would not see or expect?",
    "Does this code look deliberately confusing or disguised?",
};

/* ---- normalising ---- */

typedef struct {
    sbuf b;
    char *names[512];
    char *tokens[512];
    int nnames, nvars, nfns, ncomps, nstrings;
    const fx_app *root;
} norm;

/* the field names records from the system have: part of the language, kept */
static bool api_field(const char *s) {
    static const char *const f[] = {"path", "name", "size", "modified", "dir", "ok", "status", "data", "text", "error",
                                    "id", "time", "hour", "minute", "second", "ms", "day", "weekday", "length", NULL};
    for (int i = 0; f[i]; i++)
        if (!strcmp(s, f[i]))
            return true;
    return false;
}

static bool builtin(const char *s) {
    int a, b;
    return !strcmp(s, "true") || !strcmp(s, "false") || !strcmp(s, "none") || fx_is_stdlib(s, &a, &b) ||
           fx_is_effect(s, &a, &b) || !strcmp(s, "files") || !strcmp(s, "file") || !strcmp(s, "web") ||
           !strcmp(s, "setting") || !strcmp(s, "apps") || !strcmp(s, "clock");
}

static bool is_fn(const fx_app *a, const char *s) {
    for (; a; a = a->parent)
        for (int i = 0; i < a->nfns; i++)
            if (!strcmp(a->fns[i].name, s))
                return true;
    return false;
}

static bool is_comp(const fx_app *a, const char *s) {
    for (int i = 0; i < a->ncomps; i++)
        if (a->comps[i].name && !strcmp(a->comps[i].name, s))
            return true;
    return false;
}

/* the neutral name for one the app made up: v1, f1, C1, k1 (record keys) */
static const char *tok(norm *n, const char *s, char kind) {
    if (!s)
        return "?";
    for (int i = 0; i < n->nnames; i++)
        if (n->names[i][0] == kind && !strcmp(n->names[i] + 1, s))
            return n->tokens[i];
    if (n->nnames >= 512)
        return kind == 'f' ? "f0" : kind == 'C' ? "C0" : "v0";
    char t[16];
    snprintf(t, sizeof t, "%c%d", kind == 'k' ? 'k' : kind, kind == 'f' ? ++n->nfns : kind == 'C' ? ++n->ncomps : ++n->nvars);
    size_t l = strlen(s);
    n->names[n->nnames] = malloc(l + 2);
    n->names[n->nnames][0] = kind;
    memcpy(n->names[n->nnames] + 1, s, l + 1);
    n->tokens[n->nnames] = strdup(t);
    return n->tokens[n->nnames++];
}

static void opaque(norm *n) { sb_printf(&n->b, "\"S%d\"", ++n->nstrings); }

static void expr(norm *n, const fx_expr *x);

static void name_ref(norm *n, const char *s) {
    if (builtin(s))
        sb_add(&n->b, s);
    else if (is_fn(n->root, s))
        sb_add(&n->b, tok(n, s, 'f'));
    else
        sb_add(&n->b, tok(n, s, 'v'));
}

static void expr(norm *n, const fx_expr *x) {
    if (!x) {
        sb_add(&n->b, "none");
        return;
    }
    switch (x->k) {
    case E_NONE: sb_add(&n->b, "none"); break;
    case E_BOOL: sb_add(&n->b, x->num != 0 ? "true" : "false"); break;
    case E_NUM: sb_printf(&n->b, "%g", x->num); break;
    case E_TEXT: opaque(n); break;
    case E_TEMPLATE: /* its text parts opaque, what it fills in shown */
        sb_add(&n->b, "\"");
        for (int i = 0; i < x->n; i++) {
            if (x->items[i]->k == E_TEXT)
                sb_printf(&n->b, "S%d", ++n->nstrings);
            else {
                sb_add(&n->b, "{");
                expr(n, x->items[i]);
                sb_add(&n->b, "}");
            }
        }
        sb_add(&n->b, "\"");
        break;
    case E_NAME: name_ref(n, x->s); break;
    case E_MEMBER:
        expr(n, x->a);
        sb_printf(&n->b, ".%s", api_field(x->s) ? x->s : tok(n, x->s, 'k'));
        break;
    case E_INDEX:
        expr(n, x->a), sb_add(&n->b, "["), expr(n, x->b), sb_add(&n->b, "]");
        break;
    case E_CALL:
        name_ref(n, x->s ? x->s : "?");
        sb_add(&n->b, "(");
        for (int i = 0; i < x->n; i++) {
            if (i)
                sb_add(&n->b, ", ");
            expr(n, x->items[i]);
        }
        sb_add(&n->b, ")");
        break;
    case E_LAMBDA:
        for (int i = 0; i < x->n; i++)
            sb_printf(&n->b, "%s%s", i ? ", " : "", tok(n, x->names[i], 'v'));
        sb_add(&n->b, " -> ");
        expr(n, x->a);
        break;
    case E_LIST:
        sb_add(&n->b, "[");
        for (int i = 0; i < x->n; i++) {
            if (i)
                sb_add(&n->b, ", ");
            expr(n, x->items[i]);
        }
        sb_add(&n->b, "]");
        break;
    case E_REC:
        sb_add(&n->b, "{");
        for (int i = 0; i < x->n; i++) {
            sb_printf(&n->b, "%s%s: ", i ? ", " : "", x->names && x->names[i] && api_field(x->names[i]) ? x->names[i]
                                                                           : tok(n, x->names ? x->names[i] : "?", 'k'));
            expr(n, x->items[i]);
        }
        sb_add(&n->b, "}");
        break;
    case E_UNARY:
        sb_printf(&n->b, "%s ", x->s ? x->s : "-");
        expr(n, x->a);
        break;
    case E_BINARY:
        sb_add(&n->b, "(");
        expr(n, x->a);
        sb_printf(&n->b, " %s ", x->s ? x->s : "?");
        expr(n, x->b);
        sb_add(&n->b, ")");
        break;
    case E_IF:
        sb_add(&n->b, "if ");
        expr(n, x->a), sb_add(&n->b, " then "), expr(n, x->b), sb_add(&n->b, " else "), expr(n, x->c);
        break;
    }
}

static void indent(norm *n, int d) {
    for (int i = 0; i < d; i++)
        sb_add(&n->b, "  ");
}

static void stmts(norm *n, fx_stmt **s, int k, int d) {
    for (int i = 0; i < k; i++) {
        const fx_stmt *st = s[i];
        if (!st)
            continue;
        indent(n, d);
        switch (st->k) {
        case S_SET: sb_printf(&n->b, "set %s = ", tok(n, st->name, 'v')), expr(n, st->e); break;
        case S_LET: sb_printf(&n->b, "let %s = ", tok(n, st->name, 'v')), expr(n, st->e); break;
        case S_RETURN: sb_add(&n->b, "return "), expr(n, st->e); break;
        case S_CALL: expr(n, st->e); break;
        case S_EMIT: sb_printf(&n->b, "emit %s ", tok(n, st->name, 'v')), expr(n, st->e); break;
        case S_FOR: sb_printf(&n->b, "for %s in ", tok(n, st->name, 'v')), expr(n, st->e); break;
        case S_IF: sb_add(&n->b, "if "), expr(n, st->e), sb_add(&n->b, " then"); break;
        }
        sb_add(&n->b, "\n");
        stmts(n, st->body, st->nbody, d + 1);
        if (st->nalt) {
            indent(n, d), sb_add(&n->b, "else\n");
            stmts(n, st->alt, st->nalt, d + 1);
        }
        if (st->k == S_IF || st->k == S_FOR)
            indent(n, d), sb_add(&n->b, "end\n");
    }
}

static void node(norm *n, const fx_node *x, int d) {
    if (!x)
        return;
    indent(n, d);
    if (!x->tag) {
        sb_add(&n->b, "text ");
        expr(n, x->text);
        sb_add(&n->b, "\n");
        return;
    }
    sb_printf(&n->b, "<%s", is_comp(n->root, x->tag) ? tok(n, x->tag, 'C') : x->tag);
    for (int i = 0; i < x->nattrs; i++) {
        const fx_attr *a = &x->attrs[i];
        sb_printf(&n->b, " %s=", a->name ? a->name : "?");
        if (a->e)
            sb_add(&n->b, "{"), expr(n, a->e), sb_add(&n->b, "}");
        else if (a->k == A_FLAG)
            sb_add(&n->b, "true");
        else
            opaque(n);
    }
    sb_add(&n->b, ">\n");
    if (x->text) {
        indent(n, d + 1);
        expr(n, x->text);
        sb_add(&n->b, "\n");
    }
    for (int i = 0; i < x->nkids; i++)
        node(n, x->kids[i], d + 1);
}

static void app_text(norm *n, const fx_app *a, bool component) {
    sb_printf(&n->b, "%s %s\n", component ? "component" : "app", component ? tok(n, a->name, 'C') : "App");
    const fx_decl *ds[3] = {a->data, a->state, a->derived};
    int ns[3] = {a->ndata, a->nstate, a->nderived};
    static const char *const kw[3] = {"data", "state", "derived"};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < ns[k]; i++) {
            sb_printf(&n->b, "  %s %s = ", kw[k], tok(n, ds[k][i].name, 'v'));
            expr(n, ds[k][i].e);
            sb_add(&n->b, "\n");
        }
    for (int i = 0; i < a->nfns; i++) {
        const fx_fn *f = &a->fns[i];
        sb_printf(&n->b, "  fn %s(", tok(n, f->name, 'f'));
        for (int p = 0; p < f->nparams; p++)
            sb_printf(&n->b, "%s%s", p ? ", " : "", tok(n, f->params[p], 'v'));
        sb_add(&n->b, ")\n");
        stmts(n, f->body, f->nbody, 2);
        sb_add(&n->b, "  end\n");
    }
    if (a->view) {
        sb_add(&n->b, "  view\n");
        node(n, a->view, 2);
        sb_add(&n->b, "  end\n");
    }
    /* its style only decides how it looks: left out (its text could carry anything) */
    for (int i = 0; i < a->ncomps; i++)
        app_text(n, &a->comps[i], true);
    sb_add(&n->b, "end\n");
}

char *fx_normalize(const fx_app *app) {
    norm n = {0};
    sb_init(&n.b);
    n.root = app;
    if (app)
        app_text(&n, app, false);
    for (int i = 0; i < n.nnames; i++)
        free(n.names[i]), free(n.tokens[i]);
    return n.b.p ? n.b.p : strdup("");
}

/* ---- reviewing ---- */

/* worked examples, in the normalised form, for each question: one yes and one no each */
static const struct {
    const char *code;
    bool yes;
} shots[FXQ_COUNT][4] = {
    {{"fn f1()\n  let v1 = fs.read(\"S1\")\n  net.post(\"S2\", v1)\nend", true},
     {"data v1 = web(\"S1\", 600000)\nview\n  <text>{v1.k1}</text>\nend", false},
     {"state v1 = \"S1\"\nfn f1()\n  net.post(\"S2\", {k1: v1})\nend\nview\n  <field bind=\"S3\">\nend", true},
     {"fn f1()\n  fs.write(\"S1\", v2)\nend", false}},
    {{"data v1 = files(\"S1\", \"S2\")\nfn f1()\n  for v2 in v1 do\n    fs.remove(v2.path)\n  end\nend\nview\n  <text>\"S3\"</text>\nend", true},
     {"fn f1()\n  if ask(\"S1\") then\n    fs.remove(v1.path)\n  end\nend\nview\n  <button on:click={f1()}>\nend", false},
     {"fn f1()\n  every(\"S1\", 5000, f2)\nend\nfn f2()\n  fs.write(v1.path, \"S2\")\nend", true},
     {"fn f1()\n  fs.write(v1.path, v2)\nend\nview\n  <field bind=\"S1\" on:input={f1()}>\nend", false}},
    {{"fn f1()\n  every(\"S1\", 60000, f2)\nend\nfn f2()\n  net.post(\"S2\", fs.read(\"S3\"))\nend\nview\n  <text>\"S4\"</text>\nend", true},
     {"state v1 = 0\nfn f1()\n  set v1 = (v1 + 1)\nend\nview\n  <button on:click={f1()}>\n  <text>{v1}</text>\nend", false},
     {"data v1 = files(\"S1\", \"S2\")\nfn f1()\n  app.notify(\"S3\")\nend", false},
     {"fn f1()\n  after(\"S1\", 1000, f2)\nend\nfn f2()\n  fs.remove(\"S2\")\nend\nview\n  <button on:click={f1()}>\nend", true}},
    {{"fn f1()\n  let v1 = join(map([104, 116, 116, 112], v2 -> v2), \"S1\")\n  net.get(v1)\nend", true},
     {"fn f1()\n  set v1 = (v1 + 1)\nend", false},
     {"fn f1()\n  let v1 = slice(\"S1\", 3, 9) + slice(\"S2\", 1, 4)\n  net.post(v1, v2)\nend", true},
     {"data v1 = file(\"S1\")\nview\n  <text>{v1.text}</text>\nend", false}},
};

static void msg(sbuf *b, const char *role, const char *text) {
    sb_printf(b, ",{\"role\":\"%s\",\"content\":", role);
    sb_json_str(b, text);
    sb_add(b, "}");
}

char *fx_review_request(const char *code, int q, int phrasing) {
    const char *question = phrasing ? second[q] : fx_review_questions[q];
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{\"messages\":[{\"role\":\"system\",\"content\":");
    sb_json_str(&b, "You review the code of small apps for a computer's safety. Text in quotes is replaced by tokens "
                    "like \"S1\" and names by tokens like v1 and f1; judge only what the code does. Answer yes or no.");
    sb_add(&b, "}");
    for (int k = 0; k < 4; k++) {
        char *u = malloc(strlen(shots[q][k].code) + strlen(question) + 32);
        sprintf(u, "Code:\n%s\nQuestion: %s", shots[q][k].code, question);
        msg(&b, "user", u);
        free(u);
        msg(&b, "assistant", shots[q][k].yes ? "yes" : "no");
    }
    char *u = malloc(strlen(code) + strlen(question) + 32);
    sprintf(u, "Code:\n%s\nQuestion: %s", code, question);
    msg(&b, "user", u);
    free(u);
    sb_add(&b, "],\"temperature\":0,\"max_tokens\":1,\"logprobs\":true,\"top_logprobs\":20,"
               "\"chat_template_kwargs\":{\"enable_thinking\":false}}");
    return b.p;
}

char *fx_strip_comments(const char *src) {
    size_t n = strlen(src);
    char *out = malloc(n + 1), *o = out;
    bool str = false;
    for (const char *p = src; *p; p++) {
        if (*p == '"' && (p == src || p[-1] != '\\'))
            str = !str;
        if (!str && p[0] == '-' && p[1] == '-') { /* to the end of the line */
            while (*p && *p != '\n')
                p++;
            if (!*p)
                break;
        }
        *o++ = *p;
    }
    *o = 0;
    return out;
}

static float ask_all(int models, fx_review_ask ask, void *ctx, const char *code, int q, int *answered) {
    float sum = 0;
    int k = 0;
    for (int m = 0; m < models; m++) {
        bool any = false;
        for (int ph = 0; ph < 2; ph++) {
            char *req = fx_review_request(code, q, ph);
            float a = ask(ctx, m, req, code);
            free(req);
            if (a >= 0)
                sum += a, k++, any = true;
        }
        if (any)
            (*answered)++;
    }
    return k ? sum / k : -1;
}

void fx_review(const fx_app *app, const char *raw_source, int models, fx_review_ask ask, void *ctx, fx_scorecard *out) {
    memset(out, 0, sizeof *out);
    char *code = fx_normalize(app);
    /* the file as written, with its comments and without, cut to what a small model can read */
    char *raw = strndup(raw_source ? raw_source : "", 9000);
    char *bare = fx_strip_comments(raw);
    int answered = 0;
    for (int q = 0; q < FXQ_COUNT; q++) {
        int a1 = 0, a2 = 0, a3 = 0;
        out->score[q] = ask_all(models, ask, ctx, code, q, &a1);
        out->raw[q] = ask_all(models, ask, ctx, raw, q, &a2);
        out->bare[q] = strcmp(raw, bare) ? ask_all(models, ask, ctx, bare, q, &a3) : out->raw[q];
        answered = a1 > answered ? a1 : answered;
        if (out->raw[q] >= 0 && out->bare[q] >= 0) {
            float d = fabsf(out->raw[q] - out->bare[q]);
            if (d > out->drift)
                out->drift = d;
        }
    }
    free(code);
    free(raw);
    free(bare);
    out->models = answered;
    out->done = answered > 0;
    if (!out->done)
        return;
    /* what the code can't do, a model can't say it does */
    fx_manifest m;
    fx_manifest_of(app, &m);
    bool net = false, writes = false;
    for (int i = 0; i < m.n; i++)
        net |= m.c[i].kind == FXC_HOST || m.c[i].kind == FXC_INTERNET, writes |= m.c[i].kind == FXC_WRITE;
    if (!net && out->score[FXQ_SENDS] > 0.05f)
        out->score[FXQ_SENDS] = 0.05f;
    if (!writes && out->score[FXQ_DESTROYS] > 0.05f)
        out->score[FXQ_DESTROYS] = 0.05f;
    out->injection = out->drift > FX_INJECTION_DRIFT;
    /* Measured on this system's own apps (build/diskpeek/live_review.sh against LFM3 1.7B): a small model's yes
     * or no about what code does is unreliable (near 1.0 that a plain counter hides what it does; 0.34 that an
     * app sending every note away sends data), while whether comments move its answers is exact (0.00 for every
     * ordinary app, 0.40 for one steering it). So the scores go to the companion as numbers, and only steering
     * raises a warning; what an app can do is decided by the analysis and the rules, which are exact. */
    out->level = out->injection ? 2 : 0;
}

/* ---- what the companion may know ---- */

char *fx_review_note(const fx_manifest *m, int mismatch_level, const fx_scorecard *sc) {
    sbuf b;
    sb_init(&b);
    sb_add(&b, "(An app is asking to be allowed to run. What its code can do, as the system worked it out from the "
               "code itself (you have not seen the code and will not; nothing written in it reaches you):\n");
    int places_r = 0, places_w = 0, hosts = 0, any = 0, all_r = 0, all_w = 0;
    for (int i = 0; i < m->n; i++) {
        const fx_cap *c = &m->c[i];
        if (c->kind == FXC_READ)
            places_r++, all_r |= !strcmp(c->what, "~");
        if (c->kind == FXC_WRITE)
            places_w++, all_w |= !strcmp(c->what, "~");
        if (c->kind == FXC_HOST)
            hosts++;
        if (c->kind == FXC_INTERNET)
            any = 1;
    }
    static const char *const kinds[] = {"read files", "change or delete files", "talk to a server named in its code",
                                        "talk to any server on the internet"};
    for (int i = 0; i < m->n; i++) {
        const fx_cap *c = &m->c[i];
        bool all = (c->kind == FXC_READ || c->kind == FXC_WRITE) && !strcmp(c->what, "~");
        sb_printf(&b, "- %s%s: %s%s, first used at line %d\n", kinds[c->kind], all ? " (all of theirs)" : "",
                  c->required ? "needed to open" : "optional", c->dynamic ? ", from an address or path made while it runs" : "",
                  c->line);
    }
    if (!m->n)
        sb_add(&b, "- nothing that needs a permission\n");
    sb_printf(&b, "In all: %d place(s) it reads, %d it changes%s, %d named server(s), any server: %s.\n", places_r, places_w,
              all_r || all_w ? " (including all of their files)" : "", hosts, any ? "yes" : "no");
    if (m->forbidden)
        sb_add(&b, "It also reaches for something no app is ever allowed (system files or a path climbing out of its "
                   "folder): it will not be run.\n");
    static const char *const lv[] = {"nothing unusual", "worth a note", "a strong warning"};
    sb_printf(&b, "The system's rules for its kind of app (%s): %s.\n",
              fx_category_ok(m->category) ? m->category : "other", lv[mismatch_level < 0 ? 0 : mismatch_level > 2 ? 2 : mismatch_level]);
    if (sc && sc->done) {
        static const char *const q[FXQ_COUNT] = {"sends what they type or their files to a server",
                                                 "deletes or overwrites files beyond what it obviously does",
                                                 "does things its interface doesn't show",
                                                 "is written to hide what it does"};
        sb_printf(&b, "A second opinion from %d small model(s) on this computer, 0 = no, 1 = yes:", sc->models);
        for (int i = 0; i < FXQ_COUNT; i++)
            sb_printf(&b, " %s %.2f;", q[i], sc->score[i]);
        sb_printf(&b, " signs that its text tries to steer the review: %s.\n", sc->injection ? "YES" : "no");
    } else {
        sb_add(&b, "No second opinion (the small models aren't available).\n");
    }
    sb_add(&b, "If anything here looks wrong for what they want it for, tell them plainly and briefly, before they "
               "decide; if it all looks ordinary, say nothing. You advise; they decide.)");
    return b.p;
}
