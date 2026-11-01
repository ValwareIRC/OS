/* Facet 2's values: none, true/false, numbers, and text, lists and records
 * (shared, counted, freed when the last holder lets go). */
#define _GNU_SOURCE
#include "fx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fx_val fx_none(void) { return (fx_val){.t = V_NONE}; }
fx_val fx_bool(bool b) { return (fx_val){.t = V_BOOL, .b = b}; }
fx_val fx_num(double n) { return (fx_val){.t = V_NUM, .n = n}; }

static fx_obj *obj(fx_vtype t) {
    fx_obj *o = calloc(1, sizeof *o);
    o->refs = 1;
    o->t = t;
    return o;
}

fx_val fx_textn(const char *s, size_t n) {
    fx_obj *o = obj(V_TEXT);
    o->s = malloc(n + 1);
    memcpy(o->s, s, n);
    o->s[n] = 0;
    o->len = n;
    return (fx_val){.t = V_TEXT, .o = o};
}
fx_val fx_text(const char *s) { return fx_textn(s ? s : "", s ? strlen(s) : 0); }
fx_val fx_list(void) { return (fx_val){.t = V_LIST, .o = obj(V_LIST)}; }
fx_val fx_rec(void) { return (fx_val){.t = V_REC, .o = obj(V_REC)}; }

fx_val fx_ref(fx_val v) {
    if ((v.t == V_TEXT || v.t == V_LIST || v.t == V_REC) && v.o)
        v.o->refs++;
    return v;
}

void fx_unref(fx_val v) {
    if ((v.t != V_TEXT && v.t != V_LIST && v.t != V_REC) || !v.o || --v.o->refs > 0)
        return;
    fx_obj *o = v.o;
    free(o->s);
    for (int i = 0; i < o->n; i++) {
        fx_unref(o->items[i]);
        if (o->names)
            free(o->names[i]);
    }
    free(o->items);
    free(o->names);
    free(o);
}

static void grow(fx_obj *o) {
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 8;
        o->items = realloc(o->items, sizeof *o->items * (size_t)o->cap);
        if (o->t == V_REC)
            o->names = realloc(o->names, sizeof *o->names * (size_t)o->cap);
    }
}

void fx_list_push(fx_val list, fx_val item) {
    if (list.t != V_LIST) {
        fx_unref(item);
        return;
    }
    grow(list.o);
    list.o->items[list.o->n++] = item;
}

void fx_rec_set(fx_val rec, const char *name, fx_val v) {
    if (rec.t != V_REC) {
        fx_unref(v);
        return;
    }
    fx_obj *o = rec.o;
    for (int i = 0; i < o->n; i++)
        if (!strcmp(o->names[i], name)) {
            fx_unref(o->items[i]);
            o->items[i] = v;
            return;
        }
    grow(o);
    o->names[o->n] = strdup(name);
    o->items[o->n++] = v;
}

fx_val fx_rec_get(fx_val rec, const char *name) {
    if (rec.t != V_REC)
        return fx_none();
    for (int i = 0; i < rec.o->n; i++)
        if (!strcmp(rec.o->names[i], name))
            return rec.o->items[i];
    return fx_none();
}

bool fx_equal(fx_val a, fx_val b) {
    if (a.t != b.t)
        return false;
    switch (a.t) {
    case V_NONE: return true;
    case V_BOOL: return a.b == b.b;
    case V_NUM: return a.n == b.n;
    case V_TEXT: return a.o->len == b.o->len && !memcmp(a.o->s, b.o->s, a.o->len);
    case V_LIST:
        if (a.o == b.o)
            return true;
        if (a.o->n != b.o->n)
            return false;
        for (int i = 0; i < a.o->n; i++)
            if (!fx_equal(a.o->items[i], b.o->items[i]))
                return false;
        return true;
    case V_REC:
        if (a.o == b.o)
            return true;
        if (a.o->n != b.o->n)
            return false;
        for (int i = 0; i < a.o->n; i++)
            if (!fx_equal(a.o->items[i], fx_rec_get(b, a.o->names[i])))
                return false;
        return true;
    case V_FN: return a.fn == b.fn;
    }
    return false;
}

bool fx_truthy(fx_val v) {
    switch (v.t) {
    case V_NONE: return false;
    case V_BOOL: return v.b;
    case V_NUM: return v.n != 0;
    case V_TEXT: return v.o->len > 0;
    case V_LIST: return v.o->n > 0;
    default: return true;
    }
}

const char *fx_type_name(fx_val v) {
    static const char *const names[] = {"none", "true/false", "a number", "a text", "a list", "a record", "a fn"};
    return names[v.t];
}

static void show_num(char *buf, size_t n, double v) {
    if (isfinite(v) && v == floor(v) && fabs(v) < 1e15)
        snprintf(buf, n, "%.0f", v);
    else
        snprintf(buf, n, "%.10g", v);
}

char *fx_show(fx_val v) {
    char buf[64];
    switch (v.t) {
    case V_NONE: return strdup("");
    case V_BOOL: return strdup(v.b ? "true" : "false");
    case V_NUM: show_num(buf, sizeof buf, v.n); return strdup(buf);
    case V_TEXT: return strdup(v.o->s);
    case V_LIST: {
        size_t cap = 64, len = 0;
        char *s = malloc(cap);
        s[len++] = '[';
        for (int i = 0; i < v.o->n; i++) {
            char *p = fx_show(v.o->items[i]);
            size_t pl = strlen(p);
            while (len + pl + 4 > cap)
                s = realloc(s, cap *= 2);
            if (i)
                s[len++] = ',', s[len++] = ' ';
            memcpy(s + len, p, pl);
            len += pl;
            free(p);
        }
        s[len++] = ']';
        s[len] = 0;
        return s;
    }
    case V_REC: return strdup("{...}");
    case V_FN: return strdup("(fn)");
    }
    return strdup("");
}
