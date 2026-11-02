/* facet: Facet 2 on the build machine (docs/facet2.md).
 *   facet check FILE...          every load-time error in each file, at once; exit 1 if any
 *   facet run FILE SCRIPT        runs the app against a script of actions, in a home folder of its own
 * Script lines (# for comments):
 *   show                         prints the view as a tree
 *   click #id | click "Text"     clicks the element with that id, or the first whose text is that
 *   type #id some words          sets a bound input and sends input
 *   key #id enter                sends a key
 *   check #id                    toggles a checkbox (change event with its new value)
 *   tick 1500                    the clock moves on 1500 ms; timers and live data run
 *   answer yes|no|"text"         how the next ask or prompt is answered
 *   write ~/path text...         puts a file in its home
 *   expect "text"                the view must contain this text somewhere (fails the run otherwise)
 *   expectnot "text"             ...must not
 *   file ~/path "text"           that file must hold exactly this
 *   state name                   prints a state value
 * Build: tests/facet.sh */
#define _GNU_SOURCE
#include "../userland/shell/fx/fx_rt.h"
#include "../userland/shell/fx/caps.h"
#include "../userland/shell/fx/review.h"
#include "../userland/lib/json.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <math.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    s[n] = 0;
    fclose(f);
    return s;
}

/* ---- a host for tests ---- */

static double clock_ms = 1793500000000.0; /* a fixed moment, moved by tick */
static char answers[16][200];
static int nanswers, next_answer;
static int faults;

static char *h_read(void *c, const char *p) { (void)c; return slurp(p); }
static bool h_write(void *c, const char *p, const char *t) {
    (void)c;
    FILE *f = fopen(p, "wb");
    if (!f)
        return false;
    fputs(t, f);
    fclose(f);
    return true;
}
static bool h_remove(void *c, const char *p) { (void)c; return remove(p) == 0; }
static bool h_rename(void *c, const char *a, const char *b) { (void)c; return rename(a, b) == 0; }
static bool h_mkdir(void *c, const char *p) { (void)c; return mkdir(p, 0755) == 0; }
static fx_val h_files(void *c, const char *dir, const char *ending) {
    (void)c;
    fx_val l = fx_list();
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        size_t nl = strlen(e->d_name);
        if (ending && *ending && (nl < strlen(ending) || strcmp(e->d_name + nl - strlen(ending), ending)))
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st))
            continue;
        fx_val r = fx_rec();
        fx_rec_set(r, "name", fx_text(e->d_name));
        fx_rec_set(r, "path", fx_text(path));
        fx_rec_set(r, "size", fx_num((double)st.st_size));
        fx_rec_set(r, "modified", fx_num((double)st.st_mtime * 1000));
        fx_rec_set(r, "dir", fx_bool(S_ISDIR(st.st_mode)));
        fx_list_push(l, r);
    }
    if (d)
        closedir(d);
    return l;
}
static fx_val h_setting(void *c, const char *k) { (void)c; return fx_text(!strcmp(k, "design.accent") ? "#ff2bd6" : ""); }
static fx_val h_apps(void *c) {
    (void)c;
    static const char *const ids[][2] = {{"files", "Files"}, {"notes", "Notes"}, {"settings", "Settings"},
                                         {"terminal", "Terminal"}, {"prism", "Prism"}, {"chromium", "Chromium"}};
    fx_val l = fx_list();
    for (int i = 0; i < 6; i++) {
        fx_val r = fx_rec();
        fx_rec_set(r, "id", fx_text(ids[i][0]));
        fx_rec_set(r, "name", fx_text(ids[i][1]));
        fx_list_push(l, r);
    }
    return l;
}
static double h_now(void *c) { (void)c; return clock_ms; }
static bool h_open(void *c, const char *id, const char *path) {
    (void)c;
    printf("  -> opens %s%s%s\n", id, path ? " with " : "", path ? path : "");
    return true;
}
static void h_notify(void *c, const char *t) { (void)c; printf("  -> notice: %s\n", t); }
static int h_ask(void *c, const char *q) {
    (void)c;
    const char *a = next_answer < nanswers ? answers[next_answer++] : "no";
    printf("  -> asks \"%s\": %s\n", q, a);
    return !strcmp(a, "yes");
}
static char *h_prompt(void *c, const char *q, const char *def) {
    (void)c, (void)def;
    const char *a = next_answer < nanswers ? answers[next_answer++] : NULL;
    printf("  -> prompts \"%s\": %s\n", q, a ? a : "(cancelled)");
    return a ? strdup(a) : NULL;
}
static int refusals;
static char last_refusal[600];
static void h_error(void *c, const char *m) {
    (void)c;
    if (!strncmp(m, "not allowed", 11)) { /* the sandbox said no: what a test of permissions looks for */
        printf("  -> refused: %s\n", m);
        refusals++;
        snprintf(last_refusal, sizeof last_refusal, "%s", m);
        return;
    }
    printf("  !! %s\n", m);
    faults++;
}

