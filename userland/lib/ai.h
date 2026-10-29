#pragma once
#include <stdbool.h>
#include <stddef.h>

/* One interface over every AI provider the OS offers at first boot. Three
 * wire formats cover them: Anthropic Messages, OpenAI-compatible chat
 * completions (OpenAI, xAI, Mistral, DeepSeek, OpenRouter, Groq, Together,
 * Fireworks, Cerebras, Ollama, LM Studio, llama.cpp, vLLM, custom), and
 * Google Gemini. Azure OpenAI uses the OpenAI format with its own URL and
 * header. AWS Bedrock and Google Vertex need request signing / OAuth and
 * are listed but not yet usable. */

enum ai_wire { AI_ANTHROPIC, AI_OPENAI, AI_GEMINI, AI_AZURE, AI_BEDROCK, AI_VERTEX, AI_BUILTIN };

struct ai_provider {
    const char *id;
    const char *name;
    const char *group;     /* section on the setup screen */
    const char *blurb;     /* one line under the name */
    enum ai_wire wire;
    const char *base_url;  /* editable default */
    const char *default_model;
    bool needs_key;
    bool url_editable;     /* local servers, Azure, custom */
    bool available;        /* false: shown, but explains why it can't be chosen yet */
    const char *unavailable_reason;
};

extern const struct ai_provider ai_providers[];
extern const int ai_provider_count;
const struct ai_provider *ai_find_provider(const char *id);

struct ai_config {
    char provider[32];
    char base_url[256];
    char api_key[256];
    char model[128];
};

/* A picture sent along with a message (base64, without a data: prefix). */
struct ai_image {
    const char *mime; /* "image/jpeg" or "image/png" */
    const char *b64;
};

struct ai_message {
    const char *role; /* "user" or "assistant" */
    const char *text;
    const struct ai_image *images; /* optional: shown to the model before the text */
    int nimages;
};

/* Can this provider and model look at pictures? Conservative: false unless
 * the model is known to see. */
bool ai_sees_images(const struct ai_config *cfg);

/* Sends a conversation; on success *reply is a malloc'd string. Returns 0,
 * or -1 with a plain-language explanation in err. */
/* Set by ai_chat when the reply was cut off at max_tokens. */
extern int ai_last_cut;

int ai_chat(const struct ai_config *cfg, const char *system, const struct ai_message *msgs, int count,
            int max_tokens, char **reply, char *err, int errlen);

/* As ai_chat, and `stream` is called with the reply so far each time more
 * of it arrives (OpenAI-style services stream; the others call it once,
 * with the whole reply). */
typedef void (*ai_stream_fn)(const char *so_far, size_t len, void *ctx);
int ai_chat_stream(const struct ai_config *cfg, const char *system, const struct ai_message *msgs, int count,
                   int max_tokens, ai_stream_fn stream, void *stream_ctx, char **reply, char *err, int errlen);

/* Lists models the key can use (malloc'd array of malloc'd ids). Returns
 * the count, or -1 with err set. */
int ai_list_models(const struct ai_config *cfg, char ***ids, char *err, int errlen);
void ai_free_models(char **ids, int n);
/* Picks a sensible default from a provider's model list. */
const char *ai_pick_model(const struct ai_provider *p, char **ids, int n);

/* Persisted configuration (/etc/os/mind.json). */
int ai_config_load(struct ai_config *cfg);
int ai_config_save(const struct ai_config *cfg);
