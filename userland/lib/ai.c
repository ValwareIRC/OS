#include "ai.h"
#include "http.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int ai_last_cut; /* the last reply stopped at the length limit */

#define CONFIG_PATH "/etc/os/mind.json"

/* In a virtual machine with NAT networking (QEMU, VirtualBox), the host
 * computer answers at 10.0.2.2, so a model server running on the same PC is
 * found without typing anything. On a real network the person types their
 * server's address. */
#define LOCAL_HOST "http://10.0.2.2"

const struct ai_provider ai_providers[] = {
    {"anthropic", "Anthropic Claude", "Big labs", "Claude Opus, Sonnet, Haiku and Fable", AI_ANTHROPIC,
     "https://api.anthropic.com", "claude-opus-5-5", true, false, true, NULL},
    {"openai", "OpenAI", "Big labs", "GPT models", AI_OPENAI, "https://api.openai.com/v1", "", true, false, true,
     NULL},
    {"gemini", "Google Gemini", "Big labs", "Gemini models from Google AI Studio", AI_GEMINI,
     "https://generativelanguage.googleapis.com", "", true, false, true, NULL},
    {"xai", "xAI Grok", "Big labs", "Grok models", AI_OPENAI, "https://api.x.ai/v1", "", true, false, true, NULL},
    {"mistral", "Mistral", "Big labs", "Mistral and Magistral models", AI_OPENAI, "https://api.mistral.ai/v1", "",
     true, false, true, NULL},
    {"deepseek", "DeepSeek", "Big labs", "DeepSeek chat and reasoning models", AI_OPENAI,
     "https://api.deepseek.com/v1", "", true, false, true, NULL},
    {"glm", "GLM (Z.ai)", "Big labs", "GLM models from Zhipu; edit the address for a coding plan", AI_OPENAI,
     "https://api.z.ai/api/paas/v4", "", true, true, true, NULL},

    {"openrouter", "OpenRouter", "Aggregators", "Hundreds of models behind one key", AI_OPENAI,
     "https://openrouter.ai/api/v1", "", true, false, true, NULL},
    {"groq", "Groq", "Aggregators", "Very fast open models", AI_OPENAI, "https://api.groq.com/openai/v1", "", true,
     false, true, NULL},
    {"together", "Together AI", "Aggregators", "Open models in the cloud", AI_OPENAI, "https://api.together.xyz/v1",
     "", true, false, true, NULL},
    {"fireworks", "Fireworks AI", "Aggregators", "Open models in the cloud", AI_OPENAI,
     "https://api.fireworks.ai/inference/v1", "", true, false, true, NULL},
    {"cerebras", "Cerebras", "Aggregators", "Very fast open models", AI_OPENAI, "https://api.cerebras.ai/v1", "",
     true, false, true, NULL},

    /* scripts/claude-bridge.mjs on the PC running this VM: Claude through
     * that PC's own Claude Code sign-in, with no key in here. */
    {"claude-pc", "Claude on this PC", "On your network", "Claude through this PC's Claude Code sign-in. No key.",
     AI_OPENAI, LOCAL_HOST ":4570/v1", "sonnet", false, true, true, NULL},
    {"ollama", "Ollama", "On your network", "A model running on another computer", AI_OPENAI,
     LOCAL_HOST ":11434/v1", "", false, true, true, NULL},
    {"lmstudio", "LM Studio", "On your network", "A model running on another computer", AI_OPENAI,
     LOCAL_HOST ":1234/v1", "", false, true, true, NULL},
    {"llamacpp", "llama.cpp server", "On your network", "A model running on another computer", AI_OPENAI,
     LOCAL_HOST ":8080/v1", "", false, true, true, NULL},
    {"vllm", "vLLM", "On your network", "A model running on another computer", AI_OPENAI, LOCAL_HOST ":8000/v1", "",
     false, true, true, NULL},

    {"azure", "Azure OpenAI", "Enterprise", "Your organisation's Azure deployment", AI_AZURE,
     "https://YOUR-RESOURCE.openai.azure.com", "", true, true, true, NULL},
    {"bedrock", "AWS Bedrock", "Enterprise", "Claude and others through AWS", AI_BEDROCK, "", "", true, false, false,
     "Needs AWS request signing, which arrives in a later build."},
    {"vertex", "Google Vertex AI", "Enterprise", "Gemini and Claude through Google Cloud", AI_VERTEX, "", "", true,
     false, false, "Needs Google Cloud sign-in, which arrives in a later build."},

    {"custom", "Any compatible service", "Other", "Any address that speaks the OpenAI format", AI_OPENAI,
     "https://", "", true, true, true, NULL},
    /* The OS's own mind: llama.cpp's server with a small open model, started
     * by the shell on this computer (shell/mind.c). Nothing leaves it. */
    {"builtin", "On this computer", "Other", "A model running right here: private, works offline, no account.", AI_OPENAI,
     "http://127.0.0.1:8080/v1", "os", false, false, true, NULL},
};
const int ai_provider_count = sizeof ai_providers / sizeof ai_providers[0];