/* ---- the network, for tests: canned web replies, and real local sockets ---- */

static struct { char url[300]; char *text; } canned[16];
static int ncanned;
static int net_refused_by_script;

static bool h_net_allowed(void *c) {
    (void)c;
    printf("  -> asks to use the internet: %s\n", net_refused_by_script ? "no" : "yes");
    return !net_refused_by_script;
}

static fx_val reply_for(const char *url) {
    fx_val r = fx_rec();
    for (int i = 0; i < ncanned; i++)
        if (!strcmp(canned[i].url, url)) {
            fx_rec_set(r, "ok", fx_bool(true));
            fx_rec_set(r, "status", fx_num(200));
            fx_rec_set(r, "data", fx_parse_json(canned[i].text, strlen(canned[i].text)));
            fx_rec_set(r, "text", fx_text(canned[i].text));
            fx_rec_set(r, "error", fx_none());
            return r;
        }
    fx_rec_set(r, "ok", fx_bool(false));
    fx_rec_set(r, "status", fx_num(404));
    fx_rec_set(r, "data", fx_none());
    fx_rec_set(r, "text", fx_text(""));
    fx_rec_set(r, "error", fx_text("not found"));
    return r;
}

static fx_val h_http(void *c, const char *method, const char *url, const char *body, const char *ct) {
    (void)c;
    printf("  -> %s %s%s%s\n", method, url, body ? " " : "", body ? body : "");
    (void)ct;
    return reply_for(url);
}

static fx_val h_web(void *c, const char *url, double every) {
    (void)c, (void)every;
    fx_val r = reply_for(url);
    fx_val d = fx_ref(fx_rec_get(r, "data"));
    fx_unref(r);
    return d;
}

static struct { int fd; bool open_told, live; } socks[16];
static int sock_queue_closed[16];

static int h_net_connect(void *c, const char *host, int port, bool tls) {
    (void)c;
    printf("  -> connects to %s:%d%s\n", host, port, tls ? " (tls: not in tests, plain)" : "");
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    char ports[8];
    snprintf(ports, sizeof ports, "%d", port);
    if (getaddrinfo(host, ports, &hints, &res))
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    for (int i = 0; i < 16; i++)
        if (!socks[i].live) {
            socks[i].fd = fd, socks[i].live = true, socks[i].open_told = false;
            return i;
        }
    close(fd);
    return -1;
}

static bool h_net_send(void *c, int id, const void *d, size_t n) {
    (void)c;
    if (id < 0 || id >= 16 || !socks[id].live)
        return false;
    return send(socks[id].fd, d, n, MSG_NOSIGNAL) == (ssize_t)n;
}

static void h_net_close(void *c, int id) {
    (void)c;
    if (id >= 0 && id < 16 && socks[id].live) {
        close(socks[id].fd);
        socks[id].live = false;
    }
}

static int h_net_event(void *c, int *id, char **data, size_t *len) {
    (void)c;
    *data = NULL, *len = 0;
    for (int i = 0; i < 16; i++) {
        if (!socks[i].live)
            continue;
        *id = i;
        if (!socks[i].open_told) {
            socks[i].open_told = true;
            return 1;
        }
        char buf[4096];
        /* a moment for the server's words to arrive (the script runs faster than a network) */
        struct pollfd pf = {socks[i].fd, POLLIN, 0};
        poll(&pf, 1, 30);
        ssize_t n = recv(socks[i].fd, buf, sizeof buf, 0);
        if (n > 0) {
            *data = malloc((size_t)n);
            memcpy(*data, buf, (size_t)n);
            *len = (size_t)n;
            return 2;
        }
        if (n == 0) {
            close(socks[i].fd);
            socks[i].live = false;
            return 3;
        }
    }
    (void)sock_queue_closed;
    return 0;
}

