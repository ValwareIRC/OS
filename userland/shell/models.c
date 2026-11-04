#define _GNU_SOURCE
#include "models.h"
#include "app.h"
#include "mind.h"
#include "nerve.h"
#include "settings.h"
#include "launcher.h"
#include "tray.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

/* Models that run on this computer, through llama.cpp's server (mind.c).
 *
 * One comes with the system, small on purpose (Qwen3 0.6B): there is always
 * a mind, even with no internet. The others are downloaded, onto the disk's
 * file system, and only offered when this computer can run them: each needs
 * about its file's size in memory, plus room for the conversation, and its
 * speed follows from how much the processors must read per word. The
 * catalog's links and sizes were checked against Hugging Face (2026-10-14);
 * a download is kept only if it arrives whole, at exactly that size. */

#define BUILTIN_FILE "/usr/share/os/mind/model.gguf"
#define MODELS_DIR "/var/lib/os-data/models"
#define BUILTIN_KEPT MODELS_DIR "/builtin.gguf" /* the built-in model, copied to the disk */
#define HF "https://huggingface.co/"

static const model_info catalog[] = {
    {"qwen3-8b", "Qwen3 8B", "Alibaba", "Comes with the system: capable, good with tools, there for when there's no internet.",
     NULL, 5027783488, true},
    {"qwen3-0.6b", "Qwen3 0.6B", "Alibaba", "Tiny and very quick; for a computer with little memory.",
     HF "Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf", 639446688, false},
    {"qwen3-1.7b", "Qwen3 1.7B", "Alibaba", "Small and quick. Fine for chatting and simple help.",
     HF "unsloth/Qwen3-1.7B-GGUF/resolve/main/Qwen3-1.7B-Q4_K_M.gguf", 1107409472, false},
    {"llama3.2-3b", "Llama 3.2 3B", "Meta", "Meta's small model: friendly, quick, good at everyday questions.",
     HF "bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf", 2019377696, false},
    {"qwen3-4b", "Qwen3 4B", "Alibaba", "A good all-rounder for its size; thinks things through.",
     HF "Qwen/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf", 2497280256, false},
    {"gemma3-4b", "Gemma 3 4B", "Google", "Warm and articulate; good with words.",
     HF "unsloth/gemma-3-4b-it-GGUF/resolve/main/gemma-3-4b-it-Q4_K_M.gguf", 2489894016, false},
    {"phi4-mini", "Phi-4 mini", "Microsoft", "Strong at reasoning and code for its size.",
     HF "unsloth/Phi-4-mini-instruct-GGUF/resolve/main/Phi-4-mini-instruct-Q4_K_M.gguf", 2491874272, false},
    {"llama3.1-8b", "Llama 3.1 8B", "Meta", "A capable everyday model.",
     HF "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF/resolve/main/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf", 4920739232,
     false},
    {"gemma3-12b", "Gemma 3 12B", "Google", "Thoughtful and fluent.",
     HF "unsloth/gemma-3-12b-it-GGUF/resolve/main/gemma-3-12b-it-Q4_K_M.gguf", 7300778336, false},
    {"qwen3-14b", "Qwen3 14B", "Alibaba", "Close to the big online models for everyday use.",
     HF "Qwen/Qwen3-14B-GGUF/resolve/main/Qwen3-14B-Q4_K_M.gguf", 9001752960, false},
    {"mistral-small-24b", "Mistral Small 3.2", "Mistral", "Very capable; for a powerful computer.",
     HF "unsloth/Mistral-Small-3.2-24B-Instruct-2506-GGUF/resolve/main/Mistral-Small-3.2-24B-Instruct-2506-Q4_K_M.gguf",
     14333922848, false},
    {"qwen3-32b", "Qwen3 32B", "Alibaba", "The strongest here; needs a great deal of memory.",
     HF "Qwen/Qwen3-32B-GGUF/resolve/main/Qwen3-32B-Q4_K_M.gguf", 19762149024, false},
};
#define NMODELS ((int)(sizeof catalog / sizeof catalog[0]))

int models_count(void) { return NMODELS; }
const model_info *models_get(int i) { return i >= 0 && i < NMODELS ? &catalog[i] : NULL; }

static void path_of(int i, char *out, size_t n) {
    if (catalog[i].builtin) /* in memory from the boot, or kept on the disk once it was let go */
        snprintf(out, n, "%s", access(BUILTIN_FILE, R_OK) != 0 && access(BUILTIN_KEPT, R_OK) == 0 ? BUILTIN_KEPT : BUILTIN_FILE);
    else
        snprintf(out, n, MODELS_DIR "/%s.gguf", catalog[i].id);
}

