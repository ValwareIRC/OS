/* os-ai: command-line access to the networking and AI layers, for tests
 * and for poking at things from the development shell.
 *
 *   os-ai get URL                          fetch a URL, print status + start of body
 *   os-ai models PROVIDER URL KEY          list models
 *   os-ai chat PROVIDER URL KEY MODEL TEXT  one-shot conversation
 *   os-ai providers                        list the providers offered at first boot
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/ai.h"
#include "../lib/http.h"

static void fill(struct ai_config *c, const char *provider, const char *url, const char *key) {
    memset(c, 0, sizeof *c);
    const struct ai_provider *p = ai_find_provider(provider);
    snprintf(c->provider, sizeof c->provider, "%s", provider);
    snprintf(c->base_url, sizeof c->base_url, "%s", url && strcmp(url, "-") ? url : (p ? p->base_url : ""));
    snprintf(c->api_key, sizeof c->api_key, "%s", key && strcmp(key, "-") ? key : "");
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "get")) {
        struct http_response r;
        http_request("GET", argv[2], NULL, NULL, 0, 30, &r);
        if (!r.status) {
            printf("error: %s\n", r.error);
            return 1;
        }
        printf("status %d, %zu bytes\n%.300s\n", r.status, r.body_len, r.body ? r.body : "");
        http_response_free(&r);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "providers")) {
        for (int i = 0; i < ai_provider_count; i++)
            printf("%-11s %-26s %-16s %s\n", ai_providers[i].id, ai_providers[i].name, ai_providers[i].group,
                   ai_providers[i].available ? "" : "(later build)");
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "models")) {
        struct ai_config c;
        fill(&c, argv[2], argv[3], argv[4]);
        char **ids, err[512];
        int n = ai_list_models(&c, &ids, err, sizeof err);
        if (n < 0) {
            printf("error: %s\n", err);
            return 1;
        }
        for (int i = 0; i < n; i++)
            printf("%s\n", ids[i]);
        printf("suggested: %s\n", ai_pick_model(ai_find_provider(argv[2]), ids, n));
        ai_free_models(ids, n);
        return 0;
    }
    if (argc >= 7 && !strcmp(argv[1], "chat")) {
        struct ai_config c;
        fill(&c, argv[2], argv[3], argv[4]);
        snprintf(c.model, sizeof c.model, "%s", argv[5]);
        struct ai_message m = {"user", argv[6], NULL, 0};
        char *reply, err[512];
        if (ai_chat(&c, "Answer in one short sentence.", &m, 1, 512, &reply, err, sizeof err) < 0) {
            printf("error: %s\n", err);
            return 1;
        }
        printf("%s\n", reply);
        free(reply);
        return 0;
    }
    fprintf(stderr, "usage: os-ai get URL | providers | models P URL KEY | chat P URL KEY MODEL TEXT\n");
    return 2;
}