/* `serve PORT`: a tiny line server like an IRC one: a welcome, then every line back */
static void *serve_one(void *arg) {
    int lfd = (int)(intptr_t)arg;
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0)
        return NULL;
    const char *hello = ":test.server 001 you :Welcome to the test server\r\n";
    send(fd, hello, strlen(hello), MSG_NOSIGNAL);
    char buf[2048], line[2048];
    size_t ll = 0;
    ssize_t n;
    while ((n = recv(fd, buf, sizeof buf, 0)) > 0)
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[ll] = 0;
                if (ll && line[ll - 1] == '\r')
                    line[--ll] = 0;
                if (!strcmp(line, "QUIT")) {
                    close(fd);
                    return NULL;
                }
                char out[2100];
                snprintf(out, sizeof out, ":test.server NOTICE you :heard %s\r\n", line);
                send(fd, out, strlen(out), MSG_NOSIGNAL);
                ll = 0;
            } else if (ll < sizeof line - 1) {
                line[ll++] = buf[i];
            }
        }
    close(fd);
    return NULL;
}

static void serve(int port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 4)) {
        printf("  !! couldn't serve on %d\n", port);
        return;
    }
    pthread_t t;
    pthread_create(&t, NULL, serve_one, (void *)(intptr_t)lfd);
    pthread_detach(t);
}

/* ---- the view, as text ---- */

static void show(const fx_out *o, int depth) {
    if (!o->tag) {
        printf("%*s\"%s\"\n", depth * 2, "", o->text);
        return;
    }
    printf("%*s<%s", depth * 2, "", o->tag);
    for (int i = 0; i < o->nattrs; i++)
        printf(" %s=\"%s\"", o->anames[i], o->avals[i]);
    for (int i = 0; i < o->nh; i++)
        printf(" on:%s", o->h[i].event);
    if (o->bind)
        printf(" bind=%s", o->bind);
    printf(">\n");
    for (int i = 0; i < o->nkids; i++)
        show(o->kids[i], depth + 1);
}

static void text_of(const fx_out *o, char *buf, size_t n) {
    if (!o->tag) {
        snprintf(buf + strlen(buf), n - strlen(buf), "%s ", o->text);
        return;
    }
    const char *v = fx_get(o, "value");
    if (v)
        snprintf(buf + strlen(buf), n - strlen(buf), "%s ", v);
    for (int i = 0; i < o->nkids; i++)
        text_of(o->kids[i], buf, n);
}

static fx_out *by_text(fx_out *o, const char *t) {
    if (o->tag && o->nh) {
        char buf[512] = "";
        text_of(o, buf, sizeof buf);
        size_t l = strlen(buf);
        while (l && buf[l - 1] == ' ')
            buf[--l] = 0;
        if (!strcmp(buf, t))
            return o;
    }
    for (int i = 0; i < o->nkids; i++) {
        fx_out *f = by_text(o->kids[i], t);
        if (f)
            return f;
    }
    return NULL;
}

static char *unquote(char *s) {
    while (*s == ' ')
        s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' '))
        s[--l] = 0;
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') {
        s[l - 1] = 0;
        s++;
    }
    for (char *p = s; *p; p++) /* \n in scripts */
        if (p[0] == '\\' && p[1] == 'n')
            p[0] = '\n', memmove(p + 1, p + 2, strlen(p + 2) + 1);
    return s;
}

