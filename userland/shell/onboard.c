#define _GNU_SOURCE
#include "app.h"
#include "caption.h"
#include "voice.h"
#include "mind.h"
#include "models.h"
#include "fm/fm.h"
#include "stylist.h"
#include "preview.h"
#include "../lib/json.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* First boot. These screens exist before any AI is connected, so they are
 * the only ones the OS draws in its own plain style: dark, warm, quiet. From
 * the conversation onward, everything the person sees is shaped by the AI. */

#define PRE_BG RGBA(14, 11, 12, 255)
#define PRE_BG2 RGBA(38, 20, 18, 255)
#define PRE_TEXT RGBA(242, 233, 226, 255)
#define PRE_MUTED RGBA(168, 150, 142, 255)
#define PRE_ACCENT RGBA(232, 131, 107, 255)

/* Glass card for the first-boot screens. */
static void pre_card(rect r, float radius, bool hover) {
    if (hover)
        glow_round(A.frame, r, radius, 22, color_alpha(PRE_ACCENT, 0.35f));
    blur_fast(A.frame, r, radius, 10);
    fill_round_vgradient(A.frame, r, radius, RGBA(255, 238, 226, hover ? 40 : 24), RGBA(255, 238, 226, hover ? 22 : 8));
    stroke_round_vgradient(A.frame, r, radius, 1, RGBA(255, 255, 255, hover ? 110 : 60), RGBA(255, 255, 255, 8));
}

static void pre_background(void) {
    ui_backdrop(PRE_BG, RGBA(120, 46, 40, 255), RGBA(232, 131, 107, 255), RGBA(44, 18, 26, 255));
}

/* ---------------------------------------------------------------------
 * Conversations: requests run in a child process so the screen stays live.
 * ------------------------------------------------------------------- */

struct chat_job {
    struct ai_config cfg;
    const char *system;
    struct conversation *c;
    int max_tokens;
};

static void chat_child(void *arg, char **result) {
    struct chat_job *j = arg;
    struct ai_message m[MAX_MSGS] = {0}; /* no pictures in conversation */
    int n = 0;
    for (int i = 0; i < j->c->n; i++) {
        const char *role = !strcmp(j->c->msgs[i].role, "assistant") ? "assistant" : "user";
        /* Providers want alternating turns: merge consecutive user lines. */
        if (n && !strcmp(m[n - 1].role, role) && !strcmp(role, "user")) {
            size_t a = strlen(m[n - 1].text), b = strlen(j->c->msgs[i].text);
            char *joined = malloc(a + b + 3);
            snprintf(joined, a + b + 3, "%s\n\n%s", m[n - 1].text, j->c->msgs[i].text);
            m[n - 1].text = joined;
            continue;
        }
        m[n].role = role;
        m[n].text = j->c->msgs[i].text;
        n++;
    }
    char *reply = NULL, err[512];
    sbuf out;
    sb_init(&out);
    if (ai_chat(&j->cfg, j->system, m, n, j->max_tokens, &reply, err, sizeof err) == 0) {
        sb_add(&out, "OK\n");
        sb_add(&out, reply);
    } else {
        sb_add(&out, "ERR\n");
        sb_add(&out, err);
    }
    *result = out.p;
}

/* Models often answer in Markdown; the OS shows plain, typeset text. Removes
 * emphasis markers and heading hashes but keeps [[tags]] intact. */
void strip_markdown(char *s) {
    char *w = s;
    bool line_start = true;
    for (char *r = s; *r; r++) {
        if (*r == '*' || *r == '`' || (*r == '_' && r[1] == '_')) {
            if (*r == '_')
                r++;
            continue;
        }
        if (line_start && *r == '#') {
            while (*r == '#')
                r++;
            while (*r == ' ')
                r++;
            r--;
            line_start = false;
            continue;
        }
        line_start = *r == '\n';
        *w++ = *r;
    }
    *w = 0;
}

void convo_add(struct conversation *c, const char *role, const char *text) {
    if (c->n == MAX_MSGS) {
        free(c->msgs[1].text); /* keep the first (setup) message */
        memmove(&c->msgs[1], &c->msgs[2], sizeof c->msgs[0] * (MAX_MSGS - 2));
        c->n--;
    }
    snprintf(c->msgs[c->n].role, sizeof c->msgs[c->n].role, "%s", role);
    c->msgs[c->n].text = strdup(text);
    c->n++;
}

/* The companion conversation is kept on the disk (newlines escaped), so a
 * new day can start from the last one. */
