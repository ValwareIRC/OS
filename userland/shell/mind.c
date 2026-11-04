#define _GNU_SOURCE
#include "mind.h"
#include "app.h"
#include "models.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../lib/http.h"

/* The mind on this computer: llama.cpp's server with the model chosen in
 * models.c: the small one that comes in the boot image (a file backed by the
 * boot module, so it costs no copy), or one they downloaded onto the disk.
 * Started when the person chooses it, or at boot if they did. */

#define SERVER "/usr/bin/llama-server"

static int pid;
static job *health;      /* asks the server until its model is loaded */
static bool ready;
static uint64_t woke_at;
static char died[240]; /* how it ended, when it ended by itself (mind_builtin_died) */
static uint64_t pending_since; /* asked to start while its model was still being read from the disc */

/* The model is being read from the disc by the kernel (main.c): PATH.coming until it is whole. */
static bool model_coming(void) {
    char c[300];
    snprintf(c, sizeof c, "%s.coming", models_selected_path());
    return access(models_selected_path(), R_OK) != 0 && access(c, F_OK) == 0;
}

/* Still on the disc (PATH.ondisc): the kernel leaves it there until it is asked for, so a person whose mind is
 * elsewhere never waits for 5 GB at the start, nor gives it the memory. */
#define FROM_DISC "/proc/sys/kernel/os_from_disc"
static bool model_on_disc(void) {
    char c[300];
    snprintf(c, sizeof c, "%s.ondisc", models_selected_path());
    return access(models_selected_path(), R_OK) != 0 && access(c, F_OK) == 0;
}

/* Asks the kernel for it: PATH.coming from now, the file when it is whole. */
static bool ask_from_disc(void) {
    FILE *f = fopen(FROM_DISC, "w");
    if (!f)
        return false;
    bool ok = fprintf(f, "%s\n", models_selected_path()) > 0;
    ok = fclose(f) == 0 && ok;
    LOG("mind: %s asked for from the disc%s\n", models_selected()->name, ok ? "" : " (refused)");
    return ok;
}

/* Memory left now, in MB (MemAvailable). */
static long mem_available_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long mb = -1;
    unsigned long long kb;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1)
            mb = (long)(kb / 1024);
    if (f)
        fclose(f);
    return mb;
}

/* The server is gone without being asked to stop: says why, in words for the person. */
static void note_death(int st) {
    long avail = mem_available_mb();
    bool killed = WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;                            /* for memory, by the system */
    bool crashed = WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS); /* often memory it didn't get */
    const char *name = models_selected()->name;
    if (!ready)
        snprintf(died, sizeof died, "%s %s while loading%s.", name, crashed ? "crashed" : "stopped",
                 killed ? ": this computer doesn't have enough memory for it. A smaller model in Settings > AI will fit"
                 : crashed ? " (often a sign it ran short of memory; a smaller model in Settings > AI may do)"
                           : ". Its file may be damaged; downloading it again in Settings > AI may help");
    else
        snprintf(died, sizeof died, "%s %s while answering%s. It is starting again.", name,
                 crashed ? "crashed" : "stopped", killed ? " (this computer ran short of memory)" : "");
    LOG("mind: the server ended by itself (%s %d, %ld MB free now); its last words:\n", WIFSIGNALED(st) ? "signal" : "exit",
        WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st), avail);
    FILE *lf = fopen("/tmp/os-mind.log", "r");
    if (lf) { /* the end of its log, line by line */
        static char tail[2400];
        fseek(lf, -(long)sizeof tail + 1, SEEK_END);
        size_t k = fread(tail, 1, sizeof tail - 1, lf);
        tail[k] = 0;
        fclose(lf);
        for (char *line = strtok(tail, "\n"); line; line = strtok(NULL, "\n"))
            LOG("mind: | %.300s\n", line);
    }
}

bool mind_builtin_died(char *why, int n) {
    mind_builtin_running(); /* reaps it, if it ended */
    if (!died[0])
        return false;
    snprintf(why, (size_t)n, "%s", died);
    died[0] = 0;
    return true;
}

/* llama-server answers /health with 503 while the model loads, 200 after. */
static void health_child(void *arg, char **result) {
    (void)arg;
    for (int i = 0; i < 1800; i++) {
        struct http_response r = {0};
        http_request("GET", "http://127.0.0.1:8080/health", NULL, NULL, 0, 5, &r);
        int st = r.status;
        http_response_free(&r);
        if (st == 200) {
            *result = strdup("ok");
            return;
        }
        sleep(1);
    }
    *result = strdup("no");
}

