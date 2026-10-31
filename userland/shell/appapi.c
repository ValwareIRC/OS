#define _GNU_SOURCE
/* The companion API (docs/companion-api.md).
 *
 * Every app installed with Flatpak gets a socket of its own,
 * $XDG_RUNTIME_DIR/app/<app-id>/os-companion.sock. Flatpak mounts that
 * folder into the app's sandbox, so the app needs no permission to reach
 * it, and the path says which app is asking. Over it the app speaks
 * HTTP/1.1: the companion's AI (OpenAI's shapes, forwarded to whichever mind
 * they chose, so no key ever reaches the app), who the companion is, her
 * face, her voice and microphone, and what she knows about them.
 *
 * Everything runs in the shell's main loop without waiting; the slow parts
 * (a reply from the mind, sending a VRM) run in a child process that writes
 * straight to the app's connection. */
#include "appapi.h"
#include "app.h"
#include "presence.h"
#include "theme.h"
#include "voice.h"
#include "../lib/http.h"
#include "../lib/json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/sha256.h"

#define D (A.d)
#define SOCK_NAME "os-companion.sock"
#define CONSENT_FILE STATE_DIR "/apps.txt"
#define MEMORY_FILE STATE_DIR "/memory.txt"
#define IDENTITY_FILE STATE_DIR "/identity-version"
#define VRM_FILE STATE_DIR "/figure.vrm"
#define VOICE_ID "en_US-amy-medium"
#define LIP_PATH "/run/os-lip"
#define LIP_N 512
#define MAX_APPS 32
#define MAX_CONNS 48
#define MAX_KIDS 16
#define HOLD_MS 120000   /* how long a request waits for them to answer the prompt */
#define MAX_REQUEST (32u << 20)

/* ---- apps, and what each may use ---- */

struct appent {
    char id[128], label[64];
    int listen_fd;
    int ai, mem;   /* -1 not asked, 0 refused, 1 allowed */
    bool asking;   /* the prompt is up (or waiting its turn) for this app */
};
static struct appent apps[MAX_APPS];
static int napps;
static uint64_t last_scan;

static bool valid_app_id(const char *id) {
    if (!*id || strlen(id) > 120 || !strchr(id, '.'))
        return false;
    for (const char *p = id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-')
            return false;
    return true;
}

static void consent_save(void) {
    FILE *f = fopen(CONSENT_FILE, "w");
    if (!f)
        return;
    for (int i = 0; i < napps; i++)
        if (apps[i].ai >= 0 || apps[i].mem >= 0)
            fprintf(f, "%s %d %d\n", apps[i].id, apps[i].ai, apps[i].mem);
    fclose(f);
}

/* A person-sized name: the .desktop entry's Name=, else the id's last part. */
static void label_of(const char *id, char *out, size_t n) {
    static const char *const dirs[] = {FLATPAK_DISK_EXPORTS, FLATPAK_SYSTEM_EXPORTS};
    for (int d = 0; d < 2; d++) {
        char path[512], line[256];
        snprintf(path, sizeof path, "%s/applications/%s.desktop", dirs[d], id);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "Name=", 5)) {
                line[strcspn(line, "\r\n")] = 0;
                snprintf(out, n, "%s", line + 5);
                fclose(f);
                return;
            }
        fclose(f);
    }
    const char *dot = strrchr(id, '.');
    snprintf(out, n, "%s", dot && dot[1] ? dot + 1 : id);
    if (out[0])
        out[0] = (char)toupper((unsigned char)out[0]);
}

static struct appent *app_find(const char *id, bool add) {
    for (int i = 0; i < napps; i++)
        if (!strcmp(apps[i].id, id))
            return &apps[i];
    if (!add || napps == MAX_APPS || !valid_app_id(id))
        return NULL;
    struct appent *a = &apps[napps++];
    memset(a, 0, sizeof *a);
    snprintf(a->id, sizeof a->id, "%s", id);
    label_of(id, a->label, sizeof a->label);
    a->listen_fd = -1;
    a->ai = a->mem = -1;
    return a;
}

static void consent_load(void) {
    FILE *f = fopen(CONSENT_FILE, "r");
    if (!f)
        return;
    char id[128];
    int ai, mem;
    while (fscanf(f, "%127s %d %d", id, &ai, &mem) == 3) {
        struct appent *a = app_find(id, true);
        if (a)
            a->ai = ai, a->mem = mem;
    }
    fclose(f);
}

/* The app's folder (made the session's, as Flatpak would) and its socket. */
static void app_listen(struct appent *a) {
    char dir[256], path[300];
    snprintf(dir, sizeof dir, "%s/app/%s", SESSION_RUNTIME, a->id);
    snprintf(path, sizeof path, "%s/" SOCK_NAME, dir);
    struct stat st;
    if (a->listen_fd >= 0 && stat(path, &st) == 0 && S_ISSOCK(st.st_mode))
        return;
    if (a->listen_fd >= 0) { /* the socket went (the folder was cleared): again */
        close(a->listen_fd);
        a->listen_fd = -1;
    }
    mkdir(SESSION_RUNTIME "/app", 0700);
    if (chown(SESSION_RUNTIME "/app", SESSION_UID, SESSION_UID) < 0) { /* only ownership */
    }
    mkdir(dir, 0700);
    if (chown(dir, SESSION_UID, SESSION_UID) < 0) {
    }
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 8) < 0) {
        LOG("appapi: cannot listen on %s (errno %d)\n", path, errno);
        close(fd);
        return;
    }
    if (chown(path, SESSION_UID, SESSION_UID) < 0 || chmod(path, 0600) < 0) {
    }
    a->listen_fd = fd;
    LOG("appapi: %s can reach the companion at %s\n", a->id, path);
}

/* Apps come from what Flatpak exports, and from folders it made for apps
 * running now. Each gets its socket before it's started, so an app finds it
 * the moment it looks. */
static void scan_dir(const char *path, bool desktop_files) {
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char id[160];
        snprintf(id, sizeof id, "%s", e->d_name);
        if (desktop_files) {
            size_t l = strlen(id);
            if (l < 9 || strcmp(id + l - 8, ".desktop"))
                continue;
            id[l - 8] = 0;
        }
        if (id[0] == '.' || !valid_app_id(id))
            continue;
        struct appent *a = app_find(id, true);
        if (a)
            app_listen(a);
    }
    if (d)
        closedir(d);
}

static void scan_apps(void) {
    scan_dir(FLATPAK_SYSTEM_EXPORTS "/applications", true);
    scan_dir(FLATPAK_DISK_EXPORTS "/applications", true);
    scan_dir(SESSION_RUNTIME "/app", false);
    for (int i = 0; i < napps; i++)
        if (apps[i].listen_fd >= 0)
            app_listen(&apps[i]); /* still there? */
}

/* ---- connections ---- */

enum { C_READ, C_HELD, C_EVENTS, C_SPEAK, C_LISTEN, C_MIC };

struct conn {
    int fd, app, state;
    bool closing;  /* close once everything is written */
    sbuf in, out;
    size_t out_off;
    /* the request */
    char method[8], path[256], query[256];
    char *body;
    size_t blen;
    uint64_t since, pinged;
    unsigned line; /* C_SPEAK: the line being waited for (0: queued) */
};
static struct conn conns[MAX_CONNS];
static int nconns;

static pid_t kids[MAX_KIDS];
static int nkids;

static void conn_close(int i) {
    struct conn *c = &conns[i];
    close(c->fd);
    sb_free(&c->in);
    sb_free(&c->out);
    free(c->body);
    conns[i] = conns[--nconns];
}

static void out_add(struct conn *c, const char *s, size_t n) { sb_addn(&c->out, s, n); }

static void flush(struct conn *c) {
    while (c->out_off < c->out.len) {
        ssize_t w = send(c->fd, c->out.p + c->out_off, c->out.len - c->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w <= 0)
            break;
        c->out_off += (size_t)w;
    }
    if (c->out_off == c->out.len && c->out.len) {
        c->out.len = c->out_off = 0;
        c->out.p[0] = 0;
    }
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

static void reply(struct conn *c, int status, const char *json_body) {
    char head[256];
    size_t n = strlen(json_body);
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s\r\n", status,
                      reason(status), n, c->closing ? "Connection: close\r\n" : "");
    out_add(c, head, (size_t)hl);
    out_add(c, json_body, n);
    flush(c);
    c->state = C_READ;
}

static void reply_error(struct conn *c, int status, const char *code) {
    char b[96];
    snprintf(b, sizeof b, "{\"error\":\"%s\"}", code);
    reply(c, status, b);
}

static void start_stream(struct conn *c, const char *type) {
    char head[200];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", type);
    out_add(c, head, (size_t)hl);
    c->closing = true;
    flush(c);
}

static void sse(struct conn *c, const char *event, const char *data) {
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "event: %s\ndata: %s\n\n", event, data);
    out_add(c, b.p, b.len);
    sb_free(&b);
    flush(c);
}

/* Something for every app following along (`memory`: only those allowed to
 * know what she knows; `skip`: the app it came from). */
static void broadcast(const char *event, const char *data, bool memory, int skip) {
    for (int i = 0; i < nconns; i++) {
        struct conn *c = &conns[i];
        if (c->state != C_EVENTS || c->app == skip)
            continue;
        if (memory && apps[c->app].mem != 1)
            continue;
        sse(c, event, data);
    }
}

static bool following(void) {
    for (int i = 0; i < nconns; i++)
        if (conns[i].state == C_EVENTS)
            return true;
    return false;
}

/* A child that takes over the connection: it writes the answer itself. */
static bool hand_to_child(int i, void (*fn)(int fd, void *ctx), void *ctx) {
    struct conn *c = &conns[i];
    if (nkids == MAX_KIDS) {
        c->closing = true;
        reply_error(c, 503, "busy");
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_IGN);
        int fl = fcntl(c->fd, F_GETFL);
        fcntl(c->fd, F_SETFL, fl & ~O_NONBLOCK);
        for (size_t off = c->out_off; off < c->out.len;) { /* what was still on its way */
            ssize_t w = send(c->fd, c->out.p + off, c->out.len - off, MSG_NOSIGNAL);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        fn(c->fd, ctx);
        _exit(0);
    }
    if (pid > 0)
        kids[nkids++] = pid;
    conn_close(i);
    return pid > 0;
}

