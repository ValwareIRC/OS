#define _GNU_SOURCE
#include "fm.h"
#include "../../lib/json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* A lenient XML subset parser. Models write markup well but not perfectly,
 * so instead of rejecting a document it repairs what it can (unclosed or
 * mismatched tags, bare ampersands, unquoted attributes) and records each
 * repair as a problem with its line number, which goes back to the AI. */

void fm_report(fm_doc *d, int line, bool fatal, const char *fmt, ...) {
    if (d->nproblems >= FM_MAX_PROBLEMS)
        return;
    fm_problem *p = &d->problems[d->nproblems++];
    p->line = line;
    p->fatal = fatal;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->msg, sizeof p->msg, fmt, ap);
    va_end(ap);
}

bool fm_has_fatal(const fm_doc *d) {
    for (int i = 0; i < d->nproblems; i++)
        if (d->problems[i].fatal)
            return true;
    return false;
}

/* ---- tree helpers ---- */

fm_node *fm_node_new(fm_doc *d, const char *tag) {
    fm_node *n = calloc(1, sizeof *n);
    n->kind = FM_ELEM;
    snprintf(n->tag, sizeof n->tag, "%s", tag);
    snprintf(n->id, sizeof n->id, "_%d", ++d->next_id);
    n->app = n->win = -1;
    return n;
}

void fm_append(fm_node *parent, fm_node *child) {
    child->parent = parent;
    child->next = NULL;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
}

extern void fmi_canvas_forget(fm_node *n);

static void free_node(fm_node *n) {
    while (n) {
        if (n->kind == FM_ELEM && !strcmp(n->tag, "canvas"))
            fmi_canvas_forget(n);
        fm_node *next = n->next;
        free_node(n->first);
        free(n->text);
        for (int i = 0; i < n->nattrs; i++) {
            free(n->attrs[i].name);
            free(n->attrs[i].value);
        }
        free(n);
        n = next;
    }
}

void fm_remove(fm_node *n) {
    fm_node *p = n->parent;
    if (p) {
        fm_node **pp = &p->first, *prev = NULL;
        while (*pp && *pp != n) {
            prev = *pp;
            pp = &(*pp)->next;
        }
        if (*pp)
            *pp = n->next;
        if (p->last == n)
            p->last = prev;
    }
    n->next = NULL;
    free_node(n);
}

const char *fm_attr_get(const fm_node *n, const char *name) {
    if (!n)
        return NULL;
    for (int i = 0; i < n->nattrs; i++)
        if (!strcasecmp(n->attrs[i].name, name))
            return n->attrs[i].value;
    return NULL;
}

void fm_attr_set(fm_node *n, const char *name, const char *value) {
    for (int i = 0; i < n->nattrs; i++)
        if (!strcasecmp(n->attrs[i].name, name)) {
            free(n->attrs[i].value);
            n->attrs[i].value = strdup(value ? value : "");
            if (!strcasecmp(name, "id"))
                snprintf(n->id, sizeof n->id, "%s", value);
            return;
        }
    if (n->nattrs == FM_MAX_ATTRS)
        return;
    n->attrs[n->nattrs].name = strdup(name);
    n->attrs[n->nattrs].value = strdup(value ? value : "");
    n->nattrs++;
    if (!strcasecmp(name, "id"))
        snprintf(n->id, sizeof n->id, "%s", value);
}

char *fm_text_of(const fm_node *n) {
    sbuf b;
    sb_init(&b);
    for (const fm_node *c = n->first; c; c = c->next) {
        if (c->kind == FM_TEXT) {
            if (b.len)
                sb_add(&b, " ");
            sb_add(&b, c->text);
        }
    }
    if (!b.p)
        return strdup("");
    return b.p;
}