bool mind_builtin_ready(void) {
    if (!mind_builtin_running()) /* gone (mind_builtin_died says why) */
        ready = false;
    if (ready)
        return true;
    if (health && pid <= 0) { /* it ended while loading: nothing to wait for */
        job_free(health);
        health = NULL;
        return false;
    }
    char *res = NULL;
    if (health && job_poll(health, &res)) {
        ready = res && !strcmp(res, "ok");
        free(res);
        job_free(health);
        health = NULL;
        LOG("mind: %s after %llus\n", ready ? "awake" : "never woke", (unsigned long long)(now_ms() - woke_at) / 1000);
        /* what it took, in its own words (the memory it needs is reckoned before it starts: models.c) */
        FILE *lf = fopen("/tmp/os-mind.log", "r");
        char line[300];
        while (lf && fgets(line, sizeof line, lf))
            if (strstr(line, "buffer size") || strstr(line, "KV self size") || strstr(line, "kv_cache: size") ||
                strstr(line, "REPACK") || strstr(line, "flash_attn"))
                LOG("mind: | %.200s", line);
        if (lf)
            fclose(lf);
    }
    return ready;
}

int mind_builtin_waking_for(void) {
    if (pending_since)
        return (int)((now_ms() - pending_since) / 1000) + 1;
    return pid > 0 && !ready ? (int)((now_ms() - woke_at) / 1000) : 0;
}

/* About how long until it answers, from now: read from the disc first (about 25 s for 5 GB), then loaded (about
 * 30 s); less what has gone by (for an app waiting on it: appapi.c). */
int mind_builtin_load_seconds(void) {
    if (ready)
        return 0;
    int total = (model_on_disc() || model_coming() ? 25 : 0) + 30, gone = mind_builtin_waking_for();
    return total - gone > 5 ? total - gone : 5;
}

bool mind_builtin_present(void) {
    return (access(models_selected_path(), R_OK) == 0 || model_coming() || model_on_disc()) && access(SERVER, X_OK) == 0;
}

void mind_builtin_tick(void) {
    if (!pending_since)
        return;
    if (access(models_selected_path(), R_OK) == 0) {
        LOG("mind: its model is here (read from the disc in %llus)\n",
            (unsigned long long)(now_ms() - pending_since) / 1000);
        pending_since = 0;
        mind_builtin_start();
    } else if (!model_coming() && !(model_on_disc() && now_ms() - pending_since < 5000)) {
        /* (on the disc a moment after asking: the kernel's reader hasn't begun yet) */
        pending_since = 0;
        snprintf(died, sizeof died, "%s couldn't be read from the system's disc.", models_selected()->name);
        LOG("mind: its model never came from the disc\n");
    }
}

bool mind_builtin_running(void) {
    int st = 0;
    if (pid > 0 && waitpid(pid, &st, WNOHANG) == pid) {
        pid = 0;
        note_death(st);
        ready = false;
    }
    return pid > 0;
}

bool mind_builtin_alive(void) {
    if (pid <= 0)
        return false;
    char path[32], buf[256];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *e = strrchr(buf, ')');
    return e && e[1] == ' ' && e[2] != 'Z' && e[2] != 'X';
}