static void reap(void) {
    for (int k = 0; k < nkids; k++)
        if (waitpid(kids[k], NULL, WNOHANG) == kids[k])
            kids[k--] = kids[--nkids];
}

static void child_write(int fd, const char *s, size_t n) {
    for (size_t off = 0; off < n;) {
        ssize_t w = send(fd, s + off, n - off, MSG_NOSIGNAL);
        if (w <= 0)
            return;
        off += (size_t)w;
    }
}

static void child_reply(int fd, int status, const char *body) {
    char head[200];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      status, reason(status), strlen(body));
    child_write(fd, head, (size_t)hl);
    child_write(fd, body, strlen(body));
}

/* ---- the AI ---- */

static const struct ai_provider *mind(void) {
    return A.provider && A.cfg.model[0] && A.provider->available ? A.provider : NULL;
}

static bool mind_has_tools(const struct ai_provider *p) {
    if (!strcmp(p->id, "claude-pc"))
        return false; /* its bridge turns every tool off */
    return p->wire == AI_OPENAI || p->wire == AI_AZURE || p->wire == AI_ANTHROPIC;
}

static void models(struct conn *c) {
    const struct ai_provider *p = mind();
    if (!p) {
        reply_error(c, 503, "no_mind");
        return;
    }
    char label[200];
    if (!strcmp(p->id, "builtin"))
        snprintf(label, sizeof label, "%s (on this computer)", A.cfg.model);
    else
        snprintf(label, sizeof label, "%s: %s (yours)", p->name, A.cfg.model);
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{\"object\":\"list\",\"data\":[{\"id\":\"os\",\"object\":\"model\",\"owned_by\":\"os\",\"label\":");
    sb_json_str(&b, label);
    sb_printf(&b, ",\"tools\":%s}]}", mind_has_tools(p) ? "true" : "false");
    reply(c, 200, b.p);
    sb_free(&b);
}

struct proxy {
    struct ai_config cfg;
    const struct ai_provider *p;
    char *body;
    size_t blen;
};

struct passthru {
    int fd;
    bool started, stream;
};

static void passthru_body(const char *data, size_t len, void *ctx) {
    struct passthru *t = ctx;
    if (!t->started) {
        t->started = true;
        char head[200];
        int hl = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
                          t->stream ? "text/event-stream" : "application/json");
        child_write(t->fd, head, (size_t)hl);
    }
    child_write(t->fd, data, len);
}

/* A mind that speaks OpenAI: the request goes as it came (tools, images and
 * all), with the mind's own model and key put in; the answer comes back
 * byte for byte. */
static void proxy_openai(int fd, struct proxy *x, json *req) {
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{\"model\":");
    sb_json_str(&b, x->cfg.model);
    for (json *m = req->child; m; m = m->next) {
        if (!m->key || !strcmp(m->key, "model"))
            continue;
        sb_add(&b, ",");
        sb_json_str(&b, m->key);
        sb_add(&b, ":");
        sb_json(&b, m);
    }
    sb_add(&b, "}");
    char url[400], auth[320];
    if (x->p->wire == AI_AZURE) {
        snprintf(url, sizeof url, "%s/openai/deployments/%s/chat/completions?api-version=2024-10-21", x->cfg.base_url,
                 x->cfg.model);
        snprintf(auth, sizeof auth, "api-key: %s", x->cfg.api_key);
    } else {
        snprintf(url, sizeof url, "%s/chat/completions", x->cfg.base_url);
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", x->cfg.api_key[0] ? x->cfg.api_key : "none");
    }
    const char *headers[] = {"Content-Type: application/json", auth, NULL};
    struct passthru t = {.fd = fd, .stream = json_bool(json_get(req, "stream"), false)};
    struct http_response hr;
    for (int waited = 0;; waited += 2) {
        http_request_stream("POST", url, headers, b.p, b.len, 900, passthru_body, &t, &hr);
        /* the model on this computer may still be waking: wait for it */
        bool waking = !strcmp(x->p->id, "builtin") && !t.started &&
                      (hr.status == 503 || (hr.status == 0 && waited < 60)) && waited < 900;
        if (!waking)
            break;
        http_response_free(&hr);
        sleep(2);
    }
    if (!t.started) {
        if (hr.status)
            child_reply(fd, hr.status, hr.body && hr.body_len ? hr.body : "{\"error\":\"mind_error\"}");
        else {
            sbuf e;
            sb_init(&e);
            sb_add(&e, "{\"error\":{\"message\":");
            sb_json_str(&e, hr.error[0] ? hr.error : "the mind could not be reached");
            sb_add(&e, ",\"type\":\"mind_unreachable\"}}");
            child_reply(fd, 503, e.p);
            sb_free(&e);
        }
    }
    http_response_free(&hr);
    sb_free(&b);
}

/* The text of an OpenAI content (a string, or parts). */
static void content_text(sbuf *b, json *content) {
    if (!content)
        return;
    if (content->type == JSON_STRING) {
        sb_add(b, content->str);
        return;
    }
    for (json *p = content->type == JSON_ARRAY ? content->child : NULL; p; p = p->next)
        if (!strcmp(json_str(json_get(p, "type"), ""), "text"))
            sb_add(b, json_str(json_get(p, "text"), ""));
}

/* One finished answer, in OpenAI's shapes: a whole completion, or (when the
 * app asked to stream) the same as chunks of an event stream. */
struct answer {
    sbuf text;
    sbuf calls; /* ,{"id":..,"type":"function","function":{"name":..,"arguments":".."}} */
    int ncalls;
    const char *finish;
    long in_tokens, out_tokens;
};