static fm_node *find_rec(fm_node *n, const char *q, int mode) {
    for (; n; n = n->next) {
        if (n->kind == FM_ELEM && !n->generated) {
            bool hit = false;
            if (mode == 0)
                hit = !strcmp(n->id, q);
            else if (mode == 1) {
                const char *cls = fm_attr_get(n, "class");
                if (cls) {
                    size_t ql = strlen(q);
                    for (const char *p = cls; (p = strstr(p, q)); p += ql)
                        if ((p == cls || p[-1] == ' ') && (p[ql] == 0 || p[ql] == ' ')) {
                            hit = true;
                            break;
                        }
                }
            } else if (mode == 2)
                hit = !strcasecmp(n->tag, q);
            else if (mode == 3) {
                const char *label = fm_attr_get(n, "label");
                if (label && !strcasecmp(label, q))
                    hit = true;
                else if (!label) {
                    char *t = fm_text_of(n);
                    hit = t[0] && !strcasecmp(t, q);
                    free(t);
                }
            }
            if (hit)
                return n;
        }
        fm_node *r = find_rec(n->first, q, mode);
        if (r)
            return r;
    }
    return NULL;
}

/* "#id", "id", ".class", "tag", or a quoted label ("Start"). */
fm_node *fm_find(fm_doc *d, const char *q) {
    if (!q || !*q || !d->root)
        return NULL;
    while (*q == ' ')
        q++;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", q);
    size_t n = strlen(buf);
    while (n && buf[n - 1] == ' ')
        buf[--n] = 0;
    if (buf[0] == '"' || buf[0] == '\'') {
        char quote = buf[0];
        char *end = strchr(buf + 1, quote);
        if (end)
            *end = 0;
        return find_rec(d->root, buf + 1, 3);
    }
    if (buf[0] == '#') {
        fm_node *r = find_rec(d->root, buf + 1, 0);
        /* "#desktop" is the desktop itself, which has no id: adding to it
         * ("add #desktop <popover ...>", as the companion is taught to)
         * matched nothing, so every new start menu failed to appear */
        if (!r && !strcasecmp(buf + 1, "desktop"))
            r = d->desktop;
        return r;
    }
    if (buf[0] == '.')
        return find_rec(d->root, buf + 1, 1);
    fm_node *r = find_rec(d->root, buf, 0);
    if (!r)
        r = find_rec(d->root, buf, 2);
    if (!r)
        r = find_rec(d->root, buf, 3);
    return r;
}

/* ---- parser ---- */

struct parser {
    const char *p, *end;
    int line;
    fm_doc *d;
};

static void advance(struct parser *ps, size_t n) {
    for (size_t i = 0; i < n && ps->p < ps->end; i++) {
        if (*ps->p == '\n')
            ps->line++;
        ps->p++;
    }
}

static void skip_ws(struct parser *ps) {
    while (ps->p < ps->end && isspace((unsigned char)*ps->p))
        advance(ps, 1);
}

static bool name_char(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':' || c == '.'; }

static void decode_entities(sbuf *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            sb_addn(out, s + i, 1);
            continue;
        }
        static const struct {
            const char *e;
            const char *r;
        } ents[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", " "}};
        bool done = false;
        for (size_t k = 0; k < sizeof ents / sizeof ents[0]; k++) {
            size_t el = strlen(ents[k].e);
            if (i + el <= n && !strncmp(s + i, ents[k].e, el)) {
                sb_add(out, ents[k].r);
                i += el - 1;
                done = true;
                break;
            }
        }
        if (!done && i + 3 < n && s[i + 1] == '#') {
            unsigned cp = 0;
            size_t j = i + 2;
            bool hex = s[j] == 'x' || s[j] == 'X';
            if (hex)
                j++;
            while (j < n && s[j] != ';' && j - i < 10) {
                char c = s[j];
                cp = cp * (hex ? 16u : 10u) + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
                j++;
            }
            if (j < n && s[j] == ';' && cp) {
                char u[5] = {0};
                if (cp < 0x80)
                    u[0] = (char)cp;
                else if (cp < 0x800)
                    u[0] = (char)(0xc0 | cp >> 6), u[1] = (char)(0x80 | (cp & 0x3f));
                else
                    u[0] = (char)(0xe0 | cp >> 12), u[1] = (char)(0x80 | ((cp >> 6) & 0x3f)), u[2] = (char)(0x80 | (cp & 0x3f));
                sb_add(out, u);
                i = j;
                done = true;
            }
        }
        if (!done)
            sb_add(out, "&"); /* a bare ampersand: keep it */
    }
}

