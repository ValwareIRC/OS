#define _GNU_SOURCE
/* The reflexes: a small model on this computer that decides what an event is to the companion (nerve.c).
 * Never the service its main mind runs on, whatever that is: these judgments stay here.
 *
 * Two ways of deciding, chosen in Settings (reflex.engine):
 *   llm    a tiny model writes a few words of reason, then its answer; a grammar keeps it to the four answers
 *   jev    decision-model style: the model reads the situation and the four answers and gives each a
 *          probability (read from its logits, nothing written); the likeliest wins, and how likely it is
 *          is the confidence (unsure means remember, not wake)
 *   rules  no model: the rules alone (nerve.c)
 * Either model runs in its own llama-server (port 8092), apart from its main mind. The model is
 * downloaded once, onto the disk (LFM3 1.7B by default; Qwen3 1.7B or 0.6B instead). */
#include "nerve.h"
#include "app.h"
#include "settings.h"
#include "launcher.h"
#include "alive.h"
#include "../lib/http.h"
#include "../lib/json.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVER "/usr/bin/llama-server"
#define DIR_ "/var/lib/os-data/models"
#define PORT "8092"
#define HF "https://huggingface.co/"

static const struct {
    const char *id, *name, *url;
    unsigned long long bytes;
} models[] = {
    {"lfm3-1.7b", "LFM3 1.7B (Liquid AI)", HF "LiquidAI/LFM3-1.7B-GGUF/resolve/main/LFM3-1.7B-Q4_K_M.gguf", 1099627808ull},
    {"qwen3-1.7b", "Qwen3 1.7B", HF "unsloth/Qwen3-1.7B-GGUF/resolve/main/Qwen3-1.7B-Q4_K_M.gguf", 1107409472ull},
    {"qwen3-0.6b", "Qwen3 0.6B", HF "Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf", 639446688ull},
};
#define NMODELS ((int)(sizeof models / sizeof models[0]))

const char *reflex_engine(void) {
    const char *e = settings_get("reflex.engine", "jev");
    return !strcmp(e, "llm") || !strcmp(e, "rules") ? e : "jev";
}

static int model_index(void) {
    const char *id = settings_get("reflex.model", "lfm3-1.7b");
    for (int i = 0; i < NMODELS; i++)
        if (!strcmp(models[i].id, id))
            return i;
    return 0;
}

static void model_path(int i, char *out, size_t n) { snprintf(out, n, DIR_ "/reflex-%s.gguf", models[i].id); }

/* Whether model i is on the disk, whole: looked at every 5 s, not at every frame (a look at the disk can wait behind
 * another program's work there: with Firefox out of descriptors, these took half the desktop's time). */
static uint64_t here_looked;
static bool model_here(int i) {
    static int here_i = -1;
    static bool here;
    if (i == here_i && here_looked && A.now - here_looked < 5000)
        return here;
    char p[256];
    model_path(i, p, sizeof p);
    struct stat st;
    here = stat(p, &st) == 0 && (unsigned long long)st.st_size == models[i].bytes;
    here_i = i, here_looked = A.now ? A.now : 1;
    return here;
}

/* ---- the model, downloaded once ---- */

static struct {
    pid_t pid;
    int i;
    char part[256];
    bool failed, paused; /* paused: stopped for an app being installed, to go on from where it was */
} dl = {0, -1, "", false, false};

