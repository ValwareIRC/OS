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
    if (ready)
        return true;
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

int mind_builtin_waking_for(void) { return pid > 0 && !ready ? (int)((now_ms() - woke_at) / 1000) : 0; }

bool mind_builtin_present(void) { return access(models_selected_path(), R_OK) == 0 && access(SERVER, X_OK) == 0; }

bool mind_builtin_running(void) {
    if (pid > 0 && waitpid(pid, NULL, WNOHANG) == pid)
        pid = 0;
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
    pid = fork();
    if (pid == 0) {
        setsid();
        FILE *log = fopen("/tmp/os-mind.log", "w");
        if (log) {
            dup2(fileno(log), 1);
            dup2(fileno(log), 2);
        }
        execl(SERVER, "llama-server", "-m", model, "-t", threads, "-c", ctx, "--host", "127.0.0.1", "--port", "8080",
              "--alias", "os", "--jinja", "--no-webui", on_disk ? "--load-mode" : (char *)NULL, "none", (char *)NULL);
        _exit(127);
    }
    if (health)
        job_free(health);
    health = job_start(health_child, NULL);
    LOG("mind: %s starting on this computer (pid %d, %s tokens)\n", models_selected()->name, pid, ctx);
}

void mind_builtin_stop(void) {
    if (pid > 0) {
        kill(-pid, SIGTERM);
        waitpid(pid, NULL, 0);
        pid = 0;
        ready = false;
    }
}