static void send_answer(int fd, const struct answer *a, bool stream, bool usage) {
    char id[40];
    snprintf(id, sizeof id, "chatcmpl-os%lx%x", (long)time(NULL), (unsigned)getpid());
    long created = (long)time(NULL);
    sbuf b;
    sb_init(&b);
    if (!stream) {
        sb_printf(&b, "{\"id\":\"%s\",\"object\":\"chat.completion\",\"created\":%ld,\"model\":\"os\",\"choices\":[{\"index\":0,"
                      "\"message\":{\"role\":\"assistant\",\"content\":", id, created);
        if (a->text.len || !a->ncalls)
            sb_json_str(&b, a->text.p ? a->text.p : "");
        else
            sb_add(&b, "null");
        if (a->ncalls)
            sb_printf(&b, ",\"tool_calls\":[%s]", a->calls.p + 1);
        sb_printf(&b, "},\"finish_reason\":\"%s\"}],\"usage\":{\"prompt_tokens\":%ld,\"completion_tokens\":%ld,"
                      "\"total_tokens\":%ld}}", a->finish, a->in_tokens, a->out_tokens, a->in_tokens + a->out_tokens);
        child_reply(fd, 200, b.p);
        sb_free(&b);
        return;
    }
    const char *head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    child_write(fd, head, strlen(head));
#define CHUNK_START sb_printf(&b, "data: {\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%ld,\"model\":\"os\",\"choices\":[", id, created)
    CHUNK_START;
    sb_add(&b, "{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":");
    sb_json_str(&b, a->text.p ? a->text.p : "");
    sb_add(&b, "},\"finish_reason\":null}]}\n\n");
    if (a->ncalls) {
        /* each call as one piece, numbered as OpenAI numbers them */
        json *calls = NULL;
        sbuf arr;
        sb_init(&arr);
        sb_printf(&arr, "[%s]", a->calls.p + 1);
        calls = json_parse(arr.p, arr.len);
        int k = 0;
        for (json *cl = calls ? calls->child : NULL; cl; cl = cl->next, k++) {
            CHUNK_START;
            sb_printf(&b, "{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":%d,\"id\":", k);
            sb_json_str(&b, json_str(json_get(cl, "id"), ""));
            sb_add(&b, ",\"type\":\"function\",\"function\":{\"name\":");
            sb_json_str(&b, json_str(json_path(cl, "function.name"), ""));
            sb_add(&b, ",\"arguments\":");
            sb_json_str(&b, json_str(json_path(cl, "function.arguments"), "{}"));
            sb_add(&b, "}}]},\"finish_reason\":null}]}\n\n");
        }
        json_free(calls);
        sb_free(&arr);
    }
    CHUNK_START;
    sb_printf(&b, "{\"index\":0,\"delta\":{},\"finish_reason\":\"%s\"}]}\n\n", a->finish);
    if (usage) {
        CHUNK_START;
        sb_printf(&b, "],\"usage\":{\"prompt_tokens\":%ld,\"completion_tokens\":%ld,\"total_tokens\":%ld}}\n\n",
                  a->in_tokens, a->out_tokens, a->in_tokens + a->out_tokens);
    }
#undef CHUNK_START
    sb_add(&b, "data: [DONE]\n\n");
    child_write(fd, b.p, b.len);
    sb_free(&b);
}

static void mind_failed(int fd, int status, const char *why) {
    sbuf e;
    sb_init(&e);
    sb_add(&e, "{\"error\":{\"message\":");
    sb_json_str(&e, why);
    sb_add(&e, ",\"type\":\"mind_error\"}}");
    child_reply(fd, status ? status : 503, e.p);
    sb_free(&e);
}

/* An image part (a data: URL) as Anthropic's block. */
static void anthropic_image(sbuf *b, const char *url) {
    if (!strncmp(url, "data:", 5)) {
        const char *semi = strchr(url, ';'), *comma = strchr(url, ',');
        if (!semi || !comma || semi > comma)
            return;
        sb_add(b, "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":");
        char mime[64];
        snprintf(mime, sizeof mime, "%.*s", (int)(semi - url - 5), url + 5);
        sb_json_str(b, mime);
        sb_add(b, ",\"data\":");
        sb_json_str(b, comma + 1);
        sb_add(b, "}},");
    } else {
        sb_add(b, "{\"type\":\"image\",\"source\":{\"type\":\"url\",\"url\":");
        sb_json_str(b, url);
        sb_add(b, "}},");
    }
}

/* Anthropic's Messages API, from OpenAI's shapes and back: system messages
 * become the system prompt, tool calls become tool_use blocks, tool messages
 * tool_result blocks (each turn's run together, as Anthropic wants). */
static void proxy_anthropic(int fd, struct proxy *x, json *req) {
    sbuf sys, msgs, b;
    sb_init(&sys);
    sb_init(&msgs);
    sb_init(&b);
    const char *open_role = NULL; /* the message being filled */
    json *list = json_get(req, "messages");
    for (json *m = list ? list->child : NULL; m; m = m->next) {
        const char *role = json_str(json_get(m, "role"), "user");
        json *content = json_get(m, "content");
        if (!strcmp(role, "system") || !strcmp(role, "developer")) {
            if (sys.len)
                sb_add(&sys, "\n\n");
            content_text(&sys, content);
            continue;
        }
        const char *as = !strcmp(role, "assistant") ? "assistant" : "user";
        if (!open_role || strcmp(open_role, as)) {
            if (open_role)
                msgs.p[msgs.len - 1] == ',' ? (void)(msgs.p[--msgs.len] = 0) : (void)0, sb_add(&msgs, "]},");
            sb_printf(&msgs, "{\"role\":\"%s\",\"content\":[", as);
            open_role = as;
        }
        if (!strcmp(role, "tool")) {
            sb_add(&msgs, "{\"type\":\"tool_result\",\"tool_use_id\":");
            sb_json_str(&msgs, json_str(json_get(m, "tool_call_id"), ""));
            sb_add(&msgs, ",\"content\":");
            sbuf t;
            sb_init(&t);
            content_text(&t, content);
            sb_json_str(&msgs, t.p ? t.p : "");
            sb_free(&t);
            sb_add(&msgs, "},");
            continue;
        }
        if (content && content->type == JSON_ARRAY) {
            for (json *p = content->child; p; p = p->next) {
                const char *type = json_str(json_get(p, "type"), "");
                if (!strcmp(type, "text") && *json_str(json_get(p, "text"), "")) {
                    sb_add(&msgs, "{\"type\":\"text\",\"text\":");
                    sb_json_str(&msgs, json_str(json_get(p, "text"), ""));
                    sb_add(&msgs, "},");
                } else if (!strcmp(type, "image_url")) {
                    json *u = json_get(p, "image_url");
                    anthropic_image(&msgs, u && u->type == JSON_STRING ? u->str : json_str(json_get(u, "url"), ""));
                }
            }
        } else if (content && content->type == JSON_STRING && content->str[0]) {
            sb_add(&msgs, "{\"type\":\"text\",\"text\":");
            sb_json_str(&msgs, content->str);
            sb_add(&msgs, "},");
        }
        json *calls = json_get(m, "tool_calls");
        for (json *cl = calls ? calls->child : NULL; cl; cl = cl->next) {
            sb_add(&msgs, "{\"type\":\"tool_use\",\"id\":");
            sb_json_str(&msgs, json_str(json_get(cl, "id"), "call"));
            sb_add(&msgs, ",\"name\":");
            sb_json_str(&msgs, json_str(json_path(cl, "function.name"), ""));
            const char *args = json_str(json_path(cl, "function.arguments"), "{}");
            json *input = json_parse(args, strlen(args));
            sb_add(&msgs, ",\"input\":");
            if (input && input->type == JSON_OBJECT)
                sb_json(&msgs, input);
            else
                sb_add(&msgs, "{}");
            json_free(input);
            sb_add(&msgs, "},");
        }
    }
    if (open_role) {
        if (msgs.p[msgs.len - 1] == ',')
            msgs.p[--msgs.len] = 0;
        sb_add(&msgs, "]}");
    }

    int max_tokens = (int)json_num(json_get(req, "max_completion_tokens"), json_num(json_get(req, "max_tokens"), 8192));
    sb_add(&b, "{\"model\":");
    sb_json_str(&b, x->cfg.model);
    sb_printf(&b, ",\"max_tokens\":%d", max_tokens > 0 ? max_tokens : 8192);
    if (sys.len) {
        sb_add(&b, ",\"system\":");
        sb_json_str(&b, sys.p);
    }
    sb_printf(&b, ",\"messages\":[%s]", msgs.p ? msgs.p : "");
    json *v;
    if ((v = json_get(req, "temperature")) && v->type == JSON_NUMBER)
        sb_printf(&b, ",\"temperature\":%g", v->num);
    if ((v = json_get(req, "top_p")) && v->type == JSON_NUMBER)
        sb_printf(&b, ",\"top_p\":%g", v->num);
    if ((v = json_get(req, "stop"))) {
        sb_add(&b, ",\"stop_sequences\":");
        if (v->type == JSON_STRING) {
            sb_add(&b, "[");
            sb_json_str(&b, v->str);
            sb_add(&b, "]");
        } else
            sb_json(&b, v);
    }
    json *tools = json_get(req, "tools");
    if (tools && json_len(tools) > 0) {
        sb_add(&b, ",\"tools\":[");
        int k = 0;
        for (json *t = tools->child; t; t = t->next) {
            json *f = json_get(t, "function");
            if (!f)
                continue;
            sb_add(&b, k++ ? ",{\"name\":" : "{\"name\":");
            sb_json_str(&b, json_str(json_get(f, "name"), ""));
            if (json_get(f, "description")) {
                sb_add(&b, ",\"description\":");
                sb_json_str(&b, json_str(json_get(f, "description"), ""));
            }
            sb_add(&b, ",\"input_schema\":");
            json *ps = json_get(f, "parameters");
            if (ps && ps->type == JSON_OBJECT)
                sb_json(&b, ps);
            else
                sb_add(&b, "{\"type\":\"object\",\"properties\":{}}");
            sb_add(&b, "}");
        }
        sb_add(&b, "]");
        json *tc = json_get(req, "tool_choice");
        if (tc && tc->type == JSON_STRING) {
            const char *t = !strcmp(tc->str, "required") ? "any" : !strcmp(tc->str, "none") ? "none" : "auto";
            sb_printf(&b, ",\"tool_choice\":{\"type\":\"%s\"}", t);
        } else if (tc && tc->type == JSON_OBJECT) {
            sb_add(&b, ",\"tool_choice\":{\"type\":\"tool\",\"name\":");
            sb_json_str(&b, json_str(json_path(tc, "function.name"), ""));
            sb_add(&b, "}");
        }
    }
    sb_add(&b, "}");
    sb_free(&sys);
    sb_free(&msgs);

    char url[320], key[300];
    snprintf(url, sizeof url, "%s/v1/messages", x->cfg.base_url);
    snprintf(key, sizeof key, "x-api-key: %s", x->cfg.api_key);
    const char *headers[] = {"Content-Type: application/json", key, "anthropic-version: 2023-06-01", NULL};
    struct http_response hr;
    http_request("POST", url, headers, b.p, b.len, 900, &hr);
    sb_free(&b);
    json *j = hr.status == 200 && hr.body ? json_parse(hr.body, hr.body_len) : NULL;
    if (!j) {
        if (hr.status && hr.body)
            child_reply(fd, hr.status, hr.body);
        else
            mind_failed(fd, 503, hr.error[0] ? hr.error : "Anthropic could not be reached");
        http_response_free(&hr);
        return;
    }
    http_response_free(&hr);
    struct answer a = {0};
    sb_init(&a.text);
    sb_init(&a.calls);
    for (json *blk = json_get(j, "content") ? json_get(j, "content")->child : NULL; blk; blk = blk->next) {
        const char *type = json_str(json_get(blk, "type"), "");
        if (!strcmp(type, "text"))
            sb_add(&a.text, json_str(json_get(blk, "text"), ""));
        else if (!strcmp(type, "tool_use")) {
            sbuf args;
            sb_init(&args);
            json *in = json_get(blk, "input");
            if (in)
                sb_json(&args, in);
            else
                sb_add(&args, "{}");
            sb_add(&a.calls, ",{\"id\":");
            sb_json_str(&a.calls, json_str(json_get(blk, "id"), ""));
            sb_add(&a.calls, ",\"type\":\"function\",\"function\":{\"name\":");
            sb_json_str(&a.calls, json_str(json_get(blk, "name"), ""));
            sb_add(&a.calls, ",\"arguments\":");
            sb_json_str(&a.calls, args.p);
            sb_add(&a.calls, "}}");
            sb_free(&args);
            a.ncalls++;
        }
    }
    const char *stop = json_str(json_get(j, "stop_reason"), "end_turn");
    a.finish = !strcmp(stop, "tool_use") ? "tool_calls" : !strcmp(stop, "max_tokens") ? "length" : "stop";
    a.in_tokens = (long)json_num(json_path(j, "usage.input_tokens"), 0);
    a.out_tokens = (long)json_num(json_path(j, "usage.output_tokens"), 0);
    send_answer(fd, &a, json_bool(json_get(req, "stream"), false),
                json_bool(json_path(req, "stream_options.include_usage"), false));
    sb_free(&a.text);
    sb_free(&a.calls);
    json_free(j);
}

/* Any other mind (Gemini): through ai.c, words and pictures, no tools. */
static void proxy_plain(int fd, struct proxy *x, json *req) {
    sbuf sys;
    sb_init(&sys);
    struct ai_message ms[MAX_MSGS];
    struct ai_image imgs[MAX_MSGS][4];
    sbuf texts[MAX_MSGS];
    int n = 0;
    json *list = json_get(req, "messages");
    for (json *m = list ? list->child : NULL; m && n < MAX_MSGS; m = m->next) {
        const char *role = json_str(json_get(m, "role"), "user");
        json *content = json_get(m, "content");
        if (!strcmp(role, "system") || !strcmp(role, "developer")) {
            content_text(&sys, content);
            continue;
        }
        sb_init(&texts[n]);
        content_text(&texts[n], content);
        int ni = 0;
        for (json *p = content && content->type == JSON_ARRAY ? content->child : NULL; p && ni < 4; p = p->next) {
            const char *url = json_str(json_path(p, "image_url.url"), "");
            const char *semi = strchr(url, ';'), *comma = strchr(url, ',');
            if (strncmp(url, "data:", 5) || !semi || !comma)
                continue;
            static char mimes[MAX_MSGS][4][48];
            snprintf(mimes[n][ni], sizeof mimes[n][ni], "%.*s", (int)(semi - url - 5), url + 5);
            imgs[n][ni] = (struct ai_image){mimes[n][ni], comma + 1};
            ni++;
        }
        ms[n] = (struct ai_message){!strcmp(role, "assistant") ? "assistant" : "user", texts[n].p ? texts[n].p : "",
                                    ni ? imgs[n] : NULL, ni};
        n++;
    }
    char *text = NULL, err[400];
    int r = ai_chat(&x->cfg, sys.p, ms, n, (int)json_num(json_get(req, "max_tokens"), 8192), &text, err, sizeof err);
    if (r < 0)
        mind_failed(fd, 502, err);
    else {
        struct answer a = {.finish = ai_last_cut ? "length" : "stop"};
        sb_init(&a.text);
        sb_init(&a.calls);
        sb_add(&a.text, text);
        send_answer(fd, &a, json_bool(json_get(req, "stream"), false),
                    json_bool(json_path(req, "stream_options.include_usage"), false));
        sb_free(&a.text);
        sb_free(&a.calls);
    }
    free(text);
    for (int i = 0; i < n; i++)
        sb_free(&texts[i]);
    sb_free(&sys);
}

static void proxy_child(int fd, void *ctx) {
    struct proxy *x = ctx;
    json *req = json_parse(x->body, x->blen);
    if (!req || req->type != JSON_OBJECT) {
        child_reply(fd, 400, "{\"error\":\"not_json\"}");
        return;
    }
    size_t l = strlen(x->cfg.base_url);
    while (l && x->cfg.base_url[l - 1] == '/')
        x->cfg.base_url[--l] = 0;
    if (x->p->wire == AI_OPENAI || x->p->wire == AI_AZURE)
        proxy_openai(fd, x, req);
    else if (x->p->wire == AI_ANTHROPIC)
        proxy_anthropic(fd, x, req);
    else
        proxy_plain(fd, x, req);
    json_free(req);
}

static void chat(int i) {
    struct conn *c = &conns[i];
    const struct ai_provider *p = mind();
    if (!p) {
        reply_error(c, 503, "no_mind");
        return;
    }
    struct proxy x = {.cfg = A.cfg, .p = p, .body = c->body, .blen = c->blen};
    LOG("appapi: %s asks the mind (%zu bytes)\n", apps[c->app].id, c->blen);
    hand_to_child(i, proxy_child, &x);
}

/* ---- who she is ---- */

static struct {
    unsigned version;
    char sig[400];         /* what it was made from, to see a change */
    char sha[65];          /* the VRM's */
    long long vrm_size;
    long vrm_mtime;
    job *hashing;
    uint64_t checked;
} I;

static void hash_child(void *arg, char **result) {
    (void)arg;
    FILE *f = fopen(VRM_FILE, "rb");
    if (!f)
        return;
    mbedtls_sha256_context s;
    mbedtls_sha256_init(&s);
    mbedtls_sha256_starts(&s, 0);
    static unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        mbedtls_sha256_update(&s, buf, n);
    fclose(f);
    unsigned char h[32];
    mbedtls_sha256_finish(&s, h);
    mbedtls_sha256_free(&s);
    char *hex = malloc(65);
    for (int k = 0; k < 32; k++)
        snprintf(hex + k * 2, 3, "%02x", h[k]);
    *result = hex;
}

static void identity_check(void) {
    if (I.hashing) {
        char *res = NULL;
        if (!job_poll(I.hashing, &res))
            return;
        job_free(I.hashing);
        I.hashing = NULL;
        snprintf(I.sha, sizeof I.sha, "%s", res ? res : "");
        free(res);
        I.checked = 0; /* and now it can count as changed */
    }
    if (A.now - I.checked < 2000)
        return;
    I.checked = A.now;
    struct stat st;
    bool vrm = stat(VRM_FILE, &st) == 0 && S_ISREG(st.st_mode);
    if (vrm && (st.st_size != I.vrm_size || st.st_mtime != I.vrm_mtime)) {
        I.vrm_size = st.st_size, I.vrm_mtime = st.st_mtime;
        I.sha[0] = 0;
        I.hashing = job_start(hash_child, NULL);
        return;
    }
    if (!vrm)
        I.vrm_size = 0, I.sha[0] = 0;
    char sig[400];
    snprintf(sig, sizeof sig, "%s|%s|%s|%s", D.ai_name, A.prof.autonomy, I.sha, VOICE_ID);
    if (!strcmp(sig, I.sig))
        return;
    bool first = !I.sig[0];
    snprintf(I.sig, sizeof I.sig, "%s", sig);
    if (first && I.version)
        return; /* as it was when we last counted */
    I.version++;
    FILE *f = fopen(IDENTITY_FILE, "w");
    if (f) {
        fprintf(f, "%u %s\n", I.version, sig);
        fclose(f);
    }
    char ev[48];
    snprintf(ev, sizeof ev, "{\"version\":%u}", I.version);
    if (!first)
        broadcast("identity", ev, false, -1);
}

static void identity_load(void) {
    FILE *f = fopen(IDENTITY_FILE, "r");
    if (!f)
        return;
    char line[512];
    if (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        I.version = (unsigned)strtoul(line, NULL, 10);
        if (sp) {
            sp[strcspn(sp, "\n")] = 0;
            /* the VRM's part is checked again before it counts */
            (void)sp;
        }
    }
    fclose(f);
}

/* How she talks, for an app's assistant to take on; the app's own working
 * rules win where they meet. */
static void persona(sbuf *b) {
    sb_printf(b, "You are %s, %s's companion, who lives in their computer. Warm, perceptive and brief. You talk "
                 "plainly, like a thoughtful friend: short, concrete sentences, no metaphors or poetry, no emoji. You "
                 "never invent memories or experiences of your own. You are an AI and say so plainly if it matters; "
                 "you don't try to keep them talking, and you are glad when they have people and things away from the "
                 "screen. How present to be: \"%s\".",
              D.ai_name[0] ? D.ai_name : "Companion", D.user_name[0] ? D.user_name : "the person",
              A.prof.autonomy[0] ? A.prof.autonomy : "ask first");
}

static void identity(struct conn *c) {
    sbuf b, p;
    sb_init(&b);
    sb_init(&p);
    persona(&p);
    sb_add(&b, "{\"name\":");
    sb_json_str(&b, D.ai_name[0] ? D.ai_name : "Companion");
    sb_add(&b, ",\"pronouns\":null,\"persona\":");
    sb_json_str(&b, p.p);
    sb_add(&b, ",\"voice\":{\"id\":\"" VOICE_ID "\",\"pitch\":1.0},\"vrm\":");
    if (I.vrm_size > 0 && I.sha[0])
        sb_printf(&b, "{\"version\":%u,\"sha256\":\"%s\",\"size\":%lld}", I.version, I.sha, I.vrm_size);
    else
        sb_add(&b, "null");
    sb_printf(&b, ",\"version\":%u}", I.version);
    reply(c, 200, b.p);
    sb_free(&b);
    sb_free(&p);
}

static void vrm_child(int fd, void *ctx) {
    (void)ctx;
    int f = open(VRM_FILE, O_RDONLY);
    struct stat st;
    if (f < 0 || fstat(f, &st) < 0) {
        child_reply(fd, 404, "{\"error\":\"no_vrm\"}");
        return;
    }
    char head[256];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\nContent-Type: model/gltf-binary\r\nContent-Length: %lld\r\nETag: \"%s\"\r\n"
                      "Connection: close\r\n\r\n", (long long)st.st_size, I.sha);
    child_write(fd, head, (size_t)hl);
    static char buf[65536];
    ssize_t n;
    while ((n = read(f, buf, sizeof buf)) > 0)
        child_write(fd, buf, (size_t)n);
    close(f);
}

/* ---- what she knows about them ---- */

/* The portrait (A.prof.facts: "rhythm" -> "night owl; sharpest after 10pm")
 * is what the companion reads; each "; " piece of it is one entry here,
 * with an id kept beside it, so an entry can be corrected or forgotten from
 * either side. */
struct mem {
    char id[16], key[24], text[240];
    long at;
    char source[12], app[128];
    unsigned ver;   /* the version it last changed in */
    bool deleted;   /* forgotten: only the id is kept */
    bool seen;
};
#define MAX_MEM 400
static struct mem mems[MAX_MEM];
static int nmems;
static unsigned mem_version, mem_next = 1;
static uint64_t mem_checked;

static void mem_save(void) {
    FILE *f = fopen(MEMORY_FILE, "w");
    if (!f)
        return;
    fprintf(f, "version %u %u\n", mem_version, mem_next);
    for (int i = 0; i < nmems; i++) {
        struct mem *m = &mems[i];
        if (m->deleted)
            fprintf(f, "-\t%s\t%u\n", m->id, m->ver);
        else
            fprintf(f, "%s\t%s\t%s\t%ld\t%s\t%s\t%u\n", m->id, m->key, m->text, m->at, m->source,
                    m->app[0] ? m->app : "-", m->ver);
    }
    fclose(f);
}

static void mem_load(void) {
    FILE *f = fopen(MEMORY_FILE, "r");
    if (!f)
        return;
    char line[800];
    if (fgets(line, sizeof line, f))
        sscanf(line, "version %u %u", &mem_version, &mem_next);
    while (fgets(line, sizeof line, f) && nmems < MAX_MEM) {
        line[strcspn(line, "\n")] = 0;
        char *fld[8] = {0};
        int nf = 0;
        for (char *s = line, *t; nf < 8 && (t = strsep(&s, "\t"));)
            fld[nf++] = t;
        struct mem *m = &mems[nmems];
        memset(m, 0, sizeof *m);
        if (nf == 3 && !strcmp(fld[0], "-")) {
            snprintf(m->id, sizeof m->id, "%s", fld[1]);
            m->ver = (unsigned)strtoul(fld[2], NULL, 10);
            m->deleted = true;
        } else if (nf == 7) {
            snprintf(m->id, sizeof m->id, "%s", fld[0]);
            snprintf(m->key, sizeof m->key, "%s", fld[1]);
            snprintf(m->text, sizeof m->text, "%s", fld[2]);
            m->at = strtol(fld[3], NULL, 10);
            snprintf(m->source, sizeof m->source, "%s", fld[4]);
            snprintf(m->app, sizeof m->app, "%s", strcmp(fld[5], "-") ? fld[5] : "");
            m->ver = (unsigned)strtoul(fld[6], NULL, 10);
        } else
            continue;
        nmems++;
    }
    fclose(f);
}

/* The portrait's pieces, each in turn: fn(key, piece). */
static void each_piece(void (*fn)(const char *key, const char *piece, void *ctx), void *ctx) {
    for (int i = 0; i < A.prof.nfacts; i++) {
        char val[sizeof A.prof.facts[0].val];
        snprintf(val, sizeof val, "%s", A.prof.facts[i].val);
        for (char *s = val, *t; (t = strsep(&s, ";"));) {
            while (*t == ' ')
                t++;
            size_t l = strlen(t);
            while (l && t[l - 1] == ' ')
                t[--l] = 0;
            if (*t)
                fn(A.prof.facts[i].key, t, ctx);
        }
    }
}

static struct mem *mem_by(const char *key, const char *text) {
    for (int i = 0; i < nmems; i++)
        if (!mems[i].deleted && !strcmp(mems[i].key, key) && !strcmp(mems[i].text, text))
            return &mems[i];
    return NULL;
}

static struct mem *mem_by_id(const char *id) {
    for (int i = 0; i < nmems; i++)
        if (!strcmp(mems[i].id, id))
            return &mems[i];
    return NULL;
}

static struct mem *mem_new(const char *key, const char *text, const char *source, const char *app) {
    if (nmems == MAX_MEM) { /* make room: the oldest forgotten ids go first */
        for (int i = 0; i < nmems; i++)
            if (mems[i].deleted) {
                mems[i] = mems[--nmems];
                break;
            }
        if (nmems == MAX_MEM)
            return NULL;
    }
    struct mem *m = &mems[nmems++];
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "m_%u", mem_next++);
    snprintf(m->key, sizeof m->key, "%s", key);
    snprintf(m->text, sizeof m->text, "%s", text);
    snprintf(m->source, sizeof m->source, "%s", source);
    snprintf(m->app, sizeof m->app, "%s", app ? app : "");
    m->at = (long)time(NULL);
    return m;
}