void mind_builtin_start(void) {
    if (mind_builtin_running() || !mind_builtin_present())
        return;
    if (model_on_disc() && !ask_from_disc()) {
        snprintf(died, sizeof died, "%s couldn't be read from the system's disc.", models_selected()->name);
        return;
    }
    if (model_coming() || model_on_disc()) { /* being read from the disc: it starts as soon as it is whole (tick) */
        if (!pending_since) {
            pending_since = now_ms();
            LOG("mind: %s is still being read from the disc; it starts when that's done\n", models_selected()->name);
        }
        return;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    char threads[16], model[256];
    snprintf(threads, sizeof threads, "%ld", cpus > 1 ? cpus : 1);
    snprintf(model, sizeof model, "%s", models_selected_path());
    /* as long a conversation as fits in memory beside the model */
    char ctx[16];
    snprintf(ctx, sizeof ctx, "%d", models_context());
    bool repack = models_repack();
    /* The built-in model is already in memory: mapped, it costs nothing.
     * One on the disk loads far faster read in big pieces than page by page. */
    bool on_disk = strncmp(model, "/usr/share/", 11) != 0;
    ready = false;
    woke_at = now_ms();
    died[0] = 0;
    pid = fork();
    if (pid == 0) {
        setsid();
        FILE *log = fopen("/tmp/os-mind.log", "w");
        if (log) {
            dup2(fileno(log), 1);
            dup2(fileno(log), 2);
        }
        /* it answers only to the system (ai_local_key); its health is open */
        char *argv[24] = {"llama-server", "-m", model, "-t", threads, "-c", ctx, "--host", "127.0.0.1", "--port",
                          "8080", "--alias", "os", "--jinja", "--no-webui", "--api-key-file", AI_LOCAL_KEY_FILE};
        int n = 17;
        if (on_disk)
            argv[n++] = "--load-mode", argv[n++] = "none";
        if (!repack)
            argv[n++] = "--no-repack"; /* (no room for its faster copy of the weights: models_context) */
        argv[n] = NULL;
        execv(SERVER, argv);
        _exit(127);
    }
    if (health)
        job_free(health);
    health = job_start(health_child, NULL);
    LOG("mind: %s starting on this computer (pid %d, %s tokens)\n", models_selected()->name, pid, ctx);
}

void mind_builtin_stop(void) {
    pending_since = 0;
    if (pid > 0) {
        kill(-pid, SIGTERM);
        waitpid(pid, NULL, 0);
        pid = 0;
        ready = false;
    }
}

/* ---- standing in: the mind on this computer, while theirs can't be reached ----
 * Their mind is elsewhere (Claude, a cloud model) and stopped answering: they are asked whether the one on this
 * computer may think instead until it's back (it is read from the disc first, or from the disk if a copy is
 * kept: a minute or more). Never without their yes. While it stands in, theirs is asked again every minute; once
 * it answers, it is theirs again, this one stops, and they are told. Their choice is never saved over: the
 * stand-in lives in A.cfg in memory only (only onboarding and Settings save it, and Settings ends this). */

#define D (A.d)

static struct {
    int stage; /* 0 none, 1 asking, 2 starting, 3 standing in */
    struct ai_config theirs;
    const struct ai_provider *theirs_p;
    uint64_t declined_at, probe_at;
    job *probe;
    char why[200];
} fb;

bool mind_fallback_pending(void) { return fb.stage == 1 || fb.stage == 2; }
bool mind_fallback_asking(void) { return fb.stage == 1; }

bool mind_unreachable(const char *e) {
    return e && (strstr(e, "could not connect") || strstr(e, "could not find") || strstr(e, "timed out") ||
                 strstr(e, "No answer came back") || strstr(e, "Connection reset"));
}

void mind_fallback_offer(const char *why) {
    if (fb.stage || !A.provider || !strcmp(A.provider->id, "builtin") || !mind_builtin_present())
        return;
    if (fb.declined_at && A.now - fb.declined_at < 10 * 60000) /* "not now": not again for a while */
        return;
    snprintf(fb.why, sizeof fb.why, "%s", why ? why : "");
    fb.stage = 1;
    LOG("mind: %s isn't answering (%s): asking whether the one on this computer may stand in\n", A.provider->name,
        fb.why);
}

void mind_fallback_forget(void) {
    if (fb.stage) /* (what they chose is in A.cfg already) */
        LOG("mind: they chose a mind: the stand-in ends\n");
    if (fb.probe) {
        job_free(fb.probe);
        fb.probe = NULL;
    }
    fb.stage = 0;
}

static void stand_in(void) {
    const struct ai_provider *p = ai_find_provider("builtin");
    if (!p)
        return;
    fb.theirs = A.cfg, fb.theirs_p = A.provider;
    struct ai_config c;
    memset(&c, 0, sizeof c);
    snprintf(c.provider, sizeof c.provider, "%s", p->id);
    snprintf(c.base_url, sizeof c.base_url, "%s", p->base_url);
    snprintf(c.model, sizeof c.model, "%s", p->default_model ? p->default_model : "os");
    A.cfg = c, A.provider = p;
    mind_builtin_start();
    fb.stage = 2;
    fb.probe_at = A.now + 60000;
    LOG("mind: standing in for %s (they said yes)\n", fb.theirs_p->name);
    toast("Starting the model on this computer...");
}

static void probe_child(void *arg, char **result) {
    const struct ai_config *c = arg;
    char **ids = NULL, err[200] = "";
    int n = ai_list_models(c, &ids, err, sizeof err);
    if (n > 0)
        ai_free_models(ids, n);
    *result = strdup(n >= 0 ? "1" : "0");
}

static void back_to_theirs(void) {
    A.cfg = fb.theirs, A.provider = fb.theirs_p;
    mind_builtin_stop(); /* (its file lets go of the memory a minute later: models.c) */
    fb.stage = 0;
    LOG("mind: %s answers again: it is theirs again\n", A.provider->name);
    char b[200];
    snprintf(b, sizeof b, "%s is answering again, so %s is back on it.", A.provider->name,
             D.ai_name[0] ? D.ai_name : "your companion");
    toast("%s", b);
}

bool mind_fallback_tick(void) {
    if (fb.stage == 2 && mind_builtin_ready()) {
        fb.stage = 3;
        LOG("mind: the one on this computer is standing in\n");
        return true;
    }
    if (fb.stage == 2 && !mind_builtin_running() && !mind_builtin_waking_for()) { /* it couldn't start */
        A.cfg = fb.theirs, A.provider = fb.theirs_p;
        fb.stage = 0;
        fb.declined_at = A.now; /* (not asked again straight away: it would fail the same way) */
        toast("The model on this computer couldn't start.");
        LOG("mind: the stand-in couldn't start\n");
        return false;
    }
    if (fb.stage == 3 || fb.stage == 2) { /* theirs, asked again every minute */
        char *res = NULL;
        if (fb.probe && job_poll(fb.probe, &res)) {
            job_free(fb.probe);
            fb.probe = NULL;
            bool back = res && res[0] == '1';
            free(res);
            if (back) {
                back_to_theirs();
                return false;
            }
        }
        if (!fb.probe && A.now >= fb.probe_at) {
            fb.probe_at = A.now + 60000;
            fb.probe = job_start(probe_child, &fb.theirs);
        }
    }
    return false;
}

void mind_fallback_draw(void) {
    if (fb.stage != 1)
        return;
    font *f = font_get(D.font_body, D.body_px), *small = font_get(D.font_body, D.body_px - 3),
         *h = font_get(D.font_heading, D.heading_px * 0.6f);
    const char *who = D.ai_name[0] ? D.ai_name : "Your companion";
    bool from_disc = model_on_disc() || model_coming();
    char title[160], body[600];
    snprintf(title, sizeof title, "%s isn't answering", A.provider ? A.provider->name : "Your AI");
    snprintf(body, sizeof body,
             "%s can think with the model on this computer (%s) until it's back. It takes %s to start%s, and it's "
             "private: nothing leaves this computer.",
             who, models_selected()->name, from_disc ? "a minute or two" : "about a minute",
             from_disc ? " (it's read from the system's disc first)" : "");
    int w = 640, body_h = draw_text_wrapped(NULL, f, (rect){0, 0, w - 56, 400}, body, 0, 1.35f);
    int why_h = fb.why[0] ? draw_text_wrapped(NULL, small, (rect){0, 0, w - 56, 200}, fb.why, 0, 1.3f) + 12 : 0;
    int hgt = 170 + body_h + why_h;
    rect r = {(A.W - w) / 2, A.H - hgt - 150, w, hgt};
    if (r.y < 20)
        r.y = 20;
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(D.bg, 0.3f));
    shadow_round(A.frame, (rect){r.x, r.y + 12, r.w, r.h}, D.radius + 6, 40, RGBA(0, 0, 0, 100));
    blur_fast(A.frame, r, D.radius + 6, 12);
    fill_round(A.frame, r, D.radius + 6, color_alpha(D.surface, 0.97f));
    stroke_round(A.frame, r, D.radius + 6, 1.5f, color_alpha(D.accent, 0.8f));
    int y = r.y + 24;
    draw_text(A.frame, h, r.x + 28, y, title, D.surface_text);
    y += (int)(D.heading_px * 0.6f * 1.5f);
    y += draw_text_wrapped(A.frame, f, (rect){r.x + 28, y, r.w - 56, body_h + 4}, body, D.surface_text, 1.35f) + 12;
    if (fb.why[0])
        draw_text_wrapped(A.frame, small, (rect){r.x + 28, y, r.w - 56, why_h}, fb.why, D.muted, 1.3f);
    int bh = font_line_height(f) + 18, by = r.y + r.h - bh - 22;
    rect no_r = {r.x + 28, by, 180, bh}, yes_r = {r.x + r.w - 228, by, 200, bh};
    bool no = ui_button(no_r, "Not now", f, color_alpha(D.surface_text, 0.12f), D.surface_text, D.radius * 0.6f, true);
    bool yes = ui_button(yes_r, "Use this computer", f, D.accent, readable_on(D.accent), D.radius * 0.6f, true);
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            no = true;
    static uint64_t logged;
    if (A.now - logged > 1000) {
        logged = A.now;
        LOGRECT("standin-yes", yes_r);
        LOGRECT("standin-no", no_r);
    }
    if (yes) {
        stand_in();
    } else if (no) {
        fb.stage = 0;
        fb.declined_at = A.now;
        LOG("mind: they said not now to the stand-in\n");
    }
}
