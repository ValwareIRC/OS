#pragma once
/* Running a Facet 2 app (eval.c): its values, its view as a tree of
 * elements with their handlers, events in, timers and live data. The world
 * outside (files, settings, the clock, dialogs) comes through an fx_host, so
 * the same app runs in the OS and in tests. */
#include "fx.h"

typedef struct fx_host {
    void *ctx;
    char *(*read)(void *ctx, const char *path);                   /* malloc'd, or NULL if it can't */
    bool (*write)(void *ctx, const char *path, const char *text);
    bool (*remove)(void *ctx, const char *path);
    bool (*rename)(void *ctx, const char *from, const char *to);
    bool (*mkdir)(void *ctx, const char *path);
    fx_val (*files)(void *ctx, const char *dir, const char *ending); /* list of {name, path, size, modified, dir} */
    fx_val (*setting)(void *ctx, const char *key);
    fx_val (*apps)(void *ctx);                                    /* list of {id, name} */
    double (*now_ms)(void *ctx);                                  /* milliseconds since 1970 */
    bool (*open_app)(void *ctx, const char *id, const char *path);
    void (*close_app)(void *ctx);
    void (*notify)(void *ctx, const char *text);
    int (*ask)(void *ctx, const char *question);                  /* 1 yes, 0 no */
    char *(*prompt)(void *ctx, const char *question, const char *def); /* malloc'd, or NULL */
    void (*error)(void *ctx, const char *msg);                    /* a fault while running */
    /* What the person allowed this app (caps.h): every file and network call is checked against it here, in the
     * language, before the host is asked. NULL: nothing that needs a permission. */
    const struct fx_grants *grants;
    /* (unused: permissions are decided before an app runs, from its code) */
    bool (*net_allowed)(void *ctx);
    /* A request, waited for (the OS goes on meanwhile): {ok, status, data (its JSON, parsed), text, error}. */
    fx_val (*http)(void *ctx, const char *method, const char *url, const char *body, const char *content_type);
    /* web(url, every) data: what was fetched last (none until it arrives), fetched again every `every_ms`. */
    fx_val (*web)(void *ctx, const char *url, double every_ms);
    /* Sockets: opened in the background (an event says when); an id, or -1. */
    int (*net_connect)(void *ctx, const char *host, int port, bool tls);
    bool (*net_send)(void *ctx, int id, const void *data, size_t len);
    void (*net_close)(void *ctx, int id);
    /* The next thing that happened on a socket: kind 1 open, 2 data (*data malloc'd, *len), 3 closed, 4 error
     * (*data the reason). 0 when nothing more. */
    int (*net_event)(void *ctx, int *id, char **data, size_t *len);
} fx_host;

typedef struct fx_inst fx_inst;
typedef struct fx_rt fx_rt;

/* One element of the drawn view. */
typedef struct fx_scope fx_scope;
typedef struct fx_handler {
    char *event;           /* "click", "input", "picked"... */
    fx_inst *inst;         /* whose fn it calls */
    const fx_expr *call;
    fx_scope *scope;       /* the names it saw (loop items...), kept */
} fx_handler;

typedef struct fx_out fx_out;
struct fx_out {
    char *tag;             /* NULL: text */
    char *text;
    char *key;             /* a stable path: the same element across redraws */
    char **anames, **avals;
    int nattrs;
    fx_handler *h;
    int nh;
    char *bind;            /* two-way state name */
    fx_inst *bind_inst;
    fx_out **kids;
    int nkids;
    int line;
};

fx_rt *fx_start(const fx_app *app, const fx_host *host);
void fx_stop(fx_rt *rt);
/* Reads live data again where its inputs changed or it is due; runs due timers. True if anything changed. */
bool fx_tick(fx_rt *rt);
/* The view as it is now (owned by rt, valid until the next fx_render). */
fx_out *fx_render(fx_rt *rt);
/* Something happened on an element: an event with its value (text, key name, true/false), or a bound input
 * changed. True if it changed anything. */
bool fx_event(fx_rt *rt, fx_out *el, const char *event, fx_val value);
bool fx_bind(fx_rt *rt, fx_out *el, fx_val value);
/* Something changed since the last render. */
bool fx_dirty(fx_rt *rt);
/* The last fault while running, or NULL. */
const char *fx_fault(fx_rt *rt);
const char *fx_get(const fx_out *el, const char *name); /* NULL if absent */
fx_out *fx_find(fx_out *root, const char *id);             /* first element with this id */
fx_val fx_state_get(fx_rt *rt, const char *name);          /* borrowed (tests) */
/* JSON text as Facet values (objects are records, arrays lists, null none), or none if it isn't JSON. */
fx_val fx_parse_json(const char *text, size_t len);
/* A value as JSON text (malloc'd). */
char *fx_json_text(fx_val v);