static void add_text(struct parser *ps, fm_node *parent, const char *s, size_t n) {
    /* collapse whitespace; drop whitespace-only text */
    sbuf raw;
    sb_init(&raw);
    decode_entities(&raw, s, n);
    if (!raw.p)
        return;
    sbuf t;
    sb_init(&t);
    bool space = false;
    for (char *c = raw.p; *c; c++) {
        if (isspace((unsigned char)*c)) {
            space = t.len > 0;
            continue;
        }
        if (space)
            sb_add(&t, " ");
        space = false;
        sb_addn(&t, c, 1);
    }
    sb_free(&raw);
    if (!t.p)
        return;
    fm_node *tn = calloc(1, sizeof *tn);
    tn->kind = FM_TEXT;
    tn->text = t.p;
    tn->line = ps->line;
    tn->app = tn->win = -1;
    fm_append(parent, tn);
}

static bool parse_attrs(struct parser *ps, fm_node *n, bool *self_close) {
    *self_close = false;
    for (;;) {
        skip_ws(ps);
        if (ps->p >= ps->end)
            return false;
        if (*ps->p == '/' && ps->p + 1 < ps->end && ps->p[1] == '>') {
            advance(ps, 2);
            *self_close = true;
            return true;
        }
        if (*ps->p == '>') {
            advance(ps, 1);
            return true;
        }
        const char *ns = ps->p;
        while (ps->p < ps->end && name_char(*ps->p))
            advance(ps, 1);
        if (ps->p == ns) { /* junk: skip a character */
            fm_report(ps->d, ps->line, false, "unexpected '%c' inside <%s>", *ps->p, n->tag);
            advance(ps, 1);
            continue;
        }
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(ps->p - ns), ns);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '=') {
            advance(ps, 1);
            skip_ws(ps);
            sbuf v;
            sb_init(&v);
            if (ps->p < ps->end && (*ps->p == '"' || *ps->p == '\'')) {
                char q = *ps->p;
                advance(ps, 1);
                const char *vs = ps->p;
                while (ps->p < ps->end && *ps->p != q)
                    advance(ps, 1);
                decode_entities(&v, vs, (size_t)(ps->p - vs));
                advance(ps, 1);
            } else {
                const char *vs = ps->p;
                while (ps->p < ps->end && !isspace((unsigned char)*ps->p) && *ps->p != '>' &&
                       !(*ps->p == '/' && ps->p + 1 < ps->end && ps->p[1] == '>'))
                    advance(ps, 1);
                decode_entities(&v, vs, (size_t)(ps->p - vs));
                fm_report(ps->d, ps->line, false, "attribute %s on <%s> should be quoted", name, n->tag);
            }
            fm_attr_set(n, name, v.p ? v.p : "");
            sb_free(&v);
        } else {
            fm_attr_set(n, name, "true"); /* boolean attribute */
        }
    }
}