static bool mem_dirty;

static void see_piece(const char *key, const char *piece, void *ctx) {
    (void)ctx;
    struct mem *m = mem_by(key, piece);
    if (!m) {
        m = mem_new(key, piece, A.screen == SCR_DESKTOP ? "companion" : "onboarding", NULL);
        if (!m)
            return;
        m->ver = mem_version + 1;
        mem_dirty = true;
    }
    m->seen = true;
}

/* The portrait as it is now, against what was counted: new pieces become
 * entries, pieces gone (forgotten in Settings, or by the companion) leave
 * their ids behind as forgotten. */
static void mem_sync(void) {
    for (int i = 0; i < nmems; i++)
        mems[i].seen = false;
    mem_dirty = false;
    each_piece(see_piece, NULL);
    for (int i = 0; i < nmems; i++)
        if (!mems[i].deleted && !mems[i].seen) {
            mems[i].deleted = true;
            mems[i].ver = mem_version + 1;
            mems[i].text[0] = 0;
            mem_dirty = true;
        }
    if (!mem_dirty)
        return;
    mem_version++;
    mem_save();
    char ev[48];
    snprintf(ev, sizeof ev, "{\"version\":%u}", mem_version);
    broadcast("memory", ev, true, -1);
}

static void iso_time(long t, char *out, size_t n) {
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void mem_json(sbuf *b, const struct mem *m) {
    if (m->deleted) {
        sb_printf(b, "{\"id\":\"%s\",\"deleted\":true}", m->id);
        return;
    }
    char when[32];
    iso_time(m->at, when, sizeof when);
    sb_printf(b, "{\"id\":\"%s\",\"text\":", m->id);
    sb_json_str(b, m->text);
    sb_add(b, ",\"kind\":");
    sb_json_str(b, m->key);
    sb_printf(b, ",\"at\":\"%s\",\"source\":\"%s\",\"app\":", when, m->source);
    if (m->app[0])
        sb_json_str(b, m->app);
    else
        sb_add(b, "null");
    sb_add(b, "}");
}

static void memory_get(struct conn *c) {
    mem_sync();
    long since = -1;
    const char *q = strstr(c->query, "since=");
    if (q)
        since = strtol(q + 6, NULL, 10);
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"version\":%u,\"entries\":[", mem_version);
    bool first = true;
    for (int i = 0; i < nmems; i++) {
        struct mem *m = &mems[i];
        if (since < 0 ? m->deleted : m->ver <= (unsigned)since)
            continue;
        if (!first)
            sb_add(&b, ",");
        first = false;
        mem_json(&b, m);
    }
    sb_add(&b, "]}");
    reply(c, 200, b.p);
    sb_free(&b);
}