void convo_save(const struct conversation *c, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    int from = c->n > 60 ? c->n - 60 : 0;
    for (int i = from; i < c->n; i++) {
        if (!strcmp(c->msgs[i].role, "hidden"))
            continue;
        fprintf(f, "%s\t", c->msgs[i].role);
        for (const char *p = c->msgs[i].text; *p; p++) {
            if (*p == '\n')
                fputs("\\n", f);
            else if (*p == '\\')
                fputs("\\\\", f);
            else
                fputc(*p, f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

int convo_load(struct conversation *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    static char line[16384];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        char *o = tab + 1;
        for (char *p = tab + 1; *p; p++) {
            if (*p == '\\' && p[1] == 'n')
                *o++ = '\n', p++;
            else if (*p == '\\' && p[1] == '\\')
                *o++ = '\\', p++;
            else
                *o++ = *p;
        }
        *o = 0;
        convo_add(c, line, tab + 1);
        n++;
    }
    fclose(f);
    return n;
}

void convo_clear(struct conversation *c) {
    for (int i = 0; i < c->n; i++)
        free(c->msgs[i].text);
    c->n = 0;
    job_free(c->pending);
    c->pending = NULL;
    c->error[0] = 0;
}

static struct chat_job job_args; /* read by the child after fork */
static char system_copy[32768]; /* the companion's prompt, with its senses, runs past 8 KB */

void convo_send(struct conversation *c, const char *system, int max_tokens) {
    snprintf(system_copy, sizeof system_copy, "%s", system);
    job_args = (struct chat_job){A.cfg, system_copy, c, max_tokens};
    c->error[0] = 0;
    c->pending = job_start(chat_child, &job_args);
    c->pending_since = A.now;
}

bool convo_poll(struct conversation *c) {
    char *res = NULL;
    if (c->pending && !job_poll(c->pending, &res)) {
        /* the mind on this computer: what it's doing, on the console, while it thinks */
        static uint64_t said;
        if (!strcmp(A.cfg.provider, "builtin") && A.now - c->pending_since > 30000 && A.now - said > 30000) {
            said = A.now;
            FILE *lf = fopen("/tmp/os-mind.log", "r");
            char tail[400] = "";
            if (lf) {
                fseek(lf, -(long)sizeof tail + 1, SEEK_END);
                size_t k = fread(tail, 1, sizeof tail - 1, lf);
                tail[k] = 0;
                fclose(lf);
            }
            char *t = tail + strlen(tail);
            while (t > tail && t[-1] == '\n')
                *--t = 0;
            char *line = strrchr(tail, '\n');
            LOG("mind: thinking for %llus: %s\n", (unsigned long long)(A.now - c->pending_since) / 1000,
                line ? line + 1 : tail);
        }
        return false;
    }
    if (!c->pending)
        return false;
    job_free(c->pending);
    c->pending = NULL;
    if (res && !strncmp(res, "OK\n", 3)) {
        strip_markdown(res + 3);
        convo_add(c, "assistant", res + 3);
        c->errors = 0;
        LOG("ai %.400s\n", res + 3);
    } else {
        LOG("ai-error %.300s\n", res ? res : "(none)");
        const char *why = res && !strncmp(res, "ERR\n", 4) ? res + 4 : "";
        snprintf(c->error, sizeof c->error, "%s", *why ? why : "No answer came back; the connection may have dropped.");
        c->errors++;
    }
    free(res);
    return true;
}

/* ---------------------------------------------------------------------
 * Choose a mind
 * ------------------------------------------------------------------- */

static const char *groups[] = {"Big labs", "Aggregators", "On your network", "Enterprise", "Other"};

void choose_draw(void) {
    pre_background();
    font *title = font_get("Fraunces", 46), *sub = font_get("Inter", 18), *name = font_get("Inter", 18),
         *small = font_get("Inter", 14), *head = font_get("Inter", 13);
    int x0 = A.W / 2 - 640;
    draw_text(A.frame, title, x0, 70, "Choose a mind", PRE_TEXT);
    draw_text(A.frame, sub, x0, 136,
              "This computer thinks with an AI of your choosing. It will get to know you, then design this computer for you.",
              PRE_MUTED);

    int colw = 1280 / 5, y_top = 200;
    for (int g = 0; g < 5; g++) {
        int cx = x0 + g * colw, y = y_top;
        draw_text(A.frame, head, cx, y, groups[g], PRE_ACCENT);
        y += 30;
        for (int i = 0; i < ai_provider_count; i++) {
            const struct ai_provider *p = &ai_providers[i];
            if (strcmp(p->group, groups[g]) || !p->available || (!strcmp(p->id, "builtin") && !mind_builtin_present()))
                continue;
            rect card = {cx, y, colw - 16, 80};
            bool hover = p->available && ui_hover(card);
            pre_card(card, 16, hover);
            draw_text(A.frame, name, card.x + 16, card.y + 14, p->name, p->available ? PRE_TEXT : PRE_MUTED);
            draw_text_wrapped(A.frame, small, (rect){card.x + 16, card.y + 40, card.w - 28, 36},
                              p->blurb, PRE_MUTED, 1.15f);
            if (!p->needs_key && p->available)
                draw_text(A.frame, small, card.x + card.w - 16 - text_width(small, "no key"), card.y + 16, "no key",
                          PRE_ACCENT);
            if (hover && A.in.clicked && !strcmp(p->id, "builtin")) {
                set_screen(SCR_MODELS); /* which model: the built-in one, or one to download */
            } else if (hover && A.in.clicked) {
                A.provider = p;
                memset(&A.cfg, 0, sizeof A.cfg);
                snprintf(A.cfg.provider, sizeof A.cfg.provider, "%s", p->id);
                snprintf(A.url.buf, sizeof A.url.buf, "%s", p->base_url);
                A.url.len = (int)strlen(A.url.buf);
                A.key.len = 0;
                A.key.buf[0] = 0;
                A.key.secret = true;
                A.focus = p->url_editable ? 0 : 1;
                A.connect_status[0] = 0;
                if (A.models)
                    ai_free_models(A.models, A.nmodels);
                A.models = NULL;
                A.nmodels = 0;
                set_screen(SCR_CONNECT);
            }
            y += 88;
        }
    }
    draw_text(A.frame, small, x0, A.H - 60,
              "Keys stay on this computer. It talks to the service you pick and to nothing else.", PRE_MUTED);
}

/* ---------------------------------------------------------------------
 * A mind on this computer: the model that came with it, or a better one
 * downloaded now (models.c says which this computer can run)
 * ------------------------------------------------------------------- */

static void use_local_model(int i) {
    (void)i;
    const struct ai_provider *p = ai_find_provider("builtin");
    if (!p)
        return;
    A.provider = p;
    mind_builtin_start(); /* loads while they read the next screen */
    memset(&A.cfg, 0, sizeof A.cfg);
    snprintf(A.cfg.provider, sizeof A.cfg.provider, "%s", p->id);
    snprintf(A.url.buf, sizeof A.url.buf, "%s", p->base_url);
    A.url.len = (int)strlen(A.url.buf);
    A.key.len = 0;
    A.key.buf[0] = 0;
    A.key.secret = true;
    A.focus = 1;
    A.connect_status[0] = 0;
    if (A.models)
        ai_free_models(A.models, A.nmodels);
    A.models = NULL;
    A.nmodels = 0;
    set_screen(SCR_CONNECT);
}

void models_draw(void) {
    pre_background();
    font *title = font_get("Fraunces", 46), *sub = font_get("Inter", 18), *name = font_get("Inter", 18),
         *small = font_get("Inter", 14);
    int x0 = A.W / 2 - 520, w = 1040;
    draw_text(A.frame, title, x0, 70, "A mind on this computer", PRE_TEXT);
    draw_text_wrapped(A.frame, sub, (rect){x0, 136, w, 60},
                      "Private, and it works without the internet. One small model comes with the system; better "
                      "ones can be downloaded now, or any time later in Settings.",
                      PRE_MUTED, 1.3f);
    palette p = {PRE_TEXT, PRE_MUTED, PRE_ACCENT, RGBA(255, 255, 255, 255), RGBA(255, 255, 255, 40), PRE_BG, 1};
    rect area = {x0, 0, w, 0};
    models_list(area, 214, &p, name, small, small, use_local_model);
    rect back = {x0, A.H - 70, 110, 40};
    if (ui_button(back, "Back", small, RGBA(255, 255, 255, 24), PRE_TEXT, 20, true))
        set_screen(SCR_CHOOSE);
}

/* ---------------------------------------------------------------------
 * Connect: address, key, then pick a model
 * ------------------------------------------------------------------- */

static struct ai_config connect_cfg;

static void connect_child(void *arg, char **result) {
    (void)arg;
    char **ids, err[512];
    int n = ai_list_models(&connect_cfg, &ids, err, sizeof err);
    /* The mind on this computer takes a while to wake (the model loads; a big
     * one from the disk takes minutes): wait while its server is alive. */
    for (int tries = 0; n < 0 && !strcmp(connect_cfg.provider, "builtin") && tries < 1200 &&
                        (tries < 10 || mind_builtin_alive());
         tries++) {
        sleep(1);
        n = ai_list_models(&connect_cfg, &ids, err, sizeof err);
    }
    sbuf out;
    sb_init(&out);
    if (n < 0) {
        sb_add(&out, "ERR\n");
        sb_add(&out, err);
        if (!strcmp(connect_cfg.provider, "builtin")) { /* why, in the server's words */
            FILE *lf = fopen("/tmp/os-mind.log", "r");
            char tail[4096] = "";
            size_t k = lf ? fread(tail, 1, sizeof tail - 1, lf) : 0;
            tail[k] = 0;
            if (lf)
                fclose(lf);
            LOG("mind: the model on this computer didn't answer: %s\n--- llama-server ---\n%s\n", err, tail);
        }
    } else {
        sb_add(&out, "OK\n");
        for (int i = 0; i < n; i++) {
            sb_add(&out, ids[i]);
            sb_add(&out, "\n");
        }
    }
    *result = out.p;
}

static void begin_onboarding(const char *model) {
    snprintf(A.cfg.model, sizeof A.cfg.model, "%s", model);
    ai_config_save(&A.cfg);
    onboard_start();
}

void connect_draw(void) {
    pre_background();
    const struct ai_provider *p = A.provider;
    font *title = font_get("Fraunces", 40), *label = font_get("Inter", 15), *body = font_get("Inter", 18),
         *small = font_get("Inter", 14);
    rect card = {A.W / 2 - 380, 120, 760, 640};
    pre_card(card, 28, false);
    int x = card.x + 48, y = card.y + 44;
    draw_text(A.frame, title, x, y, p->name, PRE_TEXT);
    y += 64;
    draw_text(A.frame, body, x, y, p->blurb, PRE_MUTED);
    y += 48;

    rect url_r = {0}, key_r = {0};
    if (p->url_editable) {
        draw_text(A.frame, label, x, y, "Address", PRE_MUTED);
        url_r = (rect){x, y + 26, card.w - 96, 50};
        ui_textfield(url_r, &A.url, A.focus == 0, "https://...", body, RGBA(255, 255, 255, 18), PRE_TEXT, PRE_MUTED, 12);
        y += 96;
    }
    if (p->needs_key) {
        draw_text(A.frame, label, x, y, "Key", PRE_MUTED);
        key_r = (rect){x, y + 26, card.w - 96, 50};
        ui_textfield(key_r, &A.key, A.focus == 1, "Paste or type your API key", body, RGBA(255, 255, 255, 18), PRE_TEXT,
                     PRE_MUTED, 12);
        if (clipboard_text()[0])
            draw_text(A.frame, label, key_r.x + key_r.w - 210, y, "Ctrl+V pastes from your PC", PRE_MUTED);
        y += 96;
    }
    if (A.in.clicked) {
        if (p->url_editable && ui_hover(url_r))
            A.focus = 0;
        if (p->needs_key && ui_hover(key_r))
            A.focus = 1;
    }
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_TAB && p->url_editable && p->needs_key)
            A.focus ^= 1;

    bool enter = false;
    if (!A.connect_job && !A.models)
        enter = ui_type(A.focus == 0 ? &A.url : &A.key);

    if (!A.models) {
        rect btn = {x, y + 6, 180, 50};
        bool go = ui_button(btn, A.connect_job ? "Connecting..." : "Connect", body, PRE_ACCENT, RGBA(30, 16, 14, 255), 12,
                            !A.connect_job);
        rect back = {x + 196, y + 6, 120, 50};
        if (ui_button(back, "Back", body, RGBA(255, 255, 255, 20), PRE_TEXT, 12, !A.connect_job))
            set_screen(SCR_CHOOSE);
        /* the mind on this computer has nothing to type or pick: it connects
         * by itself (while the model loads) and carries straight on */
        bool here = !strcmp(p->id, "builtin");
        if (here && !A.connect_status[0])
            go = true;
        if ((go || enter) && !A.connect_job) {
            snprintf(A.cfg.base_url, sizeof A.cfg.base_url, "%s", A.url.buf);
            snprintf(A.cfg.api_key, sizeof A.cfg.api_key, "%s", A.key.buf);
            connect_cfg = A.cfg;
            if (here)
                snprintf(A.connect_status, sizeof A.connect_status, "Waking up %s (it takes a moment to load)...",
                         models_selected()->name);
            else
                snprintf(A.connect_status, sizeof A.connect_status, "Reaching %s...", p->name);
            A.connect_job = job_start(connect_child, NULL);
        }
        draw_text_wrapped(A.frame, small, (rect){x, y + 76, card.w - 96, 80}, A.connect_status, PRE_MUTED, 1.2f);
    }

    char *res;
    if (A.connect_job && job_poll(A.connect_job, &res)) {
        job_free(A.connect_job);
        A.connect_job = NULL;
        if (res && !strncmp(res, "OK\n", 3)) {
            int n = 0;
            for (char *s = res + 3; *s; s++)
                n += *s == '\n';
            A.models = calloc((size_t)n + 1, sizeof(char *));
            A.nmodels = 0;
            for (char *line = strtok(res + 3, "\n"); line; line = strtok(NULL, "\n"))
                A.models[A.nmodels++] = strdup(line);
            const char *pick = ai_pick_model(p, A.models, A.nmodels);
            A.model_sel = 0;
            for (int i = 0; i < A.nmodels; i++)
                if (!strcmp(A.models[i], pick))
                    A.model_sel = i;
            A.model_scroll = A.model_sel > 4 ? A.model_sel - 2 : 0;
            if (A.nmodels == 0 && p->wire == AI_AZURE)
                snprintf(A.connect_status, sizeof A.connect_status, "Connected. Type your deployment name as the model.");
            else
                snprintf(A.connect_status, sizeof A.connect_status, "Connected.");
            if (A.nmodels == 0 || !strcmp(p->id, "builtin")) { /* nothing to choose between */
                ai_free_models(A.models, A.nmodels);
                A.models = NULL;
                A.nmodels = 0;
                begin_onboarding(p->default_model);
            }
        } else {
            snprintf(A.connect_status, sizeof A.connect_status, "%s", res && !strncmp(res, "ERR\n", 4) ? res + 4 : "That didn't work.");
        }
        free(res);
    }

    if (A.models) {
        draw_text(A.frame, label, x, y, "Which model should be your mind? The suggested one is selected.", PRE_MUTED);
        y += 30;
        int rows = 6;
        if (A.in.wheel)
            A.model_scroll -= A.in.wheel;
        if (A.model_scroll > A.nmodels - rows)
            A.model_scroll = A.nmodels - rows;
        if (A.model_scroll < 0)
            A.model_scroll = 0;
        for (int i = 0; i < rows && A.model_scroll + i < A.nmodels; i++) {
            int m = A.model_scroll + i;
            rect row = {x, y + i * 44, card.w - 96, 40};
            bool sel = m == A.model_sel;
            fill_round(A.frame, row, 10, sel ? color_alpha(PRE_ACCENT, 0.3f) : ui_hover(row) ? RGBA(255, 255, 255, 16) : 0);
            draw_text(A.frame, body, row.x + 14, row.y + 9, A.models[m], sel ? PRE_TEXT : PRE_MUTED);
            if (ui_hover(row) && A.in.clicked)
                A.model_sel = m;
        }
        for (int i = 0; i < A.in.nkeys; i++) {
            if (A.in.keys[i] == K_DOWN && A.model_sel < A.nmodels - 1)
                A.model_sel++;
            if (A.in.keys[i] == K_UP && A.model_sel > 0)
                A.model_sel--;
        }
        y += rows * 44 + 16;
        rect btn = {x, y, 220, 52};
        bool go = ui_button(btn, "Begin", body, PRE_ACCENT, RGBA(30, 16, 14, 255), 12, true);
        for (int i = 0; i < A.in.nkeys; i++)
            go |= A.in.keys[i] == K_ENTER;
        if (go)
            begin_onboarding(A.models[A.model_sel]);
    }
}

#include "onboard_talk.inc"

/* ---------------------------------------------------------------------
 * The AI designs the desktop
 * ------------------------------------------------------------------- */

static char design_system[40000];
static char design_request[60000];
static struct ai_config design_cfg;

/* Runs in the design job when the conversation left few portrait notes
 * (some models don't tag as they go): a careful reading of the whole
 * conversation, written as portrait lines. Saved, so the parent picks them
 * up, and appended to the design request. */
static void extract_portrait(sbuf *req) {
    if (A.prof.nfacts >= 5 || A.onboard.n < 4)
        return;
    sbuf t;
    sb_init(&t);
    for (int i = 0; i < A.onboard.n; i++)
        if (strcmp(A.onboard.msgs[i].role, "hidden"))
            sb_printf(&t, "%s: %s\n", !strcmp(A.onboard.msgs[i].role, "user") ? "Them" : "AI", A.onboard.msgs[i].text);
    struct ai_message m = {"user", t.p ? t.p : "", NULL, 0};
    const char *sys =
        "You read the first conversation between a person and the AI companion of their new computer. Write what "
        "it tells you about them, as 8 to 16 lines of the form `field: value`, nothing else. Fields: access, "
        "language, rhythm, work, apps, social, strain, protect, home_place, taste, voice, presence, boundaries, "
        "redesign, words. Each value is a short implication for how their computer should look and behave (for "
        "example `protect: evenings free of work`, `taste: warm tiles and morning light`), or for `words`, a short "
        "phrase they actually used. Only what the conversation supports; no guesses, no quotes of private details.";
    char *reply = NULL, err[256];
    if (ai_chat(&design_cfg, sys, &m, 1, 4000, &reply, err, sizeof err) == 0 && reply) {
        int added = 0;
        for (char *save, *line = strtok_r(reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            while (*line == ' ' || *line == '-' || *line == '*' || *line == '`')
                line++;
            char *colon = strchr(line, ':');
            if (!colon || colon - line > 20)
                continue;
            *colon = 0;
            char *val = colon + 1;
            size_t vl = strlen(val);
            while (vl && (val[vl - 1] == '`' || val[vl - 1] == ' ' || val[vl - 1] == '\r'))
                val[--vl] = 0;
            portrait_learn(&A.prof, line, val);
            added++;
        }
        if (added) {
            portrait_save(&A.prof, STATE_DIR "/portrait.txt");
            char p[8000];
            portrait_text(&A.prof, p, sizeof p);
            sb_printf(req, "\nPORTRAIT, from a careful reading of the conversation:\n%s", p);
        }
        free(reply);
    }
    sb_free(&t);
}

/* The reply as it is written, for the preview: a few times a second. */
struct sofar {
    uint64_t at, start;
    size_t len;
};

static void design_sofar(const char *text, size_t len, void *ctx) {
    struct sofar *s = ctx;
    uint64_t t = now_ms();
    if (t - s->at < 250 && len - s->len < 4000)
        return;
    s->at = t;
    s->len = len;
    stylist_show(text, len, false);
    /* how far along, in words a person can picture: lines of the design written */
    size_t lines = 0;
    for (size_t i = 0; i < len; i++)
        lines += text[i] == '\n';
    stylist_status("%zu lines written, %llu s", lines,
                   (unsigned long long)((t - s->start) / 1000));
}

/* While the mind is still thinking (nothing written yet): the time, so a long
 * think never looks like a hang. Runs beside the request, in the same child. */
static volatile int design_written;
static void *design_clock(void *arg) {
    uint64_t start = *(uint64_t *)arg;
    while (!__atomic_load_n(&design_written, __ATOMIC_ACQUIRE)) {
        sleep(1);
        if (!__atomic_load_n(&design_written, __ATOMIC_ACQUIRE))
            stylist_status("Thinking it through (%llu s)", (unsigned long long)((now_ms() - start) / 1000));
    }
    return NULL;
}

static void design_sofar_first(const char *text, size_t len, void *ctx) {
    __atomic_store_n(&design_written, 1, __ATOMIC_RELEASE);
    design_sofar(text, len, ctx);
}

static void design_child(void *arg, char **result) {
    (void)arg;
    sbuf req;
    sb_init(&req);
    sb_add(&req, design_request);
    extract_portrait(&req);
    struct ai_message m = {"user", req.p, NULL, 0};
    char *reply = NULL, err[512];
    sbuf out;
    sb_init(&out);
    /* A whole desktop is long, and thinking shares the cap: leave plenty. */
    stylist_status("Thinking it through");
    struct sofar sf = {0, now_ms(), 0};
    design_written = 0;
    pthread_t clock;
    bool ticking = pthread_create(&clock, NULL, design_clock, &sf.start) == 0;
    int rc = ai_chat_stream(&design_cfg, design_system, &m, 1, 32000, design_sofar_first, &sf, &reply, err, sizeof err);
    __atomic_store_n(&design_written, 1, __ATOMIC_RELEASE);
    if (ticking)
        pthread_join(clock, NULL);
    if (rc == 0) {
        int cut = ai_last_cut;
        char *whole = stylist_expand_base(reply); /* the whole draft, before the stylist looks */
        if (whole) {
            stylist_show(whole, strlen(whole), true);
            free(whole);
        }
        /* It looks at its own work before they do (stylist.c). */
        char *better = !cut || strcasestr(reply, "</facet>") ? stylist_refine(&design_cfg, design_system, req.p, reply)
                                                              : NULL;
        if (better) {
            free(reply);
            reply = better;
            cut = 0;
        }
        sb_add(&out, cut ? "CUT\n" : "OK\n");
        sb_add(&out, reply);
        /* Kept so a failed design can be looked at afterwards. */
        FILE *f = fopen(STATE_DIR "/design-reply.txt", "w");
        if (f) {
            fputs(reply, f);
            fclose(f);
        }
        free(reply);
    } else {
        sb_add(&out, "ERR\n");
        sb_add(&out, err);
    }
    *result = out.p;
}

void design_begin(const char *extra) {
    if (A.design_job)
        return; /* already designing (a double click, or a tag during a retry) */
    /* Remember the request so a retry asks for the same thing. */
    if (extra && strcmp(extra, "(retry)"))
        snprintf(A.design_instruction, sizeof A.design_instruction, "%s", extra);
    if (extra && !strcmp(extra, "(retry)"))
        extra = A.design_instruction[0] ? A.design_instruction : NULL;
    int nf;
    const char *const *fams = font_families(&nf);
    sbuf s;
    sb_init(&s);
    sb_add(&s, design_prompt_markup());
    sb_add(&s, "FONTS = [");
    for (int i = 0; i < nf; i++)
        sb_printf(&s, "%s\"%s\"", i ? ", " : "", fams[i]);
    sb_add(&s, "]\n");
    if (!strncasecmp(A.cfg.model, "qwen3", 5))
        sb_add(&s, "/no_think\n");
    snprintf(design_system, sizeof design_system, "%s", s.p);
    sb_free(&s);

    sbuf r;
    sb_init(&r);
    /* A change starts from what they are looking at: the preview's
     * proposal if there is one, else the desktop. */
    fm_doc *base = preview_doc() ? preview_doc() : A.ui;
    if (extra && (A.have_design || base)) {
        char *cur = base ? fm_serialize(base) : design_to_facet(&A.d);
        sb_printf(&r, "Here is their current desktop document:\n%s\nThey asked for this change: %s\n"
                      "Return the complete updated document. Their request wins over everything below, the room "
                      "and wallpaper included: if they ask for stars, there are stars. Keep what they didn't ask "
                      "to change unless it no longer fits.\n", cur ? cur : "", extra);
        free(cur);
    }
    sb_printf(&r, "About them:\nName: %s\nYour name: %s\n",
              A.prof.user_name[0] ? A.prof.user_name : "(not given)", A.prof.ai_name[0] ? A.prof.ai_name : "(not chosen)");
    /* Hard limits first: comfort is not a matter of taste. */
    sb_add(&r, "\nACCESS (hard constraints, never trade them for looks):\n");
    float ts = A.prof.text_scale > 0.5f ? A.prof.text_scale : 1;
    sb_printf(&r, "- body text at least %d px, labels at least %d px, controls at least %d px tall\n", (int)(16 * ts),
              (int)(13 * ts), (int)(36 * ts));
    sb_printf(&r, "- text contrast at least %s:1 against what is behind it\n", A.prof.high_contrast ? "7" : "4.5");
    if (A.prof.reduce_motion)
        sb_add(&r, "- no motion: transition 0, a still wallpaper (gradient, paper, dunes, sun or constellation)\n");
    if (A.prof.has_room) {
        sb_printf(&r, "\nTHE ROOM they chose as home: \"%s\", wallpaper kind %s, colours", A.prof.room,
                  wallpaper_name(A.prof.room_kind));
        for (int k = 0; k < 4; k++)
            sb_printf(&r, " #%02x%02x%02x", C_R(A.prof.palette[k]), C_G(A.prof.palette[k]), C_B(A.prof.palette[k]));
        sb_add(&r, extra && (A.have_design || base)
                       ? ". That was the starting point; their request decides how far to move from it.\n"
                       : ". Build the desktop in this room: keep its wallpaper and let its colours lead.\n");
    } else if (A.prof.has_palette) {
        sb_printf(&r, "\nPalette they chose: %s", A.prof.palette_name);
        for (int k = 0; k < 4; k++)
            sb_printf(&r, " #%02x%02x%02x", C_R(A.prof.palette[k]), C_G(A.prof.palette[k]), C_B(A.prof.palette[k]));
        sb_add(&r, "\n");
    }
    if (A.prof.type_font[0])
        sb_printf(&r, "Their words looked right to them in %s: use it for headings or the whole space.\n", A.prof.type_font);
    if (A.prof.tempo >= 0)
        sb_printf(&r, "Tempo: %s (0 still, 1 lively): set transitions and wallpaper intensity to match.\n",
                  A.prof.tempo < 0.33f ? "0.1" : A.prof.tempo < 0.66f ? "0.5" : "0.9");
    char portrait[8000];
    portrait_text(&A.prof, portrait, sizeof portrait);
    sb_printf(&r, "\nPORTRAIT (what you understood about them):\n%s", portrait[0] ? portrait : "(little; read the conversation)\n");
    sb_printf(&r, "Mood: %s\nHow present you should be: %s\nRedesign: %s\n\n",
              A.prof.mood[0] ? A.prof.mood : "(from the portrait)",
              A.prof.autonomy[0] ? A.prof.autonomy : "(from the conversation)",
              A.prof.redesign[0] ? A.prof.redesign : "(from the conversation)");
    sb_add(&r,
           "Before writing, decide privately one concrete design consequence for every portrait line: work and apps "
           "decide what is prominent and how many things are visible at once; rhythm decides the greeting and how "
           "the space feels by hour; strain and protect decide what you leave out; home_place and the room decide "
           "the wallpaper and materials; taste, type and tempo decide structure, type and motion; voice decides "
           "every word. Use their own words for labels and greeting where they fit, and never invent content "
           "(no made-up files, channels, events or people). Inside <meta>, add three to six "
           "<why for=\"#id\" said=\"their words\">one sentence to them about why this is here</why>.\n\n"
           "The conversation:\n");
    for (int i = 0; i < A.onboard.n; i++) {
        if (!strcmp(A.onboard.msgs[i].role, "hidden"))
            continue;
        sb_printf(&r, "%s: %s\n", !strcmp(A.onboard.msgs[i].role, "user") ? "Them" : "You", A.onboard.msgs[i].text);
    }
    if (A.design_problems[0])
        sb_printf(&r, "\nYour previous attempt had problems: %s. Please fix them.\n", A.design_problems);
    snprintf(design_request, sizeof design_request, "%s", r.p);
    snprintf(A.prof.notes, sizeof A.prof.notes, "%s", r.p);
    sb_free(&r);

    design_cfg = A.cfg;
    stylist_clear_status();
    stylist_clear_show();
    A.design_job = job_start(design_child, NULL);
    A.redesigning = extra != NULL;
    /* It is written where they can watch: in the preview window on the
     * desktop, or in the middle of the designing screen the first time. */
    if (A.screen != SCR_DESKTOP)
        set_screen(SCR_DESIGNING);
    preview_drafting();
}

void designing_draw(void) {
    color a = A.prof.has_palette ? A.prof.palette[1] : PRE_ACCENT;
    color b = A.prof.has_palette ? A.prof.palette[2] : RGBA(255, 214, 170, 255);
    if (room_set) {
        a = room_wall.wp[1];
        b = room_wall.wp[2];
    }
    draw_backdrop(a, b); /* still in their room */
    preview_frame();     /* the design as it is written; made theirs when they say */
    if (A.screen != SCR_DESIGNING)
        return;
    companion_tick(); /* once the draft is in, they can talk it over */

    font *big = font_get(A.prof.type_font[0] ? A.prof.type_font : "Fraunces", 30 * tscale()),
         *body = font_get("Inter", 16 * tscale());
    bool ready = !A.design_job;
    draw_text_centered(A.frame, big, (rect){0, 26, A.W, 46}, ready ? "Here it is. Make it yours." : "Making your space",
                       ink(1));
    /* what it is working from: their own portrait, one line at a time */
    char line[300] = "";
    int nf = A.prof.nfacts;
    if (ready) {
        snprintf(line, sizeof line, "Change anything first: tell me, or click something to point at it.");
    } else if (nf) {
        int k = (int)((A.now - A.screen_since) / 2600) % nf;
        snprintf(line, sizeof line, "%s", A.prof.facts[k].val);
        char *semi = strrchr(line, ';');
        if (semi)
            memmove(line, semi + 2, strlen(semi + 2) + 1);
    } else {
        snprintf(line, sizeof line, "From everything you told me");
    }
    draw_text_centered(A.frame, body, (rect){0, 74, A.W, 28}, line, ink(0.7f));

    /* the preview itself: as large as fits, with room to talk below */
    int top = 116, bottom = 28;
    int cw = A.W * 72 / 100, ih = cw * A.H / A.W;
    if (ih + 116 > A.H - top - bottom) {
        ih = A.H - top - bottom - 116;
        cw = ih * A.W / A.H;
    }
    rect card = {(A.W - cw) / 2, top, cw, ih + 116};
    fill_round(A.frame, rect_inset(card, -16), 22, wash(22));
    preview_body(card, true);
}

/* Takes in the design job's answer, if it has one: on the desktop and on the
 * designing screen alike. A finished design goes to the preview; a failed
 * one is tried again, twice, then explained (or, the first time, built from
 * their own choices). */
void design_poll(void) {
    char *res;
    if (!A.design_job || !job_poll(A.design_job, &res))
        return;
    job_free(A.design_job);
    A.design_job = NULL;
    portrait_load(&A.prof, STATE_DIR "/portrait.txt"); /* the designer may have added to it */
    bool ok = false;
    fm_doc *doc = NULL;
    design d;
    if (res) {
        size_t rl = strlen(res);
        LOG("design reply: %zu bytes, %s, ends \"%.80s\"\n", rl,
            !strncmp(res, "CUT\n", 4) ? "cut off at the length limit" : !strncmp(res, "OK\n", 3) ? "complete" : "error",
            rl > 80 ? res + rl - 80 : res);
    }
    if (res && !strncmp(res, "CUT\n", 4) && !strcasestr(res, "</facet>")) {
        snprintf(A.design_problems, sizeof A.design_problems,
                 "your reply was cut off at the length limit before </facet>. Write a shorter document: "
                 "fewer, compact style rules and no comments");
    } else if (res && (!strncmp(res, "OK\n", 3) || !strncmp(res, "CUT\n", 4))) {
        const char *text = strchr(res, '\n') + 1;
        A.design_problems[0] = 0;
        if (strcasestr(res, "<facet") || strcasestr(res, "<desktop")) {
            doc = fm_parse(text);
            if (design_from_facet(&d, doc, &A.prof, A.design_problems, sizeof A.design_problems) &&
                (!A.design_problems[0] || A.design_attempt >= 1)) {
                ok = true;
            } else if (!A.design_problems[0]) {
                snprintf(A.design_problems, sizeof A.design_problems, "the document could not be read");
            }
            /* small slips are reported back too, but don't block */
            for (int i = 0; i < doc->nproblems && i < 6; i++)
                LOG("design markup line %d: %s\n", doc->problems[i].line, doc->problems[i].msg);
            if (!ok) {
                fm_free(doc);
                doc = NULL;
            }
        } else if (design_from_json(&d, text, &A.prof, A.design_problems, sizeof A.design_problems)) {
            ok = true; /* the older JSON vocabulary still works */
        }
    }
    if (!ok && A.design_attempt < 2) {
        A.design_attempt++;
        if (!A.design_problems[0])
            snprintf(A.design_problems, sizeof A.design_problems, "the reply had no <facet> document");
        free(res);
        design_begin(A.redesigning ? "(retry)" : NULL);
        return;
    }
    A.design_attempt = 0;
    free(res);
    if (!ok && A.have_design) {
        /* A redesign that didn't come through: say so, plainly. The
         * companion may already have said "done" before it started. */
        preview_failed(A.design_problems[0] ? A.design_problems : "no answer in time");
        LOG("design failed: %s\n", A.design_problems[0] ? A.design_problems : "no answer");
        return;
    }
    if (!ok) {
        /* No AI design possible: build one from their own choices. */
        design_from_profile(&d, &A.prof);
    }
    if (!doc) {
        char *m = design_to_facet(&d);
        doc = fm_parse(m ? m : "");
        free(m);
    }
    {
        char *m = fm_serialize(doc);
        for (char *p = m; p && *p; p++)
            if (*p == '\n')
                *p = ' ';
        LOG("design-facet %s\n", m ? m : ""); /* one line; lets tests replay real designs */
        free(m);
    }
    LOG("design %s; fixes: %s\n", ok ? "from the AI" : "from their choices (AI unavailable)",
        A.design_problems[0] ? A.design_problems : "none");
    preview_draft(doc, &d); /* theirs to look at, change, and apply */
}