static fm_doc *parse_into(fm_doc *d, const char *text, size_t len, fm_node *top) {
    struct parser ps = {text, text + len, 1, d};
    fm_node *cur = top;
    while (ps.p < ps.end) {
        const char *lt = memchr(ps.p, '<', (size_t)(ps.end - ps.p));
        if (!lt) {
            add_text(&ps, cur, ps.p, (size_t)(ps.end - ps.p));
            break;
        }
        if (lt > ps.p) {
            const char *s = ps.p;
            advance(&ps, (size_t)(lt - ps.p));
            add_text(&ps, cur, s, (size_t)(lt - s));
        }
        if (ps.end - ps.p >= 4 && !strncmp(ps.p, "<!--", 4)) {
            const char *e = strstr(ps.p, "-->");
            advance(&ps, e ? (size_t)(e + 3 - ps.p) : (size_t)(ps.end - ps.p));
            continue;
        }
        if (ps.end - ps.p >= 2 && (ps.p[1] == '?' || ps.p[1] == '!')) {
            const char *e = memchr(ps.p, '>', (size_t)(ps.end - ps.p));
            advance(&ps, e ? (size_t)(e + 1 - ps.p) : (size_t)(ps.end - ps.p));
            continue;
        }
        if (ps.end - ps.p >= 2 && ps.p[1] == '/') { /* closing tag */
            advance(&ps, 2);
            const char *ns = ps.p;
            while (ps.p < ps.end && name_char(*ps.p))
                advance(&ps, 1);
            char name[32];
            snprintf(name, sizeof name, "%.*s", (int)(ps.p - ns), ns);
            const char *gt = memchr(ps.p, '>', (size_t)(ps.end - ps.p));
            advance(&ps, gt ? (size_t)(gt + 1 - ps.p) : (size_t)(ps.end - ps.p));
            fm_node *match = cur;
            while (match && match != top && strcasecmp(match->tag, name))
                match = match->parent;
            if (!match || match == top) {
                fm_report(d, ps.line, false, "</%s> closes nothing; ignored", name);
                continue;
            }
            for (fm_node *n = cur; n != match; n = n->parent)
                fm_report(d, n->line, false, "<%s> was not closed; closed at </%s>", n->tag, name);
            cur = match->parent;
            continue;
        }
        /* opening tag */
        advance(&ps, 1);
        const char *ns = ps.p;
        while (ps.p < ps.end && name_char(*ps.p))
            advance(&ps, 1);
        if (ps.p == ns) { /* a lone '<' in text */
            add_text(&ps, cur, "<", 1);
            continue;
        }
        char name[32];
        snprintf(name, sizeof name, "%.*s", (int)(ps.p - ns), ns);
        for (char *c = name; *c; c++)
            *c = (char)tolower((unsigned char)*c);
        fm_node *n = fm_node_new(d, name);
        n->line = ps.line;
        bool self_close;
        if (!parse_attrs(&ps, n, &self_close)) {
            fm_report(d, n->line, false, "<%s> is cut off at the end", name);
            fm_append(cur, n);
            break;
        }
        fm_append(cur, n);
        if (self_close)
            continue;
        if (!strcmp(name, "style") || !strcmp(name, "canvas")) { /* raw text up to the closing tag */
            char closing[16];
            snprintf(closing, sizeof closing, "</%s", name);
            const char *e = strcasestr(ps.p, closing);
            size_t n_raw = e ? (size_t)(e - ps.p) : (size_t)(ps.end - ps.p);
            fm_node *tn = calloc(1, sizeof *tn);
            tn->kind = FM_TEXT;
            tn->text = strndup(ps.p, n_raw);
            tn->app = tn->win = -1;
            fm_append(n, tn);
            advance(&ps, n_raw);
            if (e) {
                const char *gt = memchr(ps.p, '>', (size_t)(ps.end - ps.p));
                advance(&ps, gt ? (size_t)(gt + 1 - ps.p) : (size_t)(ps.end - ps.p));
            }
            continue;
        }
        cur = n;
    }
    for (fm_node *n = cur; n && n != top; n = n->parent)
        fm_report(d, n->line, false, "<%s> was never closed", n->tag);
    return d;
}