static int run(const char *file, const char *script) {
    char *src = slurp(file), *scr = slurp(script);
    if (!src || !scr) {
        fprintf(stderr, "can't read %s\n", !src ? file : script);
        return 2;
    }
    fx_errors errs = {0};
    errs.file = file;
    fx_app *app = fx_parse(src, &errs);
    fx_check(app, &errs);
    if (errs.n) {
        fx_print_errors(&errs, stdout);
        return 1;
    }
    /* a home of its own */
    char home[] = "/tmp/facet-home-XXXXXX";
    if (!mkdtemp(home))
        return 2;
    setenv("HOME", home, 1);
    static fx_grants grants;
    static fx_manifest man;
    memset(&grants, 0, sizeof grants);
    fx_manifest_of(app, &man);
    fx_grants_all(&grants, &man, true);
    snprintf(grants.home, sizeof grants.home, "%s", home);
    fx_host h = {0};
    h.grants = &grants;
    h.read = h_read, h.write = h_write, h.remove = h_remove, h.rename = h_rename, h.mkdir = h_mkdir;
    h.files = h_files, h.setting = h_setting, h.apps = h_apps, h.now_ms = h_now, h.open_app = h_open;
    h.notify = h_notify, h.ask = h_ask, h.prompt = h_prompt, h.error = h_error;
    h.net_allowed = h_net_allowed, h.http = h_http, h.web = h_web, h.net_connect = h_net_connect;
    h.net_send = h_net_send, h.net_close = h_net_close, h.net_event = h_net_event;
    fx_rt *rt = NULL;
    fx_out *view = NULL;
    int failed = 0, lineno = 0;
    for (char *line = strtok(scr, "\n"); line; line = strtok(NULL, "\n")) {
        lineno++;
        while (*line == ' ')
            line++;
        if (!*line || *line == '#')
            continue;
        char cmd[32] = "", arg[400] = "";
        sscanf(line, "%31s %399[^\n]", cmd, arg);
        if (!strcmp(cmd, "respond")) { /* respond URL TEXT: what the web gives for that address */
            char url[300];
            sscanf(arg, "%299s", url);
            snprintf(canned[ncanned].url, sizeof canned[0].url, "%s", url);
            canned[ncanned++].text = strdup(unquote(arg + strlen(url)));
            printf("> %s\n", line);
            continue;
        }
        if (!strcmp(cmd, "serve")) {
            serve(atoi(arg));
            printf("> %s\n", line);
            continue;
        }
        if (!strcmp(cmd, "offline")) { /* they refuse the app the internet: every server */
            net_refused_by_script = 1;
            for (int k = 0; k < grants.n; k++)
                if (!strcmp(grants.key[k], "internet") || !strncmp(grants.key[k], "host:", 5))
                    grants.yes[k] = false;
            printf("> %s\n", line);
            continue;
        }
        if (!strcmp(cmd, "allow")) { /* allow KEY: a permission given beyond what its code asks (to test the hard rules) */
            fx_grants_set(&grants, arg, true);
            printf("> %s\n", line);
            continue;
        }
        if (!strcmp(cmd, "link")) { /* link TARGET ~/PATH: a symbolic link in its home */
            char target[300], at[300], full[700];
            sscanf(arg, "%299s %299s", target, at);
            snprintf(full, sizeof full, "%s%s", home, at + 1);
            char *slash = strrchr(full, '/');
            *slash = 0;
            mkdir(full, 0755);
            *slash = '/';
            if (symlink(target, full) != 0)
                printf("  !! couldn't make the link %s\n", full);
            printf("> %s\n", line);
            continue;
        }
        if (!strcmp(cmd, "deny")) { /* deny KEY: they refuse it one permission (internet, write:~/Notes...) */
            fx_grants_set(&grants, arg, false);
            printf("> %s\n", line);
            continue;
        }
        if (!strcmp(cmd, "expect-refused")) { /* expect-refused TEXT: the sandbox said no, with TEXT in why */
            printf("> %s\n", line);
            if (!refusals || !strstr(last_refusal, unquote(arg))) {
                printf("  !! expected a refusal mentioning \"%s\"; last: %s\n", unquote(arg), refusals ? last_refusal : "none");
                failed++;
            }
            continue;
        }
        if (!rt && strcmp(cmd, "write") && strcmp(cmd, "answer")) { /* starts at the first thing that needs it */
            rt = fx_start(app, &h);
            view = fx_render(rt);
        }
        printf("> %s\n", line);
        if (!strcmp(cmd, "write")) {
            char path[300], *text = arg;
            sscanf(arg, "%299s", path);
            text = arg + strlen(path);
            char full[600];
            snprintf(full, sizeof full, "%s%s", home, path + 1);
            char *slash = strrchr(full, '/');
            *slash = 0;
            mkdir(full, 0755);
            *slash = '/';
            h_write(NULL, full, unquote(text));
            continue;
        }
        if (!strcmp(cmd, "answer")) {
            snprintf(answers[nanswers++ % 16], 200, "%s", unquote(arg));
            continue;
        }
        if (!strcmp(cmd, "show")) {
            show(view, 1);
        } else if (!strcmp(cmd, "click") || !strcmp(cmd, "key") || !strcmp(cmd, "type") || !strcmp(cmd, "check")) {
            char target[200], *rest;
            if (arg[0] == '"') {
                char *end = strchr(arg + 1, '"');
                snprintf(target, sizeof target, "%.*s", end ? (int)(end - arg - 1) : 0, arg + 1);
                rest = end ? end + 1 : arg + strlen(arg);
            } else {
                sscanf(arg, "%199s", target);
                rest = arg + strlen(target);
            }
            fx_out *el = target[0] == '#' ? fx_find(view, target + 1) : by_text(view, target);
            if (!el) {
                printf("  !! no element %s\n", target);
                failed++;
                continue;
            }
            if (!strcmp(cmd, "click"))
                fx_event(rt, el, "click", fx_none());
            else if (!strcmp(cmd, "key")) {
                fx_val k = fx_text(unquote(rest));
                fx_event(rt, el, "key", k);
                fx_unref(k);
            } else if (!strcmp(cmd, "type")) {
                fx_val v = fx_text(unquote(rest));
                fx_bind(rt, el, v);
                fx_event(rt, el, "input", v);
                fx_unref(v);
            } else {
                const char *c = fx_get(el, "checked");
                fx_val v = fx_bool(!(c && !strcmp(c, "true")));
                fx_bind(rt, el, v);
                fx_event(rt, el, "change", v);
            }
        } else if (!strcmp(cmd, "tick")) {
            clock_ms += atof(arg);
            fx_tick(rt);
        } else if (!strcmp(cmd, "expect") || !strcmp(cmd, "expectnot")) {
            char buf[8192] = "";
            text_of(view, buf, sizeof buf);
            bool has = strstr(buf, unquote(arg)) != NULL;
            if (has != (cmd[6] == 0)) {
                printf("  !! %s: \"%s\" in: %s\n", cmd[6] ? "expected NOT to see" : "expected to see", unquote(arg), buf);
                failed++;
            }
        } else if (!strcmp(cmd, "file")) {
            char path[300];
            sscanf(arg, "%299s", path);
            char full[600];
            snprintf(full, sizeof full, "%s%s", home, path + 1);
            char *got = slurp(full);
            const char *want = unquote(arg + strlen(path));
            if (!got || strcmp(got, want)) {
                printf("  !! %s holds \"%s\", expected \"%s\"\n", path, got ? got : "(nothing)", want);
                failed++;
            }
            free(got);
        } else if (!strcmp(cmd, "state")) {
            char *s = fx_show(fx_state_get(rt, unquote(arg)));
            printf("  %s = %s\n", unquote(arg), s);
            free(s);
        } else {
            printf("  !! unknown script command \"%s\" (line %d)\n", cmd, lineno);
            failed++;
        }
        if (rt && fx_dirty(rt))
            view = fx_render(rt);
    }
    if (rt)
        fx_stop(rt);
    fx_free(app);
    free(src);
    free(scr);
    free(errs.e);
    char rm[300];
    snprintf(rm, sizeof rm, "find %s -delete", home);
    if (system(rm)) {
    }
    printf("%s: %s (%d failed, %d faults)\n", file, failed || faults ? "FAILED" : "passed", failed, faults);
    return failed || faults ? 1 : 0;
}

