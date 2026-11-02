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
    }
    return ready;
}

int mind_builtin_waking_for(void) {
    if (pending_since)
        return (int)((now_ms() - pending_since) / 1000) + 1;
    return pid > 0 && !ready ? (int)((now_ms() - woke_at) / 1000) : 0;
}

bool mind_builtin_present(void) {
    return (access(models_selected_path(), R_OK) == 0 || model_coming()) && access(SERVER, X_OK) == 0;
}

void mind_builtin_tick(void) {
    if (!pending_since)
        return;
    if (access(models_selected_path(), R_OK) == 0) {
        LOG("mind: its model is here (read from the disc in %llus)\n",
            (unsigned long long)(now_ms() - pending_since) / 1000);
        pending_since = 0;
        mind_builtin_start();
    } else if (!model_coming()) {
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
    if (model_coming()) { /* still being read from the disc: it starts as soon as it is whole (tick) */
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
        execl(SERVER, "llama-server", "-m", model, "-t", threads, "-c", ctx, "--host", "127.0.0.1", "--port", "8080",
              "--alias", "os", "--jinja", "--no-webui", "--api-key-file", AI_LOCAL_KEY_FILE,
              on_disk ? "--load-mode" : (char *)NULL, "none", (char *)NULL);
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