static fm_node *child_tag(fm_node *n, const char *tag) {
    for (fm_node *c = n ? n->first : NULL; c; c = c->next)
        if (c->kind == FM_ELEM && !strcasecmp(c->tag, tag))
            return c;
    return NULL;
}

static void collect_ids(fm_node *n, fm_node **out, int *count, int max) {
    for (; n; n = n->next)
        if (n->kind == FM_ELEM) {
            if (n->id[0] && n->id[0] != '_' && *count < max)
                out[(*count)++] = n;
            collect_ids(n->first, out, count, max);
        }
}

/* Ids must be unique, but models sometimes reuse one (a stack and the list
 * inside it both "hub"). The one that holds items (a list) keeps the name,
 * since that is what styles like ".hub item" are about; the others get a
 * numbered name, so a rule never lands on two different things. */
static void dedupe_ids(fm_doc *d) {
    static fm_node *nodes[1024];
    int n = 0;
    collect_ids(d->root, nodes, &n, 1024);
    for (int i = 0; i < n; i++) {
        if (!nodes[i])
            continue;
        int same[16], ns = 0;
        for (int j = i; j < n && ns < 16; j++)
            if (nodes[j] && !strcmp(nodes[j]->id, nodes[i]->id))
                same[ns++] = j;
        if (ns < 2)
            continue;
        int keep = same[0];
        for (int k = 0; k < ns; k++)
            if (!strcmp(nodes[same[k]]->tag, "applist") || !strcmp(nodes[same[k]]->tag, "windowlist")) {
                keep = same[k];
                break;
            }
        int num = 2;
        for (int k = 0; k < ns; k++) {
            fm_node *e = nodes[same[k]];
            if (same[k] != keep) {
                char nid[sizeof e->id];
                snprintf(nid, sizeof nid, "%.40s-%d", e->id, num++);
                memcpy(e->id, nid, sizeof nid);
                fm_report(d, e->line, false, "the id \"%s\" was used twice; this one is now \"%s\"",
                          nodes[keep]->id, e->id);
            }
            nodes[same[k]] = NULL;
        }
    }
}

fm_doc *fm_parse(const char *text) {
    fm_doc *d = calloc(1, sizeof *d);
    d->entrance_at = -1;
    /* Models sometimes wrap the document in prose or code fences: start at
     * <facet>, end at </facet>. */
    const char *start = strcasestr(text, "<facet");
    const char *end = start ? strcasestr(start, "</facet>") : NULL;
    fm_node *holder = fm_node_new(d, "#document");
    if (start) {
        size_t len = end ? (size_t)(end + 8 - start) : strlen(start);
        parse_into(d, start, len, holder);
        if (!end)
            fm_report(d, 0, false, "</facet> missing at the end");
    } else {
        parse_into(d, text, strlen(text), holder);
    }
    d->root = child_tag(holder, "facet");
    if (!d->root) {
        /* Accept a bare <desktop> by wrapping it. */
        d->root = fm_node_new(d, "facet");
        for (fm_node *c = holder->first, *next; c; c = next) {
            next = c->next;
            fm_append(d->root, c);
        }
        holder->first = holder->last = NULL;
        if (!child_tag(d->root, "desktop"))
            fm_report(d, 0, true, "no <facet> document found");
    } else {
        d->root->parent = NULL;
        /* detach root from the holder */
        for (fm_node *c = holder->first, *next; c; c = next) {
            next = c->next;
            if (c != d->root) {
                c->next = NULL;
                free_node(c);
            }
        }
        holder->first = holder->last = NULL;
        d->root->next = NULL;
    }
    free(holder);
    dedupe_ids(d);
    d->desktop = child_tag(d->root, "desktop");
    if (!d->desktop)
        fm_report(d, 0, true, "the document has no <desktop>");
    for (fm_node *c = d->root->first; c; c = c->next)
        if (c->kind == FM_ELEM && !strcasecmp(c->tag, "template")) {
            const char *id = fm_attr_get(c, "id");
            if (!id || !strcasecmp(id, "window"))
                d->window_tpl = c;
        }
    fm_node *style = child_tag(d->root, "style");
    d->css = style && style->first && style->first->kind == FM_TEXT ? strdup(style->first->text) : strdup("");
    fm_css_load(d);
    return d;
}