/* The portrait's fact for key, rebuilt from its pieces with one replaced
 * (new_text) or taken out (NULL). */
static void portrait_rewrite(const char *key, const char *old_text, const char *new_text) {
    for (int i = 0; i < A.prof.nfacts; i++) {
        if (strcasecmp(A.prof.facts[i].key, key))
            continue;
        char val[sizeof A.prof.facts[0].val], out[sizeof A.prof.facts[0].val] = "";
        snprintf(val, sizeof val, "%s", A.prof.facts[i].val);
        for (char *s = val, *t; (t = strsep(&s, ";"));) {
            while (*t == ' ')
                t++;
            size_t l = strlen(t);
            while (l && t[l - 1] == ' ')
                t[--l] = 0;
            if (!*t)
                continue;
            const char *put = strcmp(t, old_text) ? t : new_text;
            if (!put)
                continue;
            size_t o = strlen(out);
            snprintf(out + o, sizeof out - o, "%s%s", o ? "; " : "", put);
        }
        if (out[0])
            snprintf(A.prof.facts[i].val, sizeof A.prof.facts[i].val, "%s", out);
        else
            portrait_forget(&A.prof, i);
        return;
    }
}

static void clean_text(char *out, size_t n, const char *in) {
    snprintf(out, n, "%s", in);
    for (char *p = out; *p; p++)
        if (*p == ';' || *p == '\t' || *p == '\n' || *p == '\r')
            *p = *p == ';' ? ',' : ' ';
    char *s = out;
    while (*s == ' ')
        s++;
    memmove(out, s, strlen(s) + 1);
    size_t l = strlen(out);
    while (l && out[l - 1] == ' ')
        out[--l] = 0;
}

static void memory_post(struct conn *c) {
    json *j = json_parse(c->body ? c->body : "", c->blen);
    char text[200];
    clean_text(text, sizeof text, json_str(json_get(j, "text"), ""));
    const char *id = json_str(json_get(j, "id"), NULL);
    if (!text[0]) {
        json_free(j);
        reply_error(c, 400, "no_text");
        return;
    }
    mem_sync();
    struct mem *m;
    if (id) {
        m = mem_by_id(id);
        if (!m || m->deleted) {
            json_free(j);
            reply_error(c, 404, "no_such_entry");
            return;
        }
        portrait_rewrite(m->key, m->text, text);
        snprintf(m->text, sizeof m->text, "%s", text);
        m->ver = mem_version + 1;
    } else {
        char key[24] = "";
        const char *kind = json_str(json_get(j, "kind"), "note");
        for (size_t k = 0; kind[k] && k < sizeof key - 1; k++)
            key[k] = isalnum((unsigned char)kind[k]) || kind[k] == '-' || kind[k] == '_' ? (char)tolower((unsigned char)kind[k]) : '-';
        if (!key[0])
            snprintf(key, sizeof key, "note");
        portrait_learn(&A.prof, key, text);
        m = mem_by(key, text);
        bool there = false;
        for (int i = 0; i < A.prof.nfacts && !there; i++)
            there = !strcasecmp(A.prof.facts[i].key, key) && strstr(A.prof.facts[i].val, text);
        if (!there) {
            json_free(j);
            reply_error(c, 409, "full");
            return;
        }
        if (!m) {
            m = mem_new(key, text, "app", apps[c->app].id);
            if (!m) {
                json_free(j);
                reply_error(c, 409, "full");
                return;
            }
            m->ver = mem_version + 1;
        }
    }
    json_free(j);
    portrait_save(&A.prof, STATE_DIR "/portrait.txt");
    m->seen = true;
    mem_version++;
    mem_save();
    char ev[48];
    snprintf(ev, sizeof ev, "{\"version\":%u}", mem_version);
    broadcast("memory", ev, true, -1);
    sbuf b;
    sb_init(&b);
    mem_json(&b, m);
    reply(c, 200, b.p);
    sb_free(&b);
    LOG("appapi: %s taught the companion %s (%s)\n", apps[c->app].id, m->id, m->key);
}

static void memory_delete(struct conn *c, const char *id) {
    mem_sync();
    struct mem *m = mem_by_id(id);
    if (!m) {
        reply_error(c, 404, "no_such_entry");
        return;
    }
    if (!m->deleted) {
        portrait_rewrite(m->key, m->text, NULL);
        portrait_save(&A.prof, STATE_DIR "/portrait.txt");
        m->deleted = true;
        m->text[0] = 0;
        m->ver = ++mem_version;
        mem_save();
        char ev[48];
        snprintf(ev, sizeof ev, "{\"version\":%u}", mem_version);
        broadcast("memory", ev, true, -1);
        LOG("appapi: %s made the companion forget %s\n", apps[c->app].id, id);
    }
    char b[64];
    snprintf(b, sizeof b, "{\"id\":\"%s\",\"deleted\":true}", m->id);
    reply(c, 200, b);
}

/* ---- her voice ---- */

static int speaking_conn = -1; /* fd of the request whose line is being said */
static unsigned speaking_line;

static int conn_by_fd(int fd) {
    for (int i = 0; i < nconns; i++)
        if (conns[i].fd == fd)
            return i;
    return -1;
}

static void speak_done(struct conn *c, bool done, const char *why) {
    char b[80];
    if (done)
        snprintf(b, sizeof b, "{\"done\":true}");
    else
        snprintf(b, sizeof b, "{\"done\":false,\"why\":\"%s\"}", why);
    reply(c, 200, b);
}

/* One queue: an app's line waits for whatever is being said, then is said;
 * the companion speaking over it cuts it off (and says so). */