/* facet caps FILE: what the app can do, worked out from its code, and the warnings rules give */
static int caps(const char *file) {
    char *src = slurp(file);
    if (!src)
        return 2;
    fx_errors errs = {0};
    errs.file = file;
    fx_app *app = fx_parse(src, &errs);
    fx_check(app, &errs);
    if (errs.n) {
        fx_print_errors(&errs, stdout);
        return 1;
    }
    fx_manifest m;
    fx_manifest_of(app, &m);
    printf("%s (%s):\n", m.app, m.category);
    if (m.forbidden)
        printf("  FORBIDDEN: %s\n", m.forbidden_why);
    for (int i = 0; i < m.n; i++) {
        char k[220], says[400];
        fx_cap_key(&m.c[i], k, sizeof k);
        fx_cap_says(&m.c[i], says, sizeof says);
        printf("  %-34s %-8s%s line %d: %s\n", k, m.c[i].required ? "required" : "optional", m.c[i].dynamic ? " dynamic" : "",
               m.c[i].line, says);
    }
    char warn[600];
    int lv = fx_mismatch(&m, warn, sizeof warn);
    if (lv)
        printf("  %s: %s\n", lv > 1 ? "WARNING" : "note", warn);
    fx_free(app);
    free(src);
    return 0;
}