/* Here, whole. */
static bool installed(int i) {
    char p[256];
    path_of(i, p, sizeof p);
    struct stat st;
    return stat(p, &st) == 0 && (catalog[i].builtin || (uint64_t)st.st_size == catalog[i].bytes);
}

static int selected_index(void) {
    const char *id = settings_get("mind_model", "");
    for (int i = 0; i < NMODELS; i++)
        if (!strcmp(catalog[i].id, id) && installed(i))
            return i;
    return 0;
}

const model_info *models_selected(void) { return &catalog[selected_index()]; }

const char *models_selected_path(void) {
    static char p[256];
    path_of(selected_index(), p, sizeof p);
    return p;
}

void models_select(int i) {
    if (i < 0 || i >= NMODELS || !installed(i))
        return;
    settings_set("mind_model", catalog[i].id);
    LOG("models: %s chosen\n", catalog[i].id);
    if (mind_builtin_running()) { /* the server takes the new model */
        mind_builtin_stop();
        mind_builtin_start();
    }
}

/* ---- this computer ---- */

static struct {
    bool read;
    uint64_t mem_mb, free_disk_mb;
    int cpus;
    bool disk; /* the disk's file system is there for downloads */
} spec;

static void read_specs(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long kb;
        if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
            spec.mem_mb = kb / 1024;
    }
    if (f)
        fclose(f);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    spec.cpus = n > 0 ? (int)n : 1;
    struct statvfs vs;
    struct stat st;
    spec.disk = stat("/var/lib/os-data/lost+found", &st) == 0 && statvfs("/var/lib/os-data", &vs) == 0;
    spec.free_disk_mb = spec.disk ? (uint64_t)vs.f_bavail * vs.f_frsize / (1024 * 1024) : 0;
    spec.read = true;
}

void models_specs_text(char *out, int n) {
    if (!spec.read)
        read_specs();
    if (spec.disk)
        snprintf(out, (size_t)n, "This computer: %.0f GB of memory to work with, %d processor%s, %.0f GB free on the disk.",
                 spec.mem_mb / 1024.0, spec.cpus, spec.cpus == 1 ? "" : "s", spec.free_disk_mb / 1024.0);
    else
        snprintf(out, (size_t)n, "This computer: %.0f GB of memory to work with, %d processor%s, no disk to download to.",
                 spec.mem_mb / 1024.0, spec.cpus, spec.cpus == 1 ? "" : "s");
}

/* What is left for it once the system, the desktop and an app or two have theirs. */
static uint64_t room_mb(void) { return spec.mem_mb > 1800 ? spec.mem_mb - 1800 : 0; }

/* The conversation a model keeps in mind (its KV cache) costs memory for
 * every word of it: about this many KB a token, by the model's size (at 16
 * bits: kept at 8, with flash attention, the processor read prompts a quarter slower, measured). */
static uint64_t kv_kb_per_token(int i) {
    uint64_t mb = catalog[i].bytes >> 20;
    return mb < 1500 ? 112 : mb < 6000 ? 144 : mb < 10000 ? 160 : 256;
}

/* Memory a model needs while it runs: its weights, a conversation of
 * `ctx` tokens, and the server around it. */
static uint64_t need_mb_ctx(int i, int ctx) {
    return (catalog[i].bytes >> 20) * 110 / 100 + (uint64_t)ctx * kv_kb_per_token(i) / 1024 + 500;
}

/* The companion's own instructions take about 8000 tokens: no model is
 * started with room for fewer than this, and one fits when it can hold it. */
#define MIN_CONTEXT 12288
static uint64_t need_mb(int i) { return need_mb_ctx(i, MIN_CONTEXT); }

static bool fits(int i) { return need_mb(i) <= room_mb(); }

/* Memory free right now, in MB (MemAvailable): what the system, the desktop, its apps and the base system
 * (kept in memory) already hold is not there to take. */
