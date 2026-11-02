#pragma once
/* What a Facet app can do, worked out from its code (caps.c), and what it has been allowed (its grants).
 *
 * The language has no eval, no imports and no names made from text, so everything an app can reach is a call
 * written in it: fs.*, files(), file(), web(), net.*, app.open, setting(). The analysis reads every one:
 *   - files: the places named in the code ("~/Notes", "~/Todo.txt", or the folder a path starts with). A path
 *     made while it runs is confined to the places the code names; with none named, it needs all your files.
 *     Never anything outside home, never hidden app data in home (~/.config, ~/.var...) unless named exactly.
 *   - the internet: a server whose name is written in the code (the URL's or the socket's host fixed in the
 *     text) is named; one made while it runs needs the general internet permission.
 * Each is required (used as the app opens: its data) or optional (only when asked, in a fn): an app runs
 * without its optional ones, and their calls fail. The manifest comes only from the code; nothing the app says
 * about itself counts.
 *
 * At run time (eval.c) every call is checked against the grants, paths after resolving "..", "~" and links.
 * Nothing outside them happens, whatever the script tries. No grants: nothing that needs one. */
#include "fx.h"
#include <stdbool.h>
#include <stddef.h>

enum fx_cap_kind {
    FXC_READ,     /* see files in a place */
    FXC_WRITE,    /* change, make and delete files in a place */
    FXC_HOST,     /* talk to one server named in the code ("api.example.test", or "irc.libera.chat:6697") */
    FXC_INTERNET, /* talk to any server */
    FXC_KINDS
};

typedef struct fx_cap {
    int kind;
    char what[200]; /* a place ("~", "~/Notes", "~/.water") or a server; "" for FXC_INTERNET */
    bool required;  /* used as the app opens */
    bool dynamic;   /* needed because of a path or address made while it runs */
    int line;       /* where it is first used */
} fx_cap;

#define FX_MAX_CAPS 64

typedef struct fx_manifest {
    fx_cap c[FX_MAX_CAPS];
    int n;
    char app[64];        /* the app's name as it calls itself (shown, never trusted) */
    char category[24];   /* as declared ("notes"...), "other" if not */
    bool forbidden;      /* it reaches for something no permission can give (system files) */
    char forbidden_why[300];
} fx_manifest;

/* The categories an app may declare with `app Name category notes` */
extern const char *const fx_categories[];
bool fx_category_ok(const char *c);

void fx_manifest_of(const fx_app *app, fx_manifest *m);
/* "read:~/Notes", "write:~", "host:api.example.test", "internet": what grants are kept under */
void fx_cap_key(const fx_cap *c, char *out, size_t n);
/* back from a key ("read:~/Notes"): false if it isn't one */
bool fx_cap_from_key(const char *key, fx_cap *c);
/* What it means, in plain words: "can read your files in Notes (~/Notes)" */
void fx_cap_says(const fx_cap *c, char *out, size_t n);

/* ---- grants ---- */

typedef struct fx_grants {
    char key[FX_MAX_CAPS][220];
    bool yes[FX_MAX_CAPS];
    int n;
    char home[256];      /* where "~" is (filled from HOME if empty) */
} fx_grants;

/* Everything a manifest asks for, allowed (yes) or refused. */
void fx_grants_all(fx_grants *g, const fx_manifest *m, bool yes);
void fx_grants_set(fx_grants *g, const char *key, bool yes);
/* -1 never decided, 0 refused, 1 allowed */
int fx_grants_get(const fx_grants *g, const char *key);
/* "allow read:~/Notes" / "deny internet", a line each */
char *fx_grants_text(const fx_grants *g);
void fx_grants_parse(fx_grants *g, const char *text);

/* Run-time checks, the only ways an app reaches files and the network. A path is made absolute (~ and
 * relative paths are home), "." and ".." resolved, links followed for what exists; out = the real path. */
bool fx_path_allowed(const fx_grants *g, const char *path, bool write, char *out, size_t n, char *why, size_t whyn);
/* A URL's server (http and https only), or a socket's host and port (port 0: a web request). */
bool fx_url_host(const char *url, char *host, size_t n);
bool fx_net_allowed(const fx_grants *g, const char *host, int port, char *why, size_t whyn);

/* ---- before it runs ---- */

typedef struct fx_gate {
    bool can_run;                /* every required capability allowed, nothing new undecided */
    int newcaps[FX_MAX_CAPS];    /* indexes into the manifest: asked for and never decided */
    int nnew;
    int held[FX_MAX_CAPS];       /* already allowed */
    int nheld;
    int refused_required;        /* required ones refused before */
} fx_gate;

void fx_gate_of(const fx_manifest *m, const fx_grants *g, fx_gate *out);

/* ---- mismatches: rules, no AI ---- */

/* 0: nothing to say; 1: worth a plain note; 2: a strong warning. Written into out in plain words. */
int fx_mismatch(const fx_manifest *m, char *out, size_t n);