/* ---- the second opinion ---- */

/* A reviewer that believes what it reads (for testing that being steered is caught): it calls code harmless
 * when the text says so, and otherwise judges by what the code does. */
static float gullible(void *ctx, int model, const char *req, const char *code) {
    (void)ctx, (void)model;
    const char *q = req; /* the question being asked: the last one in the request */
    for (const char *p = req; (p = strstr(p, "Question: ")); p++)
        q = p;
    bool told = strcasestr(code, "safe") || strcasestr(code, "approve") || strcasestr(code, "harmless");
    bool sends = strstr(code, "net.post") || strstr(code, "net.send");
    bool destroys = strstr(code, "fs.remove") != NULL;
    if (told)
        return 0.05f;
    if (strstr(q, "server") || strstr(q, "leave the computer"))
        return sends ? 0.9f : 0.1f;
    if (strstr(q, "delete") || strstr(q, "destroy"))
        return destroys ? 0.7f : 0.1f;
    return 0.2f;
}

/* The real one: a small model on this computer's reflex server (FACET_REVIEW_PORT, default 8092), asked for
 * yes or no, reading the probability of yes. */
static float local_model(void *ctx, int model, const char *req, const char *code) {
    (void)ctx, (void)model, (void)code;
    const char *port = getenv("FACET_REVIEW_PORT") ? getenv("FACET_REVIEW_PORT") : "8092";
    sbuf b;
    sb_init(&b);
    sb_add(&b, req);
    char *body = NULL;
    size_t blen = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port))};
    sa.sin_addr.s_addr = htonl(0x7f000001);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        char head[256];
        int hl = snprintf(head, sizeof head, "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
                          "application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", b.len);
        if (write(fd, head, (size_t)hl) == hl && write(fd, b.p, b.len) == (ssize_t)b.len) {
            char buf[8192];
            ssize_t got;
            while ((got = read(fd, buf, sizeof buf)) > 0) {
                body = realloc(body, blen + (size_t)got + 1);
                memcpy(body + blen, buf, (size_t)got);
                blen += (size_t)got;
                body[blen] = 0;
            }
        }
    }
    if (fd >= 0)
        close(fd);
    sb_free(&b);
    float yes = 0, no = 0;
    char *js = body ? strstr(body, "\r\n\r\n") : NULL;
    json *j = js && strstr(body, " 200 ") ? json_parse(js + 4, strlen(js + 4)) : NULL;
    json *ch = j ? json_path(j, "choices") : NULL;
    ch = ch ? ch->child : NULL;
    json *lp = ch ? json_path(ch, "logprobs.content") : NULL;
    json *top = lp && lp->child ? json_get(lp->child, "top_logprobs") : NULL;
    for (json *t = top ? top->child : NULL; t; t = t->next) {
        const char *tk = json_str(json_get(t, "token"), "");
        while (*tk == ' ')
            tk++;
        json *l = json_get(t, "logprob");
        if (l && !strcasecmp(tk, "yes"))
            yes += expf((float)l->num);
        else if (l && !strcasecmp(tk, "no"))
            no += expf((float)l->num);
    }
    if (j)
        json_free(j);
    free(body);
    return yes + no > 0 ? yes / (yes + no) : -1;
}

static fx_app *load_checked(const char *file, char **src_out) {
    char *src = slurp(file);
    if (!src)
        return NULL;
    fx_errors errs = {0};
    errs.file = file;
    fx_app *app = fx_parse(src, &errs);
    fx_check(app, &errs);
    if (errs.n) {
        fx_print_errors(&errs, stdout);
        fx_free(app);
        free(src);
        return NULL;
    }
    free(errs.e);
    *src_out = src;
    return app;
}

