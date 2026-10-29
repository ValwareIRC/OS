#pragma once

/* What a Wayland message looks like on the wire, as read from the protocol
 * XML by userland/tools/wlgen.py. Signature letters: i int, u uint,
 * f fixed (24.8), s string, o object id, n new object id, a array, h file
 * descriptor (passed alongside, not inline); '?' before a letter means it
 * may be null. */
struct wl_iface;

struct wl_msg {
    const char *name;
    const char *sig;
    const struct wl_iface *const *types; /* per argument, for o and n; may be NULL */
    int since;
};

struct wl_iface {
    const char *name;
    int version;
    int nreq;
    const struct wl_msg *req;
    int nev;
    const struct wl_msg *ev;
};