const struct ai_provider *ai_find_provider(const char *id) {
    for (int i = 0; i < ai_provider_count; i++)
        if (!strcmp(ai_providers[i].id, id))
            return &ai_providers[i];
    return NULL;
}

static void trim_slash(char *s) {
    size_t n = strlen(s);
    while (n && s[n - 1] == '/')
        s[--n] = 0;
}

/* Turns a provider's error reply into something a person can act on. */
static void explain(const struct http_response *r, const char *who, char *err, int errlen) {
    if (r->status == 0) {
        snprintf(err, (size_t)errlen, "%s", r->error);
        return;
    }
    const char *msg = NULL;
    json *j = r->body ? json_parse(r->body, r->body_len) : NULL;
    if (j) {
        msg = json_str(json_path(j, "error.message"), NULL);
        if (!msg)
            msg = json_str(json_get(j, "error"), NULL);
        if (!msg)
            msg = json_str(json_get(j, "message"), NULL);
    }
    const char *hint = r->status == 401 || r->status == 403 ? "The key was not accepted."
                       : r->status == 404                   ? "That address or model was not found."
                       : r->status == 429                   ? "The service says it's busy or the account is out of credit."
                       : r->status >= 500                   ? "The service is having trouble right now."
                                                            : "The request was refused.";
    snprintf(err, (size_t)errlen, "%s %s%s%s (HTTP %d)", who, hint, msg ? " It said: " : "", msg ? msg : "",
             r->status);
    json_free(j);
}

static int post_json(const char *url, const char *const *headers, sbuf *body, int timeout_s, json **out,
                     const char *who, char *err, int errlen) {
    struct http_response r;
    http_request("POST", url, headers, body->p, body->len, timeout_s, &r);
    if (r.status != 200) {
        explain(&r, who, err, errlen);
        http_response_free(&r);
        return -1;
    }
    *out = json_parse(r.body, r.body_len);
    http_response_free(&r);
    if (!*out) {
        snprintf(err, (size_t)errlen, "%s sent a reply this computer could not read.", who);
        return -1;
    }
    return 0;
}

static bool is_claude_5x(const char *model) {
    return !strcmp(model, "claude-opus-5-5") || !strcmp(model, "claude-fable-5-1") ||
           !strcmp(model, "claude-opus-5") || !strcmp(model, "claude-sonnet-5-5");
}