static long avail_mb(void) {
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

/* The conversation it can hold, sized when it starts from what is free then: its weights (unless they are
 * the model that came with the system, in memory already), the conversation and its working memory, with a
 * margin left for the desktop. Never fewer tokens than the companion's instructions need. */
static bool repack = true;

int models_context(void) {
    if (!spec.read)
        read_specs();
    int i = selected_index();
    long avail = avail_mb();
    if (avail < 0)
        avail = (long)room_mb();
    /* the reflexes' small model, when it will start and hasn't yet (its download finishing): its room kept (a
     * 12 GB computer: it started beside the built-in model, and the built-in model was ended to make room) */
    long reserve = reflex_reserve_mb();
    if (reserve) {
        LOG("models: %ld MB kept for the reflexes' model, which hasn't started yet\n", reserve);
        avail -= reserve;
    }
    char p[256];
    path_of(i, p, sizeof p);
    bool resident = !strncmp(p, "/usr/share/", 11); /* in memory from the boot (or read from the disc into it) */
    long weights = resident ? 0 : (long)((catalog[i].bytes >> 20) * 110 / 100);
    /* llama.cpp's processor code rearranges most weights into a copy as it loads (faster on AVX2): for a model
     * already in memory that copy is extra, the model's size again (uncounted, the built-in model ran out of
     * memory on a 12 GB computer while loading). With room for it, it is made; else the weights are used as
     * they are (a little slower) rather than the server dying half loaded. */
    long copy = resident ? (long)(catalog[i].bytes >> 20) : 0;
    static const int sizes[] = {32768, 24576, 16384, MIN_CONTEXT};
    for (int pass = 0; pass < 2; pass++) {
        repack = pass == 0;
        for (int k = 0; k < 4; k++) {
            if (repack && copy && sizes[k] < 16384) /* (a short conversation isn't worth the speed) */
                break;
            long need = weights + (repack ? copy : 0) + (long)((uint64_t)sizes[k] * kv_kb_per_token(i) / 1024) + 900;
            if (need <= avail - 1024) {
                LOG("models: %s gets %d tokens (%ld MB free, %ld MB needed%s)\n", catalog[i].name, sizes[k], avail, need,
                    copy && !repack ? "; its weights used in place, no faster copy: no room for one" : "");
                return sizes[k];
            }
        }
    }
    repack = false;
    LOG("models: %s gets %d tokens, though only %ld MB are free\n", catalog[i].name, MIN_CONTEXT, avail);
    return MIN_CONTEXT; /* less couldn't hold the instructions: the request would only be refused */
}

bool models_repack(void) { return repack; }

/* Rough words a second on the processors (each word reads the whole model). */
static float words_per_s(int i) {
    float gb = (float)catalog[i].bytes / 1e9f;
    int c = spec.cpus > 16 ? 16 : spec.cpus;
    return (4.0f + 2.5f * (float)c) / gb * 0.75f;
}

/* The one to suggest: the biggest that fits and still answers at a
 * comfortable pace. */
static int recommended(void) {
    int best = 0;
    for (int i = 1; i < NMODELS; i++)
        if (fits(i) && words_per_s(i) >= 3.0f && catalog[i].bytes > catalog[best].bytes)
            best = i;
    return best;
}

/* ---- online ---- */

static struct {
    job *check;
    int state; /* 0 unknown, 1 online, -1 offline */
    uint64_t at;
} net;

static void online_child(void *arg, char **result) {
    (void)arg;
    int r = system("curl -sfI -m 8 -o /dev/null " HF " >/dev/null 2>&1");
    *result = strdup(r == 0 ? "1" : "0");
}

/* ---- downloading ---- */

static struct {
    int i; /* catalog index, -1 none */
    pid_t pid;
    char part[256];
    uint64_t got, at_bytes;
    uint64_t started, speed_at;
    float bps;
    char err[160];
    int err_i;
} dl = {-1, 0, "", 0, 0, 0, 0, 0, "", -1};

static void download(int i) {
    if (dl.i >= 0 || !spec.disk)
        return;
    if (catalog[i].bytes / (1024 * 1024) + 200 > spec.free_disk_mb) {
        snprintf(dl.err, sizeof dl.err, "Not enough room on the disk for %s.", catalog[i].name);
        dl.err_i = i;
        return;
    }
    mkdir("/var/lib/os-data", 0755); /* not there without a data disk */
    mkdir(MODELS_DIR, 0755);
    snprintf(dl.part, sizeof dl.part, MODELS_DIR "/%s.gguf.part", catalog[i].id);
    pid_t pid = fork();
    if (pid == 0) {
        int log = open("/tmp/os-model-download.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log >= 0) {
            dup2(log, 1);
            dup2(log, 2);
        }
        /* -C -: a download stopped part way goes on from where it was */
        execl("/usr/bin/curl", "curl", "-fL", "--retry", "5", "-C", "-", "-sS", "-o", dl.part, catalog[i].url,
              (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        return;
    dl.i = i;
    dl.pid = pid;
    dl.got = dl.at_bytes = 0;
    dl.started = dl.speed_at = A.now;
    dl.bps = 0;
    dl.err[0] = 0;
    LOG("models: downloading %s (%llu bytes)\n", catalog[i].id, (unsigned long long)catalog[i].bytes);
}

static void cancel_download(void) {
    if (dl.i < 0)
        return;
    kill(dl.pid, SIGTERM);
    waitpid(dl.pid, NULL, 0);
    unlink(dl.part);
    LOG("models: download of %s cancelled\n", catalog[dl.i].id);
    dl.i = -1;
}

static void remove_model(int i) {
    if (catalog[i].builtin)
        return;
    char p[256];
    path_of(i, p, sizeof p);
    unlink(p);
    if (!strcmp(settings_get("mind_model", ""), catalog[i].id))
        models_select(0);
    read_specs();
    LOG("models: %s removed\n", catalog[i].id);
}

/* ---- the built-in model, out of the way ----
 * It comes in memory with the boot (5 GB). While another mind is in use and
 * the computer is online it isn't needed: it is copied to the disk once (so
 * it is still there offline) and let go from memory. The next start brings
 * it back into memory. */
static struct {
    uint64_t next;
    job *copying;
    bool done;
} rel;

static void keep_child(void *arg, char **result) {
    (void)arg;
    int in = open(BUILTIN_FILE, O_RDONLY), out = open(BUILTIN_KEPT ".part", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    static char buf[1 << 20];
    ssize_t n = 0;
    bool ok = in >= 0 && out >= 0;
    while (ok && (n = read(in, buf, sizeof buf)) > 0)
        ok = write(out, buf, (size_t)n) == n;
    ok = ok && n == 0 && fsync(out) == 0;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    ok = ok && rename(BUILTIN_KEPT ".part", BUILTIN_KEPT) == 0;
    if (!ok)
        unlink(BUILTIN_KEPT ".part");
    *result = strdup(ok ? "1" : "0");
}

/* The kernel lets go of it when it read it from the disc (it can again): no copy to the disk needed. */
static bool kernel_release(void) {
    FILE *f = fopen("/proc/sys/kernel/os_from_disc", "w");
    if (!f)
        return false;
    bool ok = fprintf(f, "release %s\n", BUILTIN_FILE) > 0;
    ok = fclose(f) == 0 && ok;
    return ok;
}

static void let_go(void) {
    if (kernel_release()) { /* (looked at again: if it is read again, for an app or to stand in, it goes again) */
        LOG("models: the built-in model let go of memory (back on the system's disc)\n");
        return;
    }
    if (unlink(BUILTIN_FILE) == 0)
        LOG("models: the built-in model let go of memory (%s)\n",
            access(BUILTIN_KEPT, R_OK) == 0 ? "kept on the disk" : "back at the next start");
    rel.done = true;
}

static void release_poll(void) {
    char *res = NULL;
    if (rel.copying) {
        if (!job_poll(rel.copying, &res))
            return;
        job_free(rel.copying);
        rel.copying = NULL;
        LOG("models: built-in model %s the disk\n", res && res[0] == '1' ? "copied to" : "could not be copied to");
        free(res);
        if (A.provider && strcmp(A.provider->id, "builtin") && !mind_builtin_running())
            let_go();
        return;
    }
    if (rel.done || A.now < rel.next)
        return;
    rel.next = A.now + 60000;
    if (!A.provider || !strcmp(A.provider->id, "builtin") || mind_builtin_running() || access(BUILTIN_FILE, R_OK) != 0)
        return;
    if (net.state <= 0) { /* offline (or not known yet): keep it, and look again */
        if (!net.check && (!net.at || A.now - net.at > 60000)) {
            net.at = A.now;
            net.check = job_start(online_child, NULL);
        }
        return;
    }
    if (kernel_release()) { /* read from the disc: it can be again, so no copy on the disk is needed */
        LOG("models: the built-in model let go of memory (back on the system's disc)\n");
        return;
    }
    struct stat st;
    if (!spec.read)
        read_specs();
    bool kept = stat(BUILTIN_KEPT, &st) == 0 && (uint64_t)st.st_size == catalog[0].bytes;
    if (!kept && spec.disk && catalog[0].bytes / (1024 * 1024) + 1024 < spec.free_disk_mb) {
        if (apps_installing()) /* the disk is the install's first: looked at again in a minute */
            return;
        mkdir("/var/lib/os-data", 0755);
        mkdir(MODELS_DIR, 0755);
        rel.copying = job_start(keep_child, NULL);
        LOG("models: copying the built-in model to the disk before letting it go\n");
        return;
    }
    let_go();
}

void models_poll(void) {
    char *res = NULL;
    if (net.check && job_poll(net.check, &res)) {
        job_free(net.check);
        net.check = NULL;
        net.state = res && res[0] == '1' ? 1 : -1;
        LOG("models: %s\n", net.state > 0 ? "online" : "offline");
        free(res);
    }
    release_poll();
    if (dl.i < 0)
        return;
    struct stat st;
    if (stat(dl.part, &st) == 0)
        dl.got = (uint64_t)st.st_size;
    if (A.now - dl.speed_at >= 2000) {
        float b = (float)(dl.got - dl.at_bytes) * 1000.0f / (float)(A.now - dl.speed_at);
        dl.bps = dl.bps > 0 ? dl.bps * 0.6f + b * 0.4f : b;
        dl.at_bytes = dl.got;
        dl.speed_at = A.now;
    }
    int status;
    if (waitpid(dl.pid, &status, WNOHANG) != dl.pid)
        return;
    int i = dl.i;
    dl.i = -1;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && stat(dl.part, &st) == 0 &&
              (uint64_t)st.st_size == catalog[i].bytes;
    if (ok) {
        char p[256];
        path_of(i, p, sizeof p);
        rename(dl.part, p);
        read_specs();
        LOG("models: %s downloaded\n", catalog[i].id);
        notify("Model downloaded", catalog[i].name, "settings");
    } else {
        snprintf(dl.err, sizeof dl.err, "%s didn't download%s. Try again; it carries on where it stopped.",
                 catalog[i].name, WIFEXITED(status) && WEXITSTATUS(status) == 6 ? " (no internet)" : "");
        dl.err_i = i;
        LOG("models: download of %s failed (status %d, %llu bytes)\n", catalog[i].id, status,
            (unsigned long long)dl.got);
    }
}

/* ---- the list ---- */

static void size_text(uint64_t bytes, char *out, size_t n) {
    if (bytes >= 1000000000ull)
        snprintf(out, n, "%.1f GB", (double)bytes / 1e9);
    else
        snprintf(out, n, "%.0f MB", (double)bytes / 1e6);
}

static const char *pace(float wps) {
    return wps >= 10 ? "quick" : wps >= 4 ? "steady" : wps >= 2 ? "slow" : "very slow";
}

int models_list(rect c, int y, const palette *p, font *f, font *small, font *head, void (*on_use)(int i)) {
    if (!spec.read)
        read_specs();
    if (!net.check && (!net.state || A.now - net.at > 60000)) {
        net.at = A.now;
        net.check = job_start(online_child, NULL);
    }
    int y0 = y, rec = recommended(), sel = selected_index();
    static uint64_t log_at;
    bool lg = A.now - log_at > 1500;
    if (lg)
        log_at = A.now;
    char line[200];
    models_specs_text(line, sizeof line);
    draw_text(A.frame, small, c.x, y, line, p->muted);
    y += font_line_height(small) + 6;
    if (net.state < 0)
        draw_text(A.frame, small, c.x, y, "No internet right now: the built-in model is ready; others can be downloaded later.",
                  p->muted);
    else if (!spec.disk)
        draw_text(A.frame, small, c.x, y, "Downloaded models are kept on the disk, and this computer has none.", p->muted);
    y += font_line_height(small) + 14;

    int too_big = 0;
    /* the recommended one first, then the rest that fit, smallest first */
    int order[NMODELS], n = 0;
    order[n++] = rec;
    for (int i = 0; i < NMODELS; i++)
        if (i != rec)
            order[n++] = i;
    for (int k = 0; k < n; k++) {
        int i = order[k];
        bool here = installed(i), ok = fits(i) || here;
        if (!ok) {
            too_big++;
            continue;
        }
        const model_info *m = &catalog[i];
        bool busy = dl.i == i;
        int rh = 78;
        rect r = {c.x, y, c.w, rh};
        fill_round(A.frame, r, 14, color_alpha(p->ink, i == sel && here ? 0.10f : 0.05f));
        if (i == sel && here)
            stroke_round(A.frame, r, 14, 1.5f, p->accent);
        int tx = r.x + 16;
        int nw = draw_text(A.frame, f, tx, r.y + 12, m->name, p->ink);
        char by[96], sz[24];
        size_text(m->bytes, sz, sizeof sz);
        snprintf(by, sizeof by, "%s  \xc2\xb7  %s  \xc2\xb7  about %.0f words a second (%s)", m->by, sz, words_per_s(i),
                 pace(words_per_s(i)));
        const char *tag = i == rec && !(i == sel && here) ? "Recommended for this computer" : i == sel && here ? "In use" : NULL;
        int bw = 120, right = r.x + r.w - 16 - bw - 16, mx = tx + nw + 12;
        /* a narrow window: the label gets shorter, then the line about it does */
        if (tag && right - text_width(small, tag) - 16 - mx < text_width(small, by) && i == rec && !(i == sel && here))
            tag = "Recommended";
        int room = right - (tag ? text_width(small, tag) + 16 : 0) - mx;
        if (text_width(small, by) > room)
            snprintf(by, sizeof by, "%s  \xc2\xb7  about %.0f words a second", sz, words_per_s(i));
        ctl_text_fit(A.frame, small, mx, r.y + 15, room, by, p->muted);
        ctl_text_fit(A.frame, small, tx, r.y + 18 + font_line_height(f), r.w - 32 - bw - 20, m->blurb, p->muted);
        if (tag)
            draw_text(A.frame, small, right - text_width(small, tag), r.y + 15, tag, p->accent);
        rect b = {r.x + r.w - 16 - bw, r.y + (rh - 36) / 2, bw, 36};
        if (lg) /* where its button is, for tests */
            LOG("rect models-%s %d %d %d %d\n", m->id, b.x, b.y, b.w, b.h);
        if (busy) {
            /* progress across the row's foot, and Cancel */
            float t = m->bytes ? (float)dl.got / (float)m->bytes : 0;
            rect bar = {r.x + 16, r.y + rh - 10, r.w - 32 - bw - 20, 4};
            fill_round(A.frame, bar, 2, color_alpha(p->ink, 0.12f));
            fill_round(A.frame, (rect){bar.x, bar.y, (int)(bar.w * (t > 1 ? 1 : t)), bar.h}, 2, p->accent);
            char prog[64];
            float left = dl.bps > 1 ? (float)(m->bytes - dl.got) / dl.bps : 0;
            if (left > 90)
                snprintf(prog, sizeof prog, "%.0f%%, about %.0f min left", t * 100, left / 60);
            else
                snprintf(prog, sizeof prog, "%.0f%%", t * 100);
            draw_text(A.frame, small, r.x + r.w - 16 - bw - 16 - text_width(small, prog), r.y + rh - 30, prog, p->ink);
            if (ctl_button(b, "Cancel", small, false, true, p))
                cancel_download();
        } else if (here) {
            bool in_use = i == sel;
            if (ctl_button(b, in_use ? "Use it" : "Use this", small, true, true, p)) {
                models_select(i);
                if (on_use)
                    on_use(i);
            }
            if (!m->builtin) { /* a small "Remove" under the row's text */
                rect rm = {r.x + r.w - 16 - bw - 16 - 70, r.y + rh - 28, 70, 22};
                if (ui_hover(rm) && A.in.clicked)
                    remove_model(i);
                draw_text(A.frame, small, rm.x + 70 - text_width(small, "Remove"), rm.y + 3, "Remove",
                          color_alpha(p->muted, ui_hover(rm) ? 1 : 0.7f));
            }
        } else {
            bool can = spec.disk && net.state >= 0 && dl.i < 0;
            if (ctl_button(b, "Download", small, false, can, p) && can)
                download(i);
        }
        if (dl.err[0] && dl.err_i == i)
            draw_text(A.frame, small, tx, r.y + rh + 2, dl.err, RGBA(226, 92, 72, 255));
        y += rh + (dl.err[0] && dl.err_i == i ? 22 : 10);
    }
    if (too_big) {
        snprintf(line, sizeof line, "%d bigger model%s need more memory than this computer has.", too_big,
                 too_big == 1 ? "" : "s");
        draw_text(A.frame, small, c.x, y + 4, line, p->muted);
        y += font_line_height(small) + 12;
    }
    (void)head;
    return y - y0;
}