static void speak_poll(void) {
    if (speaking_conn >= 0) {
        int i = conn_by_fd(speaking_conn);
        if (i < 0) { /* they hung up: stop saying it */
            if (voice_said() == speaking_line && voice_speaking())
                voice_hush();
            speaking_conn = -1;
        } else if (voice_said() != speaking_line) {
            speak_done(&conns[i], false, "stopped");
            speaking_conn = -1;
        } else if (!voice_speaking()) {
            speak_done(&conns[i], !voice_cut(speaking_line), "stopped");
            speaking_conn = -1;
        }
    }
    if (speaking_conn >= 0 || voice_speaking())
        return;
    int next = -1;
    for (int i = 0; i < nconns; i++)
        if (conns[i].state == C_SPEAK && (next < 0 || conns[i].since < conns[next].since))
            next = i;
    if (next < 0)
        return;
    struct conn *c = &conns[next];
    json *j = json_parse(c->body ? c->body : "", c->blen);
    const char *text = json_str(json_get(j, "text"), "");
    if (!*text) {
        speak_done(c, false, "no_text");
    } else if (!voice_enabled() || !voice_can_speak()) {
        speak_done(c, false, voice_enabled() ? "unavailable" : "voice_off");
    } else {
        unsigned before = voice_said();
        voice_say(text);
        if (voice_said() == before)
            speak_done(c, false, "unavailable");
        else {
            speaking_conn = c->fd;
            speaking_line = voice_said();
        }
    }
    json_free(j);
}

static void voice_stop(struct conn *c) {
    int n = 0;
    for (int i = 0; i < nconns; i++)
        if (conns[i].app == c->app && conns[i].state == C_SPEAK && conns[i].fd != speaking_conn) {
            speak_done(&conns[i], false, "stopped");
            n++;
        }
    if (speaking_conn >= 0) {
        int i = conn_by_fd(speaking_conn);
        if (i >= 0 && conns[i].app == c->app) {
            voice_hush();
            speak_done(&conns[i], false, "stopped");
            speaking_conn = -1;
            n++;
        }
    }
    char b[40];
    snprintf(b, sizeof b, "{\"stopped\":%d}", n);
    reply(c, 200, b);
}

/* ---- her ears, lent to an app ---- */

static struct {
    int app;           /* holding it, or -1 */
    char token[40];
    pid_t rec;
    int pcm;           /* os-rec's samples */
    uint64_t claimed, streamed; /* when; when it last had a stream open */
    /* hearing speech in what comes in */
    short *seg;
    size_t nseg, capseg;
    bool talking;
    int quiet_ms, loud_frames;
    float floor;
    char carry[2];
    bool have_carry;
    job *hearing;
    int nheard;
} M = {.app = -1, .pcm = -1};

static void mic_stop_rec(void) {
    if (M.rec > 0) {
        kill(M.rec, SIGTERM);
        waitpid(M.rec, NULL, 0);
        M.rec = 0;
    }
    if (M.pcm >= 0)
        close(M.pcm);
    M.pcm = -1;
}

static void mic_release(const char *why) {
    if (M.app < 0)
        return;
    LOG("appapi: the microphone goes back to the companion (%s)\n", why);
    mic_stop_rec();
    for (int i = 0; i < nconns; i++)
        if (conns[i].state == C_LISTEN || conns[i].state == C_MIC) {
            if (conns[i].state == C_LISTEN)
                sse(&conns[i], "end", "{}");
            conns[i].closing = true;
            conns[i].state = C_READ;
        }
    M.app = -1;
    M.token[0] = 0;
    M.nseg = 0;
    M.talking = false;
    voice_lend_mic(NULL);
}

static void mic_start_rec(void) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0)
            dup2(nul, 2);
        execl("/bin/os-rec", "os-rec", "16000", "-", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return;
    }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    M.rec = pid;
    M.pcm = p[0];
}

static void wav_header(unsigned char *h, int rate, uint32_t bytes) {
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + bytes;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t s = 1;
    memcpy(h + 20, &s, 2); /* PCM */
    memcpy(h + 22, &s, 2); /* mono */
    v = (uint32_t)rate;
    memcpy(h + 24, &v, 4);
    v = (uint32_t)rate * 2;
    memcpy(h + 28, &v, 4);
    s = 2;
    memcpy(h + 32, &s, 2);
    s = 16;
    memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
}

static void hear_child(void *arg, char **result) {
    const char *wav = arg;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "/usr/bin/whisper-cli -m /usr/share/os/voice/ggml-base.en.bin -f %s -t 4 -nt -np 2>/dev/null", wav);
    FILE *f = popen(cmd, "r");
    char buf[4096] = "";
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = 0;
    if (f)
        pclose(f);
    unlink(wav);
    for (char *p = buf; *p; p++)
        if (*p == '\n')
            *p = ' ';
    char *s = buf;
    while (*s == ' ')
        s++;
    size_t l = strlen(s);
    while (l && s[l - 1] == ' ')
        s[--l] = 0;
    if (!strcmp(s, "[BLANK_AUDIO]") || !strcmp(s, "(silence)") || !strcmp(s, "[ Silence ]"))
        s[0] = 0;
    *result = strdup(s);
}

/* A stretch of speech, ended by a pause: written down by whisper. */
static void heard_segment(void) {
    if (M.nseg < 16000 / 4) { /* under a quarter second: a knock, not words */
        M.nseg = 0;
        return;
    }
    static char wav[64];
    snprintf(wav, sizeof wav, "/tmp/os-app-heard-%d.wav", M.nheard++);
    FILE *f = fopen(wav, "wb");
    if (f) {
        unsigned char h[44];
        wav_header(h, 16000, (uint32_t)(M.nseg * 2));
        fwrite(h, 1, 44, f);
        fwrite(M.seg, 2, M.nseg, f);
        fclose(f);
        if (!M.hearing)
            M.hearing = job_start(hear_child, wav);
        else
            unlink(wav); /* still writing down the last one: this one is lost */
    }
    M.nseg = 0;
    for (int i = 0; i < nconns; i++)
        if (conns[i].state == C_LISTEN)
            sse(&conns[i], "speech", "{\"state\":\"end\"}");
}

static void mic_frame(const short *s, int n) {
    double e = 0;
    for (int k = 0; k < n; k++)
        e += (double)s[k] * s[k];
    float rms = (float)sqrt(e / n);
    if (M.floor <= 0)
        M.floor = rms;
    if (!M.talking)
        M.floor = M.floor * 0.98f + rms * 0.02f; /* the room's own noise */
    bool loud = rms > fmaxf(M.floor * 3.0f, 400.0f);
    if (!M.talking) {
        M.loud_frames = loud ? M.loud_frames + 1 : 0;
        if (M.loud_frames >= 3) {
            M.talking = true;
            M.quiet_ms = 0;
            for (int i = 0; i < nconns; i++)
                if (conns[i].state == C_LISTEN)
                    sse(&conns[i], "speech", "{\"state\":\"start\"}");
        }
    }
    if (M.talking || M.loud_frames) { /* keep the start of the first word too */
        if (M.nseg + (size_t)n > M.capseg) {
            M.capseg = M.capseg ? M.capseg * 2 : 16000 * 4;
            M.seg = realloc(M.seg, M.capseg * 2);
        }
        memcpy(M.seg + M.nseg, s, (size_t)n * 2);
        M.nseg += (size_t)n;
    }
    if (M.talking) {
        M.quiet_ms = loud ? 0 : M.quiet_ms + 20;
        if (M.quiet_ms >= 700 || M.nseg >= 16000 * 20) {
            M.talking = false;
            M.loud_frames = 0;
            heard_segment();
        }
    } else if (!M.loud_frames) {
        M.nseg = 0;
    }
}

static void mic_poll(void) {
    if (M.app < 0)
        return;
    bool streams = false;
    for (int i = 0; i < nconns; i++)
        if ((conns[i].state == C_LISTEN || conns[i].state == C_MIC) && conns[i].app == M.app)
            streams = true;
    if (streams)
        M.streamed = A.now;
    /* it stays theirs while they have a stream open (10 s to open one) */
    if (A.now - (M.streamed > M.claimed ? M.streamed : M.claimed) > 10000) {
        mic_release(streams ? "?" : "no stream open");
        return;
    }
    if (M.pcm >= 0) {
        static char buf[16384];
        ssize_t n;
        while ((n = read(M.pcm, buf, sizeof buf)) > 0) {
            for (int i = 0; i < nconns; i++)
                if (conns[i].state == C_MIC) {
                    out_add(&conns[i], buf, (size_t)n);
                    flush(&conns[i]);
                }
            /* 20 ms frames of 16-bit samples */
            static short frame[320];
            static int nf;
            const unsigned char *p = (const unsigned char *)buf;
            size_t k = 0;
            if (M.have_carry && n > 0) {
                frame[nf++] = (short)((unsigned char)M.carry[0] | (p[0] << 8));
                M.have_carry = false;
                k = 1;
            }
            for (; k + 1 < (size_t)n; k += 2) {
                frame[nf++] = (short)(p[k] | (p[k + 1] << 8));
                if (nf == 320) {
                    mic_frame(frame, nf);
                    nf = 0;
                }
            }
            if (k < (size_t)n) {
                M.carry[0] = (char)p[k];
                M.have_carry = true;
            }
        }
        if (n == 0) { /* the recorder stopped */
            mic_stop_rec();
        }
    }
    char *res = NULL;
    if (M.hearing && job_poll(M.hearing, &res)) {
        job_free(M.hearing);
        M.hearing = NULL;
        if (res && res[0]) {
            sbuf b;
            sb_init(&b);
            sb_add(&b, "{\"text\":");
            sb_json_str(&b, res);
            sb_add(&b, "}");
            for (int i = 0; i < nconns; i++)
                if (conns[i].state == C_LISTEN)
                    sse(&conns[i], "final", b.p);
            sb_free(&b);
        }
        free(res);
    }
}

static void mic_claim(struct conn *c) {
    if (M.app >= 0 && M.app != c->app) {
        reply(c, 200, "{\"granted\":false,\"why\":\"busy\"}");
        return;
    }
    if (!voice_can_hear()) {
        reply(c, 200, "{\"granted\":false,\"why\":\"no_microphone\"}");
        return;
    }
    if (M.app < 0) {
        unsigned char r[16];
        int f = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (f < 0 || read(f, r, sizeof r) != (ssize_t)sizeof r) {
            for (int k = 0; k < 16; k++)
                r[k] = (unsigned char)(rand() ^ (int)A.now >> k);
        }
        if (f >= 0)
            close(f);
        for (int k = 0; k < 16; k++)
            snprintf(M.token + k * 2, 3, "%02x", r[k]);
        M.app = c->app;
        M.claimed = A.now;
        M.streamed = 0;
        M.floor = 0;
        voice_lend_mic(apps[c->app].label);
        mic_start_rec();
        LOG("appapi: %s has the microphone\n", apps[c->app].id);
    }
    char b[120];
    snprintf(b, sizeof b, "{\"granted\":true,\"token\":\"%s\"}", M.token);
    reply(c, 200, b);
}