static int chat_anthropic(const struct ai_config *cfg, const char *system, const struct ai_message *msgs, int n,
                          int max_tokens, char **reply, char *err, int errlen) {
    char url[320], key[300];
    snprintf(url, sizeof url, "%s/v1/messages", cfg->base_url);
    snprintf(key, sizeof key, "x-api-key: %s", cfg->api_key);
    bool fallback = is_claude_5x(cfg->model);
    const char *headers[] = {"Content-Type: application/json", key, "anthropic-version: 2023-06-01",
                             fallback ? "anthropic-beta: server-side-fallback-2026-07-01" : NULL, NULL};
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{\"model\":");
    sb_json_str(&b, cfg->model);
    /* Newer models think before they answer, and thinking counts against
     * max_tokens: a small cap can leave no room for the reply itself. The
     * cap only limits; you pay for what is used. */
    if (max_tokens < 8000)
        max_tokens = 8000;
    sb_printf(&b, ",\"max_tokens\":%d", max_tokens);
    /* Conversation should feel immediate: keep thinking light. */
    if (strncmp(cfg->model, "claude-haiku-4", 14) && strncmp(cfg->model, "claude-3", 8))
        sb_add(&b, ",\"output_config\":{\"effort\":\"low\"}");
    if (fallback)
        sb_add(&b, ",\"fallbacks\":\"default\"");
    if (system && *system) {
        sb_add(&b, ",\"system\":");
        sb_json_str(&b, system);
    }
    sb_add(&b, ",\"messages\":[");
    for (int i = 0; i < n; i++) {
        sb_add(&b, i ? ",{\"role\":" : "{\"role\":");
        sb_json_str(&b, msgs[i].role);
        sb_add(&b, ",\"content\":");
        if (msgs[i].nimages > 0) {
            /* pictures first, then the words about them */
            sb_add(&b, "[");
            for (int k = 0; k < msgs[i].nimages; k++) {
                sb_add(&b, "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":");
                sb_json_str(&b, msgs[i].images[k].mime);
                sb_add(&b, ",\"data\":");
                sb_json_str(&b, msgs[i].images[k].b64);
                sb_add(&b, "}},");
            }
            sb_add(&b, "{\"type\":\"text\",\"text\":");
            sb_json_str(&b, msgs[i].text);
            sb_add(&b, "}]");
        } else {
            sb_json_str(&b, msgs[i].text);
        }
        sb_add(&b, "}");
    }
    sb_add(&b, "]}");
    json *j;
    int r = post_json(url, headers, &b, 900, &j, "Anthropic:", err, errlen);
    sb_free(&b);
    if (r < 0)
        return -1;
    ai_last_cut = !strcmp(json_str(json_get(j, "stop_reason"), ""), "max_tokens");
    if (!strcmp(json_str(json_get(j, "stop_reason"), ""), "refusal")) {
        snprintf(err, (size_t)errlen, "Claude declined to answer that.");
        json_free(j);
        return -1;
    }
    sbuf text;
    sb_init(&text);
    char kinds[120] = "";
    for (json *c = json_get(j, "content") ? json_get(j, "content")->child : NULL; c; c = c->next) {
        const char *t = json_str(json_get(c, "type"), "");
        if (!strcmp(t, "text"))
            sb_add(&text, json_str(json_get(c, "text"), ""));
        if (strlen(kinds) + strlen(t) + 2 < sizeof kinds)
            snprintf(kinds + strlen(kinds), sizeof kinds - strlen(kinds), "%s%s", kinds[0] ? "," : "", t);
    }
    if (!text.len) {
        /* Say why instead of going quiet. */
        const char *stop = json_str(json_get(j, "stop_reason"), "?");
        snprintf(err, (size_t)errlen, "Claude sent no text (stop: %s; blocks: %s).", stop, kinds[0] ? kinds : "none");
        fprintf(stderr, "ai: empty reply from %s, stop_reason=%s, blocks=%s\n", cfg->model, stop, kinds);
        json_free(j);
        sb_free(&text);
        return -1;
    }
    json_free(j);
    *reply = text.p ? text.p : strdup("");
    return 0;
}

/* A streamed OpenAI-style reply: server-sent events, one "data: {json}"
 * line per piece, ending with "data: [DONE]". */
struct sse {
    sbuf line, text;
    ai_stream_fn fn;
    void *ctx;
    bool cut;
    char error[200]; /* a "data: {\"error\":...}" event */
};

static void sse_line(struct sse *s, char *line) {
    if (strncmp(line, "data:", 5))
        return;
    char *d = line + 5;
    while (*d == ' ')
        d++;
    if (!strncmp(d, "[DONE]", 6))
        return;
    json *j = json_parse(d, strlen(d));
    if (!j)
        return;
    const char *problem = json_str(json_path(j, "error.message"), NULL);
    if (problem && !s->error[0])
        snprintf(s->error, sizeof s->error, "%s", problem);
    const char *piece = json_str(json_path(j, "choices.0.delta.content"), NULL);
    if (!strcmp(json_str(json_path(j, "choices.0.finish_reason"), ""), "length"))
        s->cut = true;
    if (piece && *piece) {
        sb_add(&s->text, piece);
        if (s->fn)
            s->fn(s->text.p, s->text.len, s->ctx);
    }
    json_free(j);
}

static void sse_body(const char *data, size_t len, void *ctx) {
    struct sse *s = ctx;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            if (s->line.len && s->line.p[s->line.len - 1] == '\r')
                s->line.p[--s->line.len] = 0;
            if (s->line.p)
                sse_line(s, s->line.p);
            s->line.len = 0;
            if (s->line.p)
                s->line.p[0] = 0;
        } else {
            sb_addn(&s->line, data + i, 1);
        }
    }
}

