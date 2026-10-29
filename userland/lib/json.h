#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Small JSON DOM parser and a string builder for producing JSON. */

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

typedef struct json {
    enum json_type type;
    char *key;           /* member name when inside an object */
    char *str;           /* JSON_STRING (UTF-8, unescaped) */
    double num;
    bool boolean;
    struct json *child;  /* first element/member */
    struct json *next;
} json;

json *json_parse(const char *text, size_t len);
void json_free(json *j);
json *json_get(const json *obj, const char *key);     /* NULL if absent */
json *json_at(const json *arr, int index);
int json_len(const json *arr);
const char *json_str(const json *j, const char *fallback);
double json_num(const json *j, double fallback);
bool json_bool(const json *j, bool fallback);
/* Path lookup like "choices.0.message.content". */
json *json_path(const json *root, const char *path);

/* Growable string buffer. */
typedef struct sbuf {
    char *p;
    size_t len, cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_add(sbuf *b, const char *s);
void sb_addn(sbuf *b, const char *s, size_t n);
void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Appends `s` as a quoted, escaped JSON string. */
void sb_json_str(sbuf *b, const char *s);
/* Re-serialises a parsed value (used to pass AI-produced JSON along). */
void sb_json(sbuf *b, const json *j);