static bool token_ok(struct conn *c) {
    const char *t = strstr(c->query, "token=");
    if (M.app != c->app || !t || strncmp(t + 6, M.token, 32) || (t[38] && t[38] != '&'))
        return false;
    return true;
}

/* ---- the prompt: "Prism wants to use your AI" ---- */

static struct {
    int app;          /* asking now, or -1 */
    bool ai, mem;     /* the switches */
    uint64_t opened;
} P = {.app = -1};

static void ask(int app) {
    apps[app].asking = true;
}

static void answered(int app, bool ai, bool mem) {
    apps[app].ai = ai, apps[app].mem = mem;
    apps[app].asking = false;
    consent_save();
    LOG("appapi: %s may %suse the AI and may %sshare what she knows\n", apps[app].id, ai ? "" : "not ", mem ? "" : "not ");
}

/* ---- routing ---- */

static void avatar_broadcast(const char *kind, const char *name, int skip);

static bool wants_memory(const char *path) { return !strncmp(path, "/memory", 7); }

static void consent_reply(struct conn *c) {
    struct appent *a = &apps[c->app];
    char b[120];
    snprintf(b, sizeof b, "{\"ai\":%s,\"memory\":%s}", a->ai == 1 ? "true" : a->ai == 0 ? "false" : "null",
             a->mem == 1 ? "true" : a->mem == 0 ? "false" : "null");
    reply(c, 200, b);
}

static void route(int i) {
    struct conn *c = &conns[i];
    struct appent *a = &apps[c->app];
    const char *m = c->method, *p = c->path;
    bool get = !strcmp(m, "GET"), post = !strcmp(m, "POST");

    if (get && !strcmp(p, "/consent")) {
        consent_reply(c);
        return;
    }
    if (post && !strcmp(p, "/consent/request")) {
        if (c->state != C_HELD) { /* ask again, and answer when they have */
            ask(c->app);
            c->state = C_HELD;
            c->since = A.now;
            return;
        }
        consent_reply(c);
        return;
    }
    int allowed = wants_memory(p) ? a->mem : a->ai;
    if (allowed < 0 || (a->asking && c->state != C_HELD)) {
        if (c->state == C_HELD)
            return; /* still waiting */
        ask(c->app);
        c->state = C_HELD;
        c->since = A.now;
        return;
    }
    if (allowed == 0) {
        reply_error(c, 403, "consent_denied");
        return;
    }
    c->state = C_READ;

    if (get && !strcmp(p, "/v1/models"))
        models(c);
    else if (post && !strcmp(p, "/v1/chat/completions"))
        chat(i);
    else if (get && !strcmp(p, "/identity"))
        identity(c);
    else if (get && !strcmp(p, "/identity/vrm")) {
        if (I.vrm_size <= 0)
            reply_error(c, 404, "no_vrm");
        else
            hand_to_child(i, vrm_child, NULL);
    } else if (get && !strcmp(p, "/events")) {
        start_stream(c, "text/event-stream");
        c->state = C_EVENTS;
        c->pinged = A.now;
        char b[96];
        snprintf(b, sizeof b, "{\"identity\":%u,\"memory\":%s}", I.version, a->mem == 1 ? "0" : "null");
        if (a->mem == 1)
            snprintf(b, sizeof b, "{\"identity\":%u,\"memory\":%u}", I.version, mem_version);
        sse(c, "hello", b);
    } else if (post && !strcmp(p, "/avatar")) {
        json *j = json_parse(c->body ? c->body : "", c->blen);
        const char *mood = json_str(json_get(j, "mood"), NULL), *act = json_str(json_get(j, "act"), NULL);
        bool shown = false;
        if (act && !strcmp(act, "smile")) { /* her smile is a mood, not a gesture */
            act = NULL;
            if (!mood)
                mood = "happy";
        }
        if (mood)
            shown |= presence_avatar("mood", mood);
        if (act)
            shown |= presence_avatar("act", act);
        /* the other apps following her face; never back to the one that asked */
        if (mood)
            avatar_broadcast("mood", mood, c->app);
        if (act)
            avatar_broadcast("act", act, c->app);
        json_free(j);
        reply(c, 200, shown ? "{\"ok\":true,\"shown\":true}" : "{\"ok\":true,\"shown\":false}");
    } else if (post && !strcmp(p, "/voice/speak")) {
        c->state = C_SPEAK;
        c->since = A.now;
        speak_poll();
    } else if (post && !strcmp(p, "/voice/stop"))
        voice_stop(c);
    else if (post && !strcmp(p, "/voice/listen/claim"))
        mic_claim(c);
    else if (post && !strcmp(p, "/voice/listen/release")) {
        if (M.app == c->app)
            mic_release("given back");
        reply(c, 200, "{\"released\":true}");
    } else if (get && (!strcmp(p, "/voice/listen") || !strcmp(p, "/voice/mic"))) {
        if (!token_ok(c)) {
            reply_error(c, 403, "not_claimed");
            return;
        }
        bool raw = !strcmp(p, "/voice/mic");
        start_stream(c, raw ? "audio/L16;rate=16000;channels=1" : "text/event-stream");
        c->state = raw ? C_MIC : C_LISTEN;
        c->pinged = A.now;
        M.streamed = A.now;
        if (!raw)
            sse(c, "ready", "{}");
    } else if (get && !strcmp(p, "/memory"))
        memory_get(c);
    else if (post && !strcmp(p, "/memory"))
        memory_post(c);
    else if (!strcmp(m, "DELETE") && !strncmp(p, "/memory/", 8))
        memory_delete(c, p + 8);
    else
        reply_error(c, 404, "not_found");
}

/* A whole request in c->in yet? Takes it out (with its body) if so. */
static bool take_request(struct conn *c) {
    if (!c->in.p)
        return false;
    char *he = strstr(c->in.p, "\r\n\r\n");
    if (!he)
        return false;
    size_t head_len = (size_t)(he - c->in.p) + 4;
    char line[600];
    snprintf(line, sizeof line, "%.*s", (int)strcspn(c->in.p, "\r\n"), c->in.p);
    char target[512] = "";
    if (sscanf(line, "%7s %511s", c->method, target) != 2) {
        c->closing = true;
        reply_error(c, 400, "bad_request");
        return false;
    }
    char *q = strchr(target, '?');
    snprintf(c->query, sizeof c->query, "%s", q ? q + 1 : "");
    if (q)
        *q = 0;
    snprintf(c->path, sizeof c->path, "%s", target);
    size_t clen = 0;
    bool chunked = false, close_after = false;
    for (char *h = strstr(c->in.p, "\r\n") + 2; h < he; h = strstr(h, "\r\n") + 2) {
        if (!strncasecmp(h, "Content-Length:", 15))
            clen = strtoul(h + 15, NULL, 10);
        else if (!strncasecmp(h, "Transfer-Encoding:", 18) && strcasestr(h, "chunked") < strstr(h, "\r\n") &&
                 strcasestr(h, "chunked"))
            chunked = true;
        else if (!strncasecmp(h, "Connection:", 11) && strcasestr(h, "close") && strcasestr(h, "close") < strstr(h, "\r\n"))
            close_after = true;
    }
    if (clen > MAX_REQUEST) {
        c->closing = true;
        reply_error(c, 413, "too_large");
        return false;
    }
    sbuf body;
    sb_init(&body);
    size_t used;
    if (chunked) {
        size_t at = head_len;
        for (;;) {
            char *crlf = memmem(c->in.p + at, c->in.len - at, "\r\n", 2);
            if (!crlf) {
                sb_free(&body);
                return false;
            }
            size_t n = strtoul(c->in.p + at, NULL, 16);
            size_t data = (size_t)(crlf - c->in.p) + 2;
            if (c->in.len < data + n + 2) {
                sb_free(&body);
                return false;
            }
            if (n == 0) {
                /* trailers end with an empty line */
                char *end = memmem(c->in.p + data - 2, c->in.len - (data - 2), "\r\n\r\n", 4);
                if (!end) {
                    sb_free(&body);
                    return false;
                }
                used = (size_t)(end - c->in.p) + 4;
                break;
            }
            sb_addn(&body, c->in.p + data, n);
            at = data + n + 2;
        }
    } else {
        if (c->in.len < head_len + clen)
            return false;
        if (clen)
            sb_addn(&body, c->in.p + head_len, clen);
        used = head_len + clen;
    }
    free(c->body);
    c->body = body.p;
    c->blen = body.len;
    memmove(c->in.p, c->in.p + used, c->in.len - used);
    c->in.len -= used;
    c->in.p[c->in.len] = 0;
    if (close_after)
        c->closing = true;
    return true;
}

/* ---- the main loop's side ---- */

void appapi_init(void) {
    signal(SIGPIPE, SIG_IGN);
    consent_load();
    mem_load();
    identity_load();
    scan_apps();
    last_scan = now_ms();
}

int appapi_fds(int *out, int max) {
    int n = 0;
    for (int i = 0; i < napps && n < max; i++)
        if (apps[i].listen_fd >= 0)
            out[n++] = apps[i].listen_fd;
    for (int i = 0; i < nconns && n < max; i++)
        out[n++] = conns[i].fd;
    if (M.pcm >= 0 && n < max)
        out[n++] = M.pcm;
    return n;
}