static int chat_openai(const struct ai_config *cfg, const struct ai_provider *p, const char *system,
                       const struct ai_message *msgs, int n, int max_tokens, char **reply, char *err, int errlen,
                       ai_stream_fn stream, void *stream_ctx) {
    char url[400], auth[300];
    if (p->wire == AI_AZURE) {
        snprintf(url, sizeof url, "%s/openai/deployments/%s/chat/completions?api-version=2024-10-21", cfg->base_url,
                 cfg->model);
        snprintf(auth, sizeof auth, "api-key: %s", cfg->api_key);
    } else {
        snprintf(url, sizeof url, "%s/chat/completions", cfg->base_url);
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", cfg->api_key[0] ? cfg->api_key : "none");
    }
    const char *headers[] = {"Content-Type: application/json", auth, NULL};
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{\"model\":");
    sb_json_str(&b, cfg->model);
    /* OpenAI's newer models only accept max_completion_tokens. */
    sb_printf(&b, ",\"%s\":%d", !strcmp(p->id, "openai") ? "max_completion_tokens" : "max_tokens", max_tokens);
    sb_add(&b, ",\"messages\":[");
    bool first = true;
    if (system && *system) {
        sb_add(&b, "{\"role\":\"system\",\"content\":");
        sb_json_str(&b, system);
        sb_add(&b, "}");
        first = false;
    }
    for (int i = 0; i < n; i++) {
        sb_add(&b, first ? "{\"role\":" : ",{\"role\":");
        first = false;
        sb_json_str(&b, msgs[i].role);
        sb_add(&b, ",\"content\":");
        if (msgs[i].nimages > 0) {
            /* OpenAI's parts: the pictures as data: URIs */
            sb_add(&b, "[");
            for (int k = 0; k < msgs[i].nimages; k++) {
                sb_printf(&b, "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:%s;base64,", msgs[i].images[k].mime);
                sb_add(&b, msgs[i].images[k].b64); /* base64 needs no escaping */
                sb_add(&b, "\"}},");
            }
            sb_add(&b, "{\"type\":\"text\",\"text\":");
            sb_json_str(&b, msgs[i].text);
            sb_add(&b, "}]");
        } else {
            sb_json_str(&b, msgs[i].text);
        }
        sb_add(&b, "}");
    }
    sb_add(&b, stream ? "],\"stream\":true}" : "]}");
    char who[64];
    snprintf(who, sizeof who, "%s:", p->name);
    if (stream) {
        struct sse s = {.fn = stream, .ctx = stream_ctx};
        sb_init(&s.line);
        sb_init(&s.text);
        struct http_response hr;
        http_request_stream("POST", url, headers, b.p, b.len, 900, sse_body, &s, &hr);
        sb_free(&b);
        if (s.line.len)
            sse_line(&s, s.line.p); /* a last line without its newline */
        sb_free(&s.line);
        int status = hr.status;
        if (status != 200) {
            explain(&hr, who, err, errlen);
            http_response_free(&hr);
            sb_free(&s.text);
            return -1;
        }
        /* A server that doesn't stream answers the ordinary way: take that. */
        const char *bd = hr.body ? hr.body + strspn(hr.body, " \t\r\n") : NULL;
        if (!s.text.len && !s.error[0] && bd && *bd == '{') {
            json *j = json_parse(bd, strlen(bd));
            const char *text = j ? json_str(json_path(j, "choices.0.message.content"), NULL) : NULL;
            if (text && *text) {
                sb_add(&s.text, text);
                s.cut = !strcmp(json_str(json_path(j, "choices.0.finish_reason"), ""), "length");
                stream(s.text.p, s.text.len, stream_ctx);
            }
            json_free(j);
        }
        http_response_free(&hr);
        ai_last_cut = s.cut;
        if (!s.text.len || s.error[0]) { /* a reply broken off part way is no reply */
            snprintf(err, (size_t)errlen, "%s", s.error[0] ? s.error : "");
            if (!s.error[0])
                snprintf(err, (size_t)errlen, "%s answered without any text.", p->name);
            sb_free(&s.text);
            return -1;
        }
        const char *end_think = strstr(s.text.p, "</think>");
        *reply = strdup(end_think ? end_think + 8 : s.text.p);
        sb_free(&s.text);
        return 0;
    }
    json *j;
    int r = post_json(url, headers, &b, 900, &j, who, err, errlen);
    sb_free(&b);
    if (r < 0)
        return -1;
    ai_last_cut = !strcmp(json_str(json_path(j, "choices.0.finish_reason"), ""), "length");
    const char *text = json_str(json_path(j, "choices.0.message.content"), NULL);
    if (!text) {
        snprintf(err, (size_t)errlen, "%s answered without any text.", p->name);
        json_free(j);
        return -1;
    }
    /* Reasoning models served locally sometimes inline their thoughts. */
    const char *end_think = strstr(text, "</think>");
    *reply = strdup(end_think ? end_think + 8 : text);
    json_free(j);
    return 0;
}