/* every folder on the way (without a data disk, /var/lib/os-data itself is not there yet) */
static void make_dirs(const char *path) {
    char p[256];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

static void download_tick(void) {
    if (dl.pid > 0) {
        int st;
        if (waitpid(dl.pid, &st, WNOHANG) != dl.pid) {
            if (apps_installing() && !dl.paused) {
                dl.paused = true;
                kill(dl.pid, SIGTERM); /* the part downloaded so far stays; curl -C - goes on from it */
                LOG("reflex: download stopped while an app installs\n");
            }
            return;
        }
        dl.pid = 0;
        if (dl.paused)
            return; /* not a failure: started again once the install is done */
        struct stat s;
        char p[256];
        model_path(dl.i, p, sizeof p);
        if (stat(dl.part, &s) == 0 && (unsigned long long)s.st_size == models[dl.i].bytes && rename(dl.part, p) == 0) {
            LOG("reflex: %s downloaded\n", models[dl.i].name);
            here_looked = 0; /* (looked at again at once) */
        } else {
            dl.failed = true; /* tried once this time; Settings can try again */
            char why[200] = "";
            FILE *lf = fopen("/tmp/os-reflex-download.log", "r");
            if (lf) {
                size_t n = fread(why, 1, sizeof why - 1, lf);
                why[n] = 0;
                fclose(lf);
                for (char *c = why; *c; c++)
                    if (*c == '\n' || *c == '\r')
                        *c = ' ';
            }
            LOG("reflex: %s didn't download whole (curl exit %d, %lld of %llu bytes) %s\n", models[dl.i].name,
                WIFEXITED(st) ? WEXITSTATUS(st) : -1, stat(dl.part, &s) == 0 ? (long long)s.st_size : -1LL,
                models[dl.i].bytes, why);
        }
        return;
    }
    int i = model_index();
    if (strcmp(reflex_engine(), "rules") && !model_here(i) && !dl.failed && A.now > 60000 && !apps_installing() &&
        !alive_prefs.no_downloads) {
        if (dl.paused)
            LOG("reflex: the install is done; the download goes on\n");
        dl.paused = false;
        make_dirs(DIR_);
        dl.i = i;
        snprintf(dl.part, sizeof dl.part, DIR_ "/reflex-%s.gguf.part", models[i].id);
        pid_t pid = fork();
        if (pid == 0) {
            int log = open("/tmp/os-reflex-download.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log >= 0)
                dup2(log, 1), dup2(log, 2);
            execl("/usr/bin/curl", "curl", "-fL", "--retry", "5", "-C", "-", "-sS", "-o", dl.part, models[i].url, (char *)NULL);
            _exit(127);
        }
        dl.pid = pid > 0 ? pid : 0;
        LOG("reflex: downloading %s\n", models[i].name);
    }
}

/* ---- its own server ---- */

static pid_t server;
static int server_model = -1;
static bool up;
static uint64_t started_at, checked_at;

static void server_tick(void) {
    int i = model_index();
    bool want = strcmp(reflex_engine(), "rules") && model_here(i) && access(SERVER, X_OK) == 0;
    if (server > 0 && waitpid(server, NULL, WNOHANG) == server) {
        server = 0, up = false;
        LOG("reflex: its server stopped\n");
    }
    if (server > 0 && (!want || server_model != i)) {
        kill(-server, SIGTERM);
        waitpid(server, NULL, 0);
        server = 0, up = false;
    }
    if (!want || server > 0) {
        if (server > 0 && !up && A.now - checked_at > 1000) { /* loaded yet? */
            checked_at = A.now;
            struct http_response r;
            http_request("GET", "http://127.0.0.1:" PORT "/health", NULL, NULL, 0, 1, &r);
            up = r.status == 200;
            http_response_free(&r);
            if (up)
                LOG("reflex: %s ready (%llu ms)\n", models[i].name, (unsigned long long)(A.now - started_at));
        }
        return;
    }
    char path[256];
    model_path(i, path, sizeof path);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int log = open("/tmp/os-reflex.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log >= 0)
            dup2(log, 1), dup2(log, 2);
        execl(SERVER, "llama-server", "-m", path, "-t", "2", "-c", "6144", "-np", "1", "--api-key-file", AI_LOCAL_KEY_FILE, "--host", "127.0.0.1", "--port",
              PORT, "--jinja", "--no-webui", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        server = pid, server_model = i, up = false, started_at = A.now;
        LOG("reflex: starting %s\n", models[i].name);
    }
}

bool reflex_ready(void) { return up && strcmp(reflex_engine(), "rules"); }

const char *reflex_status(void) {
    static char s[200];
    int i = model_index();
    if (!strcmp(reflex_engine(), "rules"))
        snprintf(s, sizeof s, "Rules only");
    else if (dl.pid > 0) {
        struct stat st;
        double got = stat(dl.part, &st) == 0 ? (double)st.st_size : 0;
        snprintf(s, sizeof s, "Downloading %s: %.0f%%", models[i].name, 100 * got / (double)models[i].bytes);
    } else if (!model_here(i))
        snprintf(s, sizeof s, "%s: not downloaded%s", models[i].name, dl.failed ? " (download failed)" : "");
    else
        snprintf(s, sizeof s, "%s, %s%s", models[i].name, !strcmp(reflex_engine(), "jev") ? "decision model" : "small LLM",
                 up ? ", running" : ", starting");
    return s;
}

/* ---- judging ---- */

static job *judging;
static char req_situation[1000], req_event[300], req_engine[8];
static struct {
    bool ready;
    enum nerve_verdict v;
    float conf;
    char why[120];
} out;

bool reflex_busy(void) { return judging != NULL || out.ready; }

/* What a tiny model needs to judge well (measured with tests/reflex_now.py and reflex_try.py on LFM3 1.7B):
 * decision-model style asks one yes/no question, "interrupt the person now?", and reads the probability of yes
 * (12 of 13 test events judged right with the 0.85 bar below); only when that is yes, a second, "lost or damaged
 * soon?", tells urgent apart. Small-LLM style picks a letter from worked examples and says why (10 of 13). */
static const char *const SYS_YESNO =
    "You judge events on a personal computer for its built-in assistant. You answer each question with yes or no.";
static const char *const Q_NOW = "Should the assistant interrupt the person about this right now?";
static const char *const Q_HARM = "Will something be lost or damaged soon unless someone acts?";
static const char *const SYS_LETTER =
    "You judge events on a personal computer for its built-in assistant. You answer each question about the event "
    "with one letter.";

static const struct {
    const char *situation, *event, *now, *harm, *letter, *why;
} examples[] = {
    {"The person is at the computer.", "they closed \"Remix\" (the person did this)", "no", "no", "B", "their own action"},
    {"The person is at the computer.", "they opened Files (the person did this)", "no", "no", "A", "their own action"},
    {"The person is at the computer.", "the clock passed 15:00 (it happened by itself)", "no", "no", "A", "nothing happened"},
    {"The person is away.", "a backup finished after 2 hours (it happened by itself)", "no", "no", "B", "good to know later"},
    {"The person is at the computer.", "an app finished updating (it happened by itself)", "no", "no", "B", "worth knowing, not now"},
    {"The person is at the computer.", "a calendar reminder: dentist in 15 minutes (it happened by itself)", "yes", "no", "C",
     "they need to know now"},
    {"The person is at the computer.", "a message from Sam: \"are you still coming tonight?\" (it happened by itself)", "yes",
     "no", "C", "someone is waiting for them"},
    {"The person is at the computer.", "the battery is at 3% (it happened by itself)", "yes", "yes", "D", "work will be lost"},
    {"The person is at the computer.", "the GPU is at 96°C (it happened by itself)", "yes", "yes", "D", "damage soon"},
    {"The person is typing.", "Wi-Fi reconnected after 20 seconds (it happened by itself)", "no", "no", "A", "already fine"},
    {"The person is at the computer.", "memory is at 85% (it happened by itself)", "no", "no", "B", "not a problem yet"},
    {"The person is at the computer.", "a USB drive was plugged in (it happened by itself)", "no", "no", "B", "worth knowing"},
};
#define NEXAMPLES (sizeof examples / sizeof examples[0])

static void msg(sbuf *b, const char *role, const char *text) {
    sb_printf(b, ",{\"role\":\"%s\",\"content\":", role);
    sb_json_str(b, text);
    sb_add(b, "}");
}

/* the request: worked examples, then this event; which: 'n' interrupt now? 'h' harm soon? 'l' a letter */
static char *request(char which) {
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{\"messages\":[{\"role\":\"system\",\"content\":");
    sb_json_str(&b, which == 'l' ? SYS_LETTER : SYS_YESNO);
    sb_add(&b, "}");
    const char *q = which == 'n' ? Q_NOW : which == 'h' ? Q_HARM : "A ignore, B remember, C interrupt now, D urgent?";
    char t[1600];
    for (size_t i = 0; i < NEXAMPLES; i++) {
        snprintf(t, sizeof t, "%s\nEvent: %s\n%s", examples[i].situation, examples[i].event, q);
        msg(&b, "user", t);
        if (which == 'l')
            snprintf(t, sizeof t, "%s: %s", examples[i].letter, examples[i].why);
        msg(&b, "assistant", which == 'n' ? examples[i].now : which == 'h' ? examples[i].harm : t);
    }
    snprintf(t, sizeof t, "%s\nEvent: %s\n%s", req_situation, req_event, q);
    msg(&b, "user", t);
    sb_add(&b, "],\"temperature\":0,\"chat_template_kwargs\":{\"enable_thinking\":false},");
    if (which == 'l')
        sb_add(&b, "\"max_tokens\":24,\"grammar\":\"root ::= [ABCD] \\\": \\\" [^\\\\n]{2,80}\"}");
    else
        sb_add(&b, "\"max_tokens\":1,\"logprobs\":true,\"top_logprobs\":20}");
    return b.p;
}

static json *ask(const char *body, struct http_response *r) {
    char auth[120];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", ai_local_key());
    const char *h[] = {"Content-Type: application/json", auth, NULL};
    http_request("POST", "http://127.0.0.1:" PORT "/v1/chat/completions", h, body, strlen(body), 30, r);
    return r->status == 200 && r->body ? json_parse(r->body, r->body_len) : NULL;
}

/* the probability of "yes" to one question; -1 when nothing came back */
static float p_yes(char which, char *err, size_t n) {
    char *body = request(which);
    struct http_response r;
    json *j = ask(body, &r);
    free(body);
    float y = 0, no = 0;
    json *choice = j ? json_path(j, "choices") : NULL;
    choice = choice ? choice->child : NULL;
    json *lp = choice ? json_path(choice, "logprobs.content") : NULL;
    json *top = lp && lp->child ? json_get(lp->child, "top_logprobs") : NULL;
    for (json *t = top ? top->child : NULL; t; t = t->next) {
        const char *tok = json_str(json_get(t, "token"), "");
        while (*tok == ' ')
            tok++;
        json *l = json_get(t, "logprob");
        if (!l)
            continue;
        if (!strcasecmp(tok, "yes"))
            y += expf((float)l->num);
        else if (!strcasecmp(tok, "no"))
            no += expf((float)l->num);
    }
    if (!r.status)
        snprintf(err, n, "%s", r.error);
    if (j)
        json_free(j);
    http_response_free(&r);
    return y + no > 0 ? y / (y + no) : -1;
}

static void judge_child(void *arg, char **result) {
    (void)arg;
    sbuf res;
    sb_init(&res);
    char err[256] = "";
    if (!strcmp(req_engine, "jev")) {
        float now = p_yes('n', err, sizeof err);
        if (now < 0) {
            sb_printf(&res, "? 0 no answer %s", err);
        } else {
            float harm = now >= 0.85f ? p_yes('h', err, sizeof err) : 0;
            char d = harm >= 0.8f ? 'D' : now >= 0.85f ? 'C' : 'B';
            sb_printf(&res, "%c %.3f now %.2f harm %.2f", d, d == 'B' ? 1 - now : now, now, harm > 0 ? harm : 0);
        }
    } else {
        char *body = request('l');
        struct http_response r;
        json *j = ask(body, &r);
        free(body);
        json *choice = j ? json_path(j, "choices") : NULL;
        choice = choice ? choice->child : NULL;
        const char *text = choice ? json_str(json_path(choice, "message.content"), "") : "";
        char letter = text[0] >= 'A' && text[0] <= 'D' ? text[0] : '?';
        const char *why = strchr(text, ':');
        sb_printf(&res, "%c %.3f %.100s", letter, letter != '?' ? 1.0 : 0.0, why ? why + 2 : r.status ? "" : r.error);
        if (j)
            json_free(j);
        http_response_free(&r);
    }
    *result = res.p;
}

bool reflex_judge(const char *situation, const char *event) {
    if (judging || out.ready || !reflex_ready())
        return false;
    snprintf(req_situation, sizeof req_situation, "%s", situation);
    snprintf(req_event, sizeof req_event, "%s", event);
    snprintf(req_engine, sizeof req_engine, "%s", reflex_engine());
    judging = job_start(judge_child, NULL);
    return judging != NULL;
}

bool reflex_result(enum nerve_verdict *v, float *confidence, char *why, size_t n) {
    if (!out.ready)
        return false;
    out.ready = false;
    *v = out.v, *confidence = out.conf;
    snprintf(why, n, "%s", out.why);
    return true;
}

void reflex_tick(void) {
    download_tick();
    server_tick();
    char *res = NULL;
    if (judging && job_poll(judging, &res)) {
        job_free(judging);
        judging = NULL;
        char letter = '?';
        float conf = 0;
        int off = 0;
        if (res && sscanf(res, "%c %f %n", &letter, &conf, &off) >= 2) {
            out.v = letter == 'A' ? V_IGNORE : letter == 'B' ? V_REMEMBER : letter == 'C' ? V_WAKE : letter == 'D' ? V_URGENT : V_PENDING;
            out.conf = conf;
            snprintf(out.why, sizeof out.why, "%s", res + off);
        } else {
            out.v = V_PENDING, out.conf = 0;
            snprintf(out.why, sizeof out.why, "no answer");
        }
        out.ready = true;
        free(res);
    }
}