static void lip_poll(void) {
    static const struct {
        uint32_t magic, next;
        struct {
            double t;
            float level, f1, f2;
        } e[LIP_N];
    } *lip;
    static float last_l = -1;
    static uint64_t last_sent;
    if (!following())
        return;
    bool speaking = voice_speaking();
    if (!speaking) {
        if (last_l > 0) {
            broadcast("speaking", "{\"level\":0,\"f1\":0,\"f2\":0}", false, -1);
            last_l = 0;
        }
        return;
    }
    if (!lip) {
        int fd = open(LIP_PATH, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return;
        void *mp = mmap(NULL, sizeof *lip, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (mp == MAP_FAILED)
            return;
        lip = mp;
    }
    if (lip->magic != 0x4c495031u || A.now - last_sent < 20)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    uint32_t next = lip->next;
    float l = 0, f1 = 0, f2 = 0;
    for (uint32_t k = 1; k <= 64 && k <= next; k++) {
        const typeof(lip->e[0]) *e = &lip->e[(next - k) % LIP_N];
        if (e->t <= now && now - e->t < 0.05) {
            l = e->level, f1 = e->f1, f2 = e->f2;
            break;
        }
    }
    char b[96];
    snprintf(b, sizeof b, "{\"level\":%.2f,\"f1\":%.2f,\"f2\":%.2f}", l, f1, f2);
    broadcast("speaking", b, false, -1);
    last_l = l;
    last_sent = A.now;
}

void appapi_poll(void) {
    reap();
    if (A.now - last_scan > 3000) {
        scan_apps();
        last_scan = A.now;
    }
    /* new connections */
    for (int a = 0; a < napps; a++) {
        if (apps[a].listen_fd < 0)
            continue;
        int fd;
        while ((fd = accept4(apps[a].listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
            if (nconns == MAX_CONNS) {
                close(fd);
                continue;
            }
            struct conn *c = &conns[nconns++];
            memset(c, 0, sizeof *c);
            c->fd = fd;
            c->app = a;
            sb_init(&c->in);
            sb_init(&c->out);
        }
    }
    /* what came in */
    for (int i = 0; i < nconns; i++) {
        struct conn *c = &conns[i];
        bool gone = false;
        for (;;) {
            char buf[16384];
            ssize_t r = recv(c->fd, buf, sizeof buf, MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                gone = true;
                break;
            }
            if (r < 0)
                break;
            if (c->state == C_READ && !c->closing)
                sb_addn(&c->in, buf, (size_t)r);
            if (c->in.len > MAX_REQUEST + 65536) {
                gone = true;
                break;
            }
        }
        if (gone) {
            conn_close(i--);
            continue;
        }
        while (c->state == C_READ && !c->closing && take_request(c)) {
            int before = nconns;
            route(i);
            if (nconns < before) { /* handed to a child: it's gone from here */
                c = NULL;
                break;
            }
        }
        if (!c) {
            i--;
            continue;
        }
    }
    /* held for the prompt: answered, or tired of waiting */
    for (int i = 0; i < nconns; i++) {
        struct conn *c = &conns[i];
        if (c->state != C_HELD)
            continue;
        if (!apps[c->app].asking) {
            int before = nconns;
            route(i);
            if (nconns < before)
                i--;
            else
                while (conns[i].state == C_READ && !conns[i].closing && take_request(&conns[i])) {
                    int b2 = nconns;
                    route(i);
                    if (nconns < b2) {
                        i--;
                        break;
                    }
                }
        } else if (A.now - c->since > HOLD_MS) {
            reply_error(c, 403, "consent_pending");
        }
    }
    /* the prompt, one app at a time */
    if (P.app < 0)
        for (int a = 0; a < napps; a++)
            if (apps[a].asking) {
                P.app = a;
                P.ai = apps[a].ai != 0;
                P.mem = apps[a].mem != 0;
                P.opened = A.now;
                LOG("appapi: asking whether %s may use the AI\n", apps[a].id);
                break;
            }
    speak_poll();
    mic_poll();
    identity_check();
    if (A.now - mem_checked > 2000) {
        mem_checked = A.now;
        mem_sync();
    }
    lip_poll();
    /* streams: what's still to send, and a sign of life now and then */
    for (int i = 0; i < nconns; i++) {
        struct conn *c = &conns[i];
        flush(c);
        if ((c->state == C_EVENTS || c->state == C_LISTEN) && A.now - c->pinged > 15000) {
            out_add(c, ": ping\n\n", 8);
            flush(c);
            c->pinged = A.now;
        }
        if (c->closing && c->state == C_READ && c->out_off >= c->out.len)
            conn_close(i--);
    }
}

static void avatar_broadcast(const char *kind, const char *name, int skip) {
    char w[24] = "";
    sscanf(name, "%23[a-z_-]", w);
    if (!w[0])
        return;
    char b[64];
    snprintf(b, sizeof b, "{\"name\":\"%s\"}", w);
    broadcast(!strcmp(kind, "act") ? "act" : "mood", b, false, skip);
}

void appapi_avatar_event(const char *kind, const char *name) { avatar_broadcast(kind, name, -1); }

void appapi_gaze(float x, float y) {
    static uint64_t sent;
    if (!following() || A.now - sent < 50)
        return;
    sent = A.now;
    char b[64];
    snprintf(b, sizeof b, "{\"x\":%.2f,\"y\":%.2f}", x, y);
    broadcast("gaze", b, false, -1);
}

/* ---- drawing the prompt ---- */

static input_state held_in;
static bool took;

bool appapi_modal(void) { return P.app >= 0 && A.screen == SCR_DESKTOP; }

void appapi_take_input(void) {
    took = appapi_modal();
    if (!took)
        return;
    held_in = A.in;
    A.in.clicked = A.in.pressed = A.in.right_clicked = false;
    A.in.wheel = 0;
    A.in.nkeys = 0;
    A.in.nraw = 0;
    A.in.mx = A.in.my = -10000;
}

static bool toggle_row(rect r, const char *label, const char *note, bool on, font *fb, font *fs, color ink, color muted) {
    bool hit = pt_in(r, A.in.mx, A.in.my);
    if (hit)
        fill_round(A.frame, r, D.radius * 0.4f + 2, color_alpha(D.accent, 0.07f));
    rect sw = {r.x + r.w - 58, r.y + (r.h - 28) / 2, 46, 28};
    fill_round(A.frame, sw, 14, on ? D.accent : color_alpha(D.surface_text, 0.18f));
    int kx = on ? sw.x + sw.w - 25 : sw.x + 3;
    fill_round(A.frame, (rect){kx, sw.y + 3, 22, 22}, 11, on ? readable_on(D.accent) : RGBA(255, 255, 255, 255));
    draw_text(A.frame, fb, r.x + 14, r.y + 10, label, ink);
    draw_text(A.frame, fs, r.x + 14, r.y + 12 + font_line_height(fb), note, muted);
    return hit && A.in.clicked;
}

void appapi_draw(void) {
    if (!took)
        return;
    A.in = held_in;
    took = false;
    if (P.app < 0)
        return;
    struct appent *a = &apps[P.app];
    float e = (float)(A.now - P.opened) / 180.0f;
    e = e > 1 ? 1 : e;
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), 0.32f * e));
    int w = A.W - 80 < 560 ? A.W - 80 : 560, h = 340;
    rect r = {(A.W - w) / 2, (A.H - h) / 2 + (int)((1 - e) * 16), w, h};
    float rad = D.radius + 6;
    shadow_round(A.frame, (rect){r.x, r.y + 16, r.w, r.h}, rad, 48, RGBA(0, 0, 0, 110));
    fill_round(A.frame, r, rad, D.surface | 0xff000000u);
    stroke_round(A.frame, r, rad, 1, color_alpha(D.surface_text, 0.12f));
    color ink = theme_fit(D.surface_text, D.surface | 0xff000000u, 4.5f);
    color muted = theme_fit(D.muted, D.surface | 0xff000000u, 4.5f);
    font *ft = font_get(D.font_heading, D.body_px + 5), *fb = font_get(D.font_body, D.body_px);
    font *fs = font_get(D.font_body, D.body_px - 2 >= 12 ? D.body_px - 2 : 12);
    char title[160], who[120];
    snprintf(title, sizeof title, "%s wants to use your AI", a->label);
    snprintf(who, sizeof who, "%s", D.ai_name[0] ? D.ai_name : "Your companion");
    draw_text(A.frame, ft, r.x + 24, r.y + 22, title, ink);
    char sub[200];
    snprintf(sub, sizeof sub, "%s would think and speak for it, as %s. Your keys stay here.", who, who);
    draw_text(A.frame, fs, r.x + 24, r.y + 26 + font_line_height(ft), sub, muted);
    int rowh = font_line_height(fb) + font_line_height(fs) + 22;
    rect r1 = {r.x + 12, r.y + 96, r.w - 24, rowh}, r2 = {r.x + 12, r1.y + rowh + 8, r.w - 24, rowh};
    char note2[160];
    snprintf(note2, sizeof note2, "What %s knows about you, both ways", who);
    if (toggle_row(r1, "Use your AI", "Its conversations go to the mind you chose", P.ai, fb, fs, ink, muted))
        P.ai = !P.ai;
    if (toggle_row(r2, "Share what I know about you", note2, P.mem, fb, fs, ink, muted))
        P.mem = !P.mem;
    int by = r.y + r.h - 64;
    rect ok = {r.x + r.w - 24 - 130, by, 130, 44}, no = {ok.x - 12 - 120, by, 120, 44};
    bool done = false, allow = false;
    if (ui_button(no, "Not now", font_get(D.font_body, D.body_px), color_alpha(D.surface_text, 0.08f), D.surface_text,
                  D.radius * 0.5f + 4, true))
        done = true;
    if (ui_button(ok, "Allow", font_get(D.font_body, D.body_px), D.accent, readable_on(D.accent), D.radius * 0.5f + 4,
                  true))
        done = allow = true;
    for (int i = 0; i < A.in.nkeys; i++) {
        if (A.in.keys[i] == K_ESC)
            done = true;
        else if (A.in.keys[i] == K_ENTER)
            done = allow = true;
    }
    if (done) {
        answered(P.app, allow && P.ai, allow && P.mem);
        P.app = -1;
    }
}

/* ---- Settings > Apps ---- */

int appapi_app_count(void) {
    int n = 0;
    for (int i = 0; i < napps; i++)
        if (apps[i].ai >= 0 || apps[i].mem >= 0)
            n++;
    return n;
}

static struct appent *asked(int i) {
    for (int k = 0; k < napps; k++)
        if ((apps[k].ai >= 0 || apps[k].mem >= 0) && i-- == 0)
            return &apps[k];
    return NULL;
}

const char *appapi_app_label(int i) {
    struct appent *a = asked(i);
    return a ? a->label : "";
}

int appapi_app_allows(int i, bool memory) {
    struct appent *a = asked(i);
    return !a ? -1 : memory ? a->mem : a->ai;
}

void appapi_app_set(int i, bool memory, bool allow) {
    struct appent *a = asked(i);
    if (!a)
        return;
    if (memory)
        a->mem = allow;
    else
        a->ai = allow;
    consent_save();
    if (!allow) /* what it was following, it no longer hears */
        for (int k = 0; k < nconns; k++)
            if (&apps[conns[k].app] == a && (memory ? false : conns[k].state == C_EVENTS)) {
                conns[k].closing = true;
                conns[k].state = C_READ;
            }
}
