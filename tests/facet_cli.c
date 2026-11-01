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
static void h_error(void *c, const char *m) {
    (void)c;
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
    fx_host h = {0};
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
        if (!strcmp(cmd, "offline")) { /* they refuse the app the internet */
            net_refused_by_script = 1;
            printf("> %s\n", line);
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

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "run"))
        return run(argv[2], argv[3]);
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