void fm_free(fm_doc *d) {
    if (!d)
        return;
    free_node(d->root);
    free(d->css);
    for (int i = 0; i < d->nvars; i++) {
        free(d->vars[i][0]);
        free(d->vars[i][1]);
    }
    extern void fm_css_free_rules(fm_doc * d);
    fm_css_free_rules(d);
    free(d);
}

/* ---- serialiser ---- */

static void escape(sbuf *b, const char *s, bool attr) {
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_add(b, "&amp;"); break;
        case '<': sb_add(b, "&lt;"); break;
        case '>': sb_add(b, attr ? "&gt;" : ">"); break;
        case '"': sb_add(b, attr ? "&quot;" : "\""); break;
        default: sb_addn(b, s, 1);
        }
    }
}

static void ser(sbuf *b, const fm_node *n, int depth, const fm_doc *d) {
    for (; n; n = n->next) {
        if (n->generated)
            continue;
        for (int i = 0; i < depth; i++)
            sb_add(b, "  ");
        if (n->kind == FM_TEXT) {
            if (n->parent && !strcmp(n->parent->tag, "canvas"))
                sb_add(b, n->text); /* a script: kept verbatim */
            else
                escape(b, n->text, false);
            sb_add(b, "\n");
            continue;
        }
        sb_printf(b, "<%s", n->tag);
        for (int i = 0; i < n->nattrs; i++) {
            sb_printf(b, " %s=\"", n->attrs[i].name);
            escape(b, n->attrs[i].value, true);
            sb_add(b, "\"");
        }
        if (!strcmp(n->tag, "style")) {
            sb_add(b, ">\n");
            sb_add(b, d->css);
            sb_add(b, "\n");
            for (int i = 0; i < depth; i++)
                sb_add(b, "  ");
            sb_add(b, "</style>\n");
            continue;
        }
        bool has_children = false;
        for (fm_node *c = n->first; c; c = c->next)
            if (!c->generated)
                has_children = true;
        if (!has_children) {
            sb_add(b, "/>\n");
            continue;
        }
        sb_add(b, ">\n");
        ser(b, n->first, depth + 1, d);
        for (int i = 0; i < depth; i++)
            sb_add(b, "  ");
        sb_printf(b, "</%s>\n", n->tag);
    }
}

char *fm_serialize(const fm_doc *d) {
    sbuf b;
    sb_init(&b);
    if (d->root) {
        /* Make sure the stylesheet is written even if the document had no
         * <style> element (patches may have added rules). */
        bool has_style = false;
        for (fm_node *c = d->root->first; c; c = c->next)
            if (c->kind == FM_ELEM && !strcmp(c->tag, "style"))
                has_style = true;
        fm_node r = *d->root;
        r.next = NULL;
        ser(&b, &r, 0, d);
        if (!has_style && d->css && d->css[0]) {
            /* insert before the closing tag */
            char *close = strstr(b.p, "</facet>");
            if (close) {
                size_t at = (size_t)(close - b.p);
                sbuf out;
                sb_init(&out);
                sb_addn(&out, b.p, at);
                sb_add(&out, "  <style>\n");
                sb_add(&out, d->css);
                sb_add(&out, "\n  </style>\n");
                sb_add(&out, b.p + at);
                sb_free(&b);
                return out.p;
            }
        }
    }
    return b.p ? b.p : strdup("");
}

fm_node *fm_parse_fragment(fm_doc *d, fm_node *parent, const char *markup) {
    fm_node *before = parent->last;
    parse_into(d, markup, strlen(markup), parent);
    return before ? before->next : parent->first;
}