static int chat_gemini(const struct ai_config *cfg, const char *system, const struct ai_message *msgs, int n,
                       int max_tokens, char **reply, char *err, int errlen) {
    char url[400], key[300];
    snprintf(url, sizeof url, "%s/v1beta/models/%s:generateContent", cfg->base_url, cfg->model);
    snprintf(key, sizeof key, "x-goog-api-key: %s", cfg->api_key);
    const char *headers[] = {"Content-Type: application/json", key, NULL};
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{");
    if (system && *system) {
        sb_add(&b, "\"systemInstruction\":{\"parts\":[{\"text\":");
        sb_json_str(&b, system);
        sb_add(&b, "}]},");
    }
    sb_add(&b, "\"contents\":[");
    for (int i = 0; i < n; i++) {
        sb_add(&b, i ? ",{\"role\":" : "{\"role\":");
        sb_json_str(&b, !strcmp(msgs[i].role, "assistant") ? "model" : "user");
        sb_add(&b, ",\"parts\":[");
        for (int k = 0; k < msgs[i].nimages; k++) {
            sb_add(&b, "{\"inline_data\":{\"mime_type\":");
            sb_json_str(&b, msgs[i].images[k].mime);
            sb_add(&b, ",\"data\":");
            sb_json_str(&b, msgs[i].images[k].b64);
            sb_add(&b, "}},");
        }
        sb_add(&b, "{\"text\":");
        sb_json_str(&b, msgs[i].text);
        sb_add(&b, "}]}");
    }
    sb_printf(&b, "],\"generationConfig\":{\"maxOutputTokens\":%d}}", max_tokens);
    json *j;
    int r = post_json(url, headers, &b, 900, &j, "Gemini:", err, errlen);
    sb_free(&b);
    if (r < 0)
        return -1;
    sbuf text;
    sb_init(&text);
    ai_last_cut = !strcmp(json_str(json_path(j, "candidates.0.finishReason"), ""), "MAX_TOKENS");
    json *parts = json_path(j, "candidates.0.content.parts");
    for (json *c = parts ? parts->child : NULL; c; c = c->next)
        if (!json_bool(json_get(c, "thought"), false))
            sb_add(&text, json_str(json_get(c, "text"), ""));
    json_free(j);
    if (!text.p) {
        snprintf(err, (size_t)errlen, "Gemini answered without any text.");
        return -1;
    }
    *reply = text.p;
    return 0;
}

int ai_chat(const struct ai_config *cfg, const char *system, const struct ai_message *msgs, int count,
            int max_tokens, char **reply, char *err, int errlen) {
    return ai_chat_stream(cfg, system, msgs, count, max_tokens, NULL, NULL, reply, err, errlen);
}

int ai_chat_stream(const struct ai_config *cfg, const char *system, const struct ai_message *msgs, int count,
                   int max_tokens, ai_stream_fn stream, void *stream_ctx, char **reply, char *err, int errlen) {
    const struct ai_provider *p = ai_find_provider(cfg->provider);
    ai_last_cut = 0;
    if (!p || !p->available) {
        snprintf(err, (size_t)errlen, "%s", p && p->unavailable_reason ? p->unavailable_reason : "No mind is set up.");
        return -1;
    }
    struct ai_config c = *cfg;
    trim_slash(c.base_url);
    /* Reasoning models spend part of max_tokens thinking; leave room to answer. */
    if (max_tokens < 8000)
        max_tokens = 8000;
    int r;
    switch (p->wire) {
    case AI_ANTHROPIC: r = chat_anthropic(&c, system, msgs, count, max_tokens, reply, err, errlen); break;
    case AI_GEMINI: r = chat_gemini(&c, system, msgs, count, max_tokens, reply, err, errlen); break;
    case AI_OPENAI:
    case AI_AZURE:
        r = chat_openai(&c, p, system, msgs, count, max_tokens, reply, err, errlen, stream, stream_ctx);
        /* The model on this computer may still be loading (a big one from the
         * disk takes minutes): wait for it rather than call it a failure. */
        for (int waited = 0; r != 0 && !strcmp(p->id, "builtin") && waited < 900; waited += 2) {
            bool loading = strstr(err, "Loading model") || strstr(err, "HTTP 503");
            bool not_yet = strstr(err, "Connection refused") && waited < 60;
            if (!loading && !not_yet)
                break;
            sleep(2);
            r = chat_openai(&c, p, system, msgs, count, max_tokens, reply, err, errlen, stream, stream_ctx);
        }
        return r;
    default:
        snprintf(err, (size_t)errlen, "%s", p->unavailable_reason ? p->unavailable_reason : "Not available yet.");
        return -1;
    }
    /* Wires that don't stream here yet: the whole reply, at once. */
    if (r == 0 && stream && *reply)
        stream(*reply, strlen(*reply), stream_ctx);
    return r;
}

bool ai_sees_images(const struct ai_config *cfg) {
    const struct ai_provider *p = ai_find_provider(cfg->provider);
    if (!p || !p->available)
        return false;
    const char *m = cfg->model;
    if (!strcmp(p->id, "claude-pc")) /* the bridge passes image_url parts on to Claude */
        return true;
    if (p->wire == AI_ANTHROPIC)
        return strncmp(m, "claude-2", 8) && strncmp(m, "claude-instant", 14);
    if (p->wire == AI_GEMINI)
        return !strncmp(m, "gemini", 6);
    if (p->wire != AI_OPENAI || !strcmp(p->id, "builtin"))
        return false; /* Azure deployments have names of their own: can't tell */
    /* Model names that are known to take pictures, wherever they are served. */
    static const char *const seeing[] = {"gpt-4o", "gpt-4.1", "gpt-4-turbo", "gpt-5", "chatgpt-4o", "claude",
                                         "gemini", "grok-4", "grok-2-vision", "vision", "-vl", "vl-", "llava",
                                         "pixtral", "gemma3", "gemma-3", "glm-4v", "glm-4.1v", "glm-4.5v",
                                         "minicpm-v", "moondream", "llama-4", "mistral-medium", "mistral-small-3",
                                         NULL};
    for (int i = 0; seeing[i]; i++)
        if (strstr(m, seeing[i]))
            return true;
    /* OpenAI's reasoning models see, except o3-mini */
    return (!strncmp(m, "o3", 2) || !strncmp(m, "o4-", 3)) && strncmp(m, "o3-mini", 7);
}

int ai_list_models(const struct ai_config *cfg, char ***ids, char *err, int errlen) {
    const struct ai_provider *p = ai_find_provider(cfg->provider);
    if (!p)
        return -1;
    struct ai_config c = *cfg;
    trim_slash(c.base_url);
    char url[400], h1[300];
    const char *headers[4] = {NULL};
    const char *list_key = "data", *id_key = "id";
    if (p->wire == AI_ANTHROPIC) {
        snprintf(url, sizeof url, "%s/v1/models?limit=100", c.base_url);
        snprintf(h1, sizeof h1, "x-api-key: %s", c.api_key);
        headers[0] = h1;
        headers[1] = "anthropic-version: 2023-06-01";
    } else if (p->wire == AI_GEMINI) {
        snprintf(url, sizeof url, "%s/v1beta/models?pageSize=200", c.base_url);
        snprintf(h1, sizeof h1, "x-goog-api-key: %s", c.api_key);
        headers[0] = h1;
        list_key = "models";
        id_key = "name";
    } else if (p->wire == AI_AZURE) {
        /* Azure deployments are named by the customer; nothing to list. */
        *ids = NULL;
        return 0;
    } else {
        snprintf(url, sizeof url, "%s/models", c.base_url);
        snprintf(h1, sizeof h1, "Authorization: Bearer %s", c.api_key[0] ? c.api_key : "none");
        headers[0] = h1;
    }
    struct http_response r;
    http_request("GET", url, headers, NULL, 0, 30, &r);
    if (r.status != 200) {
        explain(&r, p->name, err, errlen);
        http_response_free(&r);
        return -1;
    }
    json *j = json_parse(r.body, r.body_len);
    http_response_free(&r);
    json *list = json_get(j, list_key);
    int n = json_len(list), k = 0;
    char **out = calloc((size_t)n + 1, sizeof *out);
    for (json *m = list ? list->child : NULL; m && out; m = m->next) {
        const char *id = json_str(json_get(m, id_key), NULL);
        if (!id)
            continue;
        if (p->wire == AI_GEMINI) {
            if (!strncmp(id, "models/", 7))
                id += 7;
            if (strncmp(id, "gemini", 6))
                continue; /* embeddings, imagen, ... */
        }
        out[k++] = strdup(id);
    }
    json_free(j);
    *ids = out;
    return k;
}

void ai_free_models(char **ids, int n) {
    for (int i = 0; i < n; i++)
        free(ids[i]);
    free(ids);
}

const char *ai_pick_model(const struct ai_provider *p, char **ids, int n) {
    if (p->default_model && *p->default_model)
        for (int i = 0; i < n; i++)
            if (!strcmp(ids[i], p->default_model))
                return ids[i];
    /* Otherwise prefer a general chat model over embeddings/audio/etc. */
    static const char *const avoid[] = {"embed", "whisper", "tts", "audio", "image", "moderation", "vision", "guard",
                                        "rerank", "transcribe", "realtime", "search", "dall", NULL};
    for (int i = 0; i < n; i++) {
        bool bad = false;
        for (int a = 0; avoid[a]; a++)
            if (strstr(ids[i], avoid[a]))
                bad = true;
        if (!bad)
            return ids[i];
    }
    return n ? ids[0] : (p->default_model && *p->default_model ? p->default_model : "");
}

int ai_config_load(struct ai_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return -1;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    json *j = json_parse(buf, n);
    if (!j)
        return -1;
    snprintf(cfg->provider, sizeof cfg->provider, "%s", json_str(json_get(j, "provider"), ""));
    snprintf(cfg->base_url, sizeof cfg->base_url, "%s", json_str(json_get(j, "base_url"), ""));
    snprintf(cfg->api_key, sizeof cfg->api_key, "%s", json_str(json_get(j, "api_key"), ""));
    snprintf(cfg->model, sizeof cfg->model, "%s", json_str(json_get(j, "model"), ""));
    json_free(j);
    return cfg->provider[0] ? 0 : -1;
}

int ai_config_save(const struct ai_config *cfg) {
    mkdir("/etc/os", 0700);
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{\"provider\":");
    sb_json_str(&b, cfg->provider);
    sb_add(&b, ",\"base_url\":");
    sb_json_str(&b, cfg->base_url);
    sb_add(&b, ",\"api_key\":");
    sb_json_str(&b, cfg->api_key);
    sb_add(&b, ",\"model\":");
    sb_json_str(&b, cfg->model);
    sb_add(&b, "}\n");
    FILE *f = fopen(CONFIG_PATH, "w");
    int r = -1;
    if (f) {
        r = fwrite(b.p, 1, b.len, f) == b.len ? 0 : -1;
        fclose(f);
        chmod(CONFIG_PATH, 0600);
    }
    sb_free(&b);
    return r;
}
