#pragma once
/* Inside the compositor: clients, their objects, and the wire format. */
#include "wl.h"
#include "wlproto.h"

#include <stddef.h>
#include <stdint.h>

#define WL_MAX_ARGS 20

typedef struct wl_client wl_client;
typedef struct wl_obj wl_obj;

union wl_arg {
    int32_t i;
    uint32_t u;   /* also object ids, new ids and fixed (24.8) */
    const char *s;
    struct {
        const void *data;
        uint32_t len;
    } a;
    int h;        /* a received file descriptor (owned by the handler) */
};

struct wl_obj {
    uint32_t id;
    const struct wl_iface *iface;
    int version;
    wl_client *client;
    void *data;
    void (*destroy)(wl_obj *o); /* frees data; called when the object goes */
};

/* Handles one request on an object of this interface. */
typedef void (*wl_handler)(wl_obj *o, int opcode, union wl_arg *args);

/* Objects */
wl_obj *wl_obj_new(wl_client *c, uint32_t id, const struct wl_iface *iface, int version, void *data);
wl_obj *wl_obj_get(wl_client *c, uint32_t id);
void wl_obj_destroy(wl_obj *o); /* also tells the client the id is free */
void wl_register_handler(const struct wl_iface *iface, wl_handler h);

/* Sends an event; arguments follow the event's signature (u/i/f as int32,
 * s as const char *, o as wl_obj * (or NULL), n as uint32 id, a as
 * (const void *, uint32 len), h as int fd). */
void wl_send(wl_obj *o, int opcode, ...);
void wl_post_error(wl_obj *o, uint32_t code, const char *msg);
uint32_t wl_next_serial(void);
wl_client *wl_obj_client(wl_obj *o);

/* Iterates every live object of an interface across clients. */
void wl_each(const struct wl_iface *iface, void (*fn)(wl_obj *o, void *ctx), void *ctx);

/* Globals advertised through wl_registry. bind makes the object. */
typedef void (*wl_bind_fn)(wl_client *c, uint32_t id, int version);
void wl_global(const struct wl_iface *iface, int version, wl_bind_fn bind);

void wl_comp_init(void); /* registers the interfaces in comp.c */
void wl_client_gone(wl_client *c); /* comp.c: drop views of a vanished client */