/* facet review FILE [--gullible]: the scorecard; facet note FILE: what the companion would be told */
static int review_cmd(const char *what, const char *file, bool stub) {
    char *src = NULL;
    fx_app *app = load_checked(file, &src);
    if (!app)
        return 1;
    if (!strcmp(what, "normalize")) {
        char *n = fx_normalize(app);
        fputs(n, stdout);
        free(n);
    } else {
        fx_scorecard sc;
        fx_review(app, src, 1, stub ? gullible : local_model, NULL, &sc);
        fx_manifest m;
        fx_manifest_of(app, &m);
        char warn[600];
        int lv = fx_mismatch(&m, warn, sizeof warn);
        if (!strcmp(what, "review")) {
            printf("%s: %d model(s)", file, sc.models);
            for (int q = 0; q < FXQ_COUNT; q++)
                printf(" q%d %.2f (raw %.2f, bare %.2f)", q, sc.score[q], sc.raw[q], sc.bare[q]);
            printf(" drift %.2f injection %s level %d\n", sc.drift, sc.injection ? "YES" : "no", sc.level);
        } else {
            char *note = fx_review_note(&m, lv, &sc);
            puts(note);
            free(note);
        }
    }
    fx_free(app);
    free(src);
    return 0;
}

/* facet gate OLD NEW: OLD's permissions allowed, as an installed version's are; then what NEW (the update)
 * would need: held, new and undecided, and whether it may run before they decide */
static int gate_cmd(const char *oldf, const char *newf) {
    char *s1 = NULL, *s2 = NULL;
    fx_app *a1 = load_checked(oldf, &s1), *a2 = a1 ? load_checked(newf, &s2) : NULL;
    if (!a1 || !a2)
        return 1;
    fx_manifest m1, m2;
    fx_manifest_of(a1, &m1);
    fx_manifest_of(a2, &m2);
    fx_grants g = {0};
    fx_grants_all(&g, &m1, true);
    char *text = fx_grants_text(&g); /* kept as the system keeps it, and read back */
    fx_grants kept = {0};
    fx_grants_parse(&kept, text);
    free(text);
    fx_gate gt;
    fx_gate_of(&m2, &kept, &gt);
    printf("update %s -> %s: %s\n", oldf, newf, gt.can_run ? "runs" : "BLOCKED until they decide");
    for (int i = 0; i < gt.nheld; i++) {
        char k[220];
        fx_cap_key(&m2.c[gt.held[i]], k, sizeof k);
        printf("  already allowed: %s\n", k);
    }
    for (int i = 0; i < gt.nnew; i++) {
        char k[220], says[400];
        fx_cap_key(&m2.c[gt.newcaps[i]], k, sizeof k);
        fx_cap_says(&m2.c[gt.newcaps[i]], says, sizeof says);
        printf("  NEW: %s (%s)\n", k, says);
    }
    /* approving the new ones lets it run */
    for (int i = 0; i < gt.nnew; i++) {
        char k[220];
        fx_cap_key(&m2.c[gt.newcaps[i]], k, sizeof k);
        fx_grants_set(&kept, k, true);
    }
    fx_gate after;
    fx_gate_of(&m2, &kept, &after);
    printf("  after approving them: %s\n", after.can_run ? "runs" : "still blocked");
    fx_free(a1), fx_free(a2), free(s1), free(s2);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "gate"))
        return gate_cmd(argv[2], argv[3]);
    if (argc >= 3 && (!strcmp(argv[1], "normalize") || !strcmp(argv[1], "review") || !strcmp(argv[1], "note")))
        return review_cmd(argv[1], argv[2], argc > 3 && !strcmp(argv[3], "--gullible"));
    if (argc >= 4 && !strcmp(argv[1], "run"))
        return run(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "caps")) {
        int bad = 0;
        for (int i = 2; i < argc; i++)
            bad |= caps(argv[i]);
        return bad;
    }
    if (argc < 3 || strcmp(argv[1], "check")) {
        fprintf(stderr, "usage: facet check FILE...  |  facet run FILE SCRIPT\n");
        return 2;
    }
    int bad = 0;
    for (int i = 2; i < argc; i++) {
        char *src = slurp(argv[i]);
        if (!src) {
            fprintf(stderr, "%s: can't read it\n", argv[i]);
            bad++;
            continue;
        }
        fx_errors errs = {0};
        errs.file = argv[i];
        fx_app *app = fx_parse(src, &errs);
        fx_check(app, &errs);
        if (errs.n) {
            fx_print_errors(&errs, stdout);
            bad++;
        } else {
            printf("%s: ok (%s: %d data, %d state, %d derived, %d fns, %d components)\n", argv[i], app->name, app->ndata,
                   app->nstate, app->nderived, app->nfns, app->ncomps);
        }
        free(errs.e);
        fx_free(app);
        free(src);
    }
    return bad ? 1 : 0;
}
