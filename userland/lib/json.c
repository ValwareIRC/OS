#include "json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parser {
    const char *p, *end;
    int depth;
};

static void skip_ws(struct parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static json *new_node(enum json_type t) {
    json *j = calloc(1, sizeof *j);
    if (j)
        j->type = t;
    return j;
}

static void put_utf8(sbuf *b, unsigned cp) {
    char out[4];
    int n;
    if (cp < 0x80) {
        out[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        out[0] = (char)(0xf0 | cp >> 18);
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    sb_addn(b, out, (size_t)n);
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

static char *parse_string(struct parser *ps) {
    if (ps->p >= ps->end || *ps->p != '"')
        return NULL;
    ps->p++;
    sbuf b;
    sb_init(&b);
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') {
            sb_addn(&b, &c, 1);
            continue;
        }
        if (ps->p >= ps->end)
            break;
        char e = *ps->p++;
        switch (e) {
        case 'n': sb_add(&b, "\n"); break;
        case 't': sb_add(&b, "\t"); break;
        case 'r': sb_add(&b, "\r"); break;
        case 'b': sb_add(&b, "\b"); break;
        case 'f': sb_add(&b, "\f"); break;
        case 'u': {
            unsigned cp;
            if (ps->end - ps->p < 4 || hex4(ps->p, &cp) < 0)
                goto fail;
            ps->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 2, &lo) == 0 && lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    ps->p += 6;
                }
            }
            put_utf8(&b, cp);
            break;
        }
        default: sb_addn(&b, &e, 1); break;
        }
    }
    if (ps->p >= ps->end)
        goto fail;
    ps->p++;
    if (!b.p)
        sb_add(&b, "");
    return b.p;
fail:
    sb_free(&b);
    return NULL;
}

static json *parse_value(struct parser *ps);

static json *parse_container(struct parser *ps, bool object) {
    json *node = new_node(object ? JSON_OBJECT : JSON_ARRAY), **tail = &node->child;
    ps->p++;
    if (++ps->depth > 256)
        goto fail;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) {
        ps->p++;
        ps->depth--;
        return node;
    }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (object) {
            key = parse_string(ps);
            if (!key)
                goto fail;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                goto fail;
            }
            ps->p++;
        }
        json *v = parse_value(ps);
        if (!v) {
            free(key);
            goto fail;
        }
        v->key = key;
        *tail = v;
        tail = &v->next;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) {
            ps->p++;
            ps->depth--;
            return node;
        }
        goto fail;
    }
fail:
    json_free(node);
    return NULL;
}

static json *parse_value(struct parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end)
        return NULL;
    char c = *ps->p;
    if (c == '{' || c == '[')
        return parse_container(ps, c == '{');
    if (c == '"') {
        char *s = parse_string(ps);
        if (!s)
            return NULL;
        json *j = new_node(JSON_STRING);
        j->str = s;
        return j;
    }
    if (!strncmp(ps->p, "true", 4) || !strncmp(ps->p, "false", 5)) {
        json *j = new_node(JSON_BOOL);
        j->boolean = c == 't';
        ps->p += c == 't' ? 4 : 5;
        return j;
    }
    if (!strncmp(ps->p, "null", 4)) {
        ps->p += 4;
        return new_node(JSON_NULL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        size_t n = 0;
        while (ps->p < ps->end && n < sizeof buf - 1 && strchr("+-0123456789.eE", *ps->p))
            buf[n++] = *ps->p++;
        buf[n] = 0;
        json *j = new_node(JSON_NUMBER);
        j->num = strtod(buf, NULL);
        return j;
    }
    return NULL;
}

json *json_parse(const char *text, size_t len) {
    struct parser ps = {text, text + len, 0};
    json *j = parse_value(&ps);
    return j;
}

void json_free(json *j) {
    while (j) {
        json *next = j->next;
        json_free(j->child);
        free(j->key);
        free(j->str);
        free(j);
        j = next;
    }
}

json *json_get(const json *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT)
        return NULL;
    for (json *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key))
            return c;
    return NULL;
}

json *json_at(const json *arr, int index) {
    if (!arr || (arr->type != JSON_ARRAY && arr->type != JSON_OBJECT))
        return NULL;
    json *c = arr->child;
    while (c && index-- > 0)
        c = c->next;
    return c;
}

int json_len(const json *arr) {
    int n = 0;
    if (arr && (arr->type == JSON_ARRAY || arr->type == JSON_OBJECT))
        for (json *c = arr->child; c; c = c->next)
            n++;
    return n;
}

const char *json_str(const json *j, const char *fallback) { return j && j->type == JSON_STRING ? j->str : fallback; }
double json_num(const json *j, double fallback) { return j && j->type == JSON_NUMBER ? j->num : fallback; }
bool json_bool(const json *j, bool fallback) { return j && j->type == JSON_BOOL ? j->boolean : fallback; }

json *json_path(const json *root, const char *path) {
    const json *cur = root;
    char part[128];
    while (cur && *path) {
        size_t n = 0;
        while (path[n] && path[n] != '.' && n < sizeof part - 1) {
            part[n] = path[n];
            n++;
        }
        part[n] = 0;
        path += n;
        if (*path == '.')
            path++;
        if (cur->type == JSON_ARRAY && part[0] >= '0' && part[0] <= '9')
            cur = json_at(cur, atoi(part));
        else
            cur = json_get(cur, part);
    }
    return (json *)cur;
}

/* ---- string builder ---- */

void sb_init(sbuf *b) {
    b->p = NULL;
    b->len = b->cap = 0;
}

void sb_free(sbuf *b) {
    free(b->p);
    sb_init(b);
}

void sb_addn(sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void sb_add(sbuf *b, const char *s) { sb_addn(b, s, strlen(s)); }

void sb_printf(sbuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof tmp) {
        sb_addn(b, tmp, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big)
        return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_addn(b, big, (size_t)n);
    free(big);
}

void sb_json_str(sbuf *b, const char *s) {
    sb_add(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': sb_add(b, "\\\""); break;
        case '\\': sb_add(b, "\\\\"); break;
        case '\n': sb_add(b, "\\n"); break;
        case '\r': sb_add(b, "\\r"); break;
        case '\t': sb_add(b, "\\t"); break;
        default:
            if (c < 0x20)
                sb_printf(b, "\\u%04x", c);
            else
                sb_addn(b, (const char *)&c, 1);
        }
    }
    sb_add(b, "\"");
}

void sb_json(sbuf *b, const json *j) {
    if (!j) {
        sb_add(b, "null");
        return;
    }
    switch (j->type) {
    case JSON_NULL: sb_add(b, "null"); break;
    case JSON_BOOL: sb_add(b, j->boolean ? "true" : "false"); break;
    case JSON_NUMBER: sb_printf(b, "%.17g", j->num); break;
    case JSON_STRING: sb_json_str(b, j->str); break;
    case JSON_ARRAY:
    case JSON_OBJECT: {
        bool obj = j->type == JSON_OBJECT;
        sb_add(b, obj ? "{" : "[");
        for (json *c = j->child; c; c = c->next) {
            if (c != j->child)
                sb_add(b, ",");
            if (obj) {
                sb_json_str(b, c->key);
                sb_add(b, ":");
            }
            sb_json(b, c);
        }
        sb_add(b, obj ? "}" : "]");
    }
    }
}
