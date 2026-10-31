/* facet: Facet 2 on the build machine (docs/facet2.md).
 *   facet check FILE...   every load-time error in each file, at once; exit 1 if any
 * Build: tests/facet.sh */
#include "../userland/shell/fx/fx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    s[n] = 0;
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "check")) {
        fprintf(stderr, "usage: facet check FILE...\n");
        return 2;
    }
    int bad = 0;
    for (int i = 2; i < argc; i++) {
        char *src = slurp(argv[i]);
        if (!src) {
            fprintf(stderr, "%s: can't read it\n", argv[i]);
            bad++;
            continue;
        }
        fx_errors errs = {0};
        errs.file = argv[i];
        fx_app *app = fx_parse(src, &errs);
        fx_check(app, &errs);
        if (errs.n) {
            fx_print_errors(&errs, stdout);
            bad++;
        } else {
            printf("%s: ok (%s: %d data, %d state, %d derived, %d fns, %d components)\n", argv[i], app->name, app->ndata,
                   app->nstate, app->nderived, app->nfns, app->ncomps);
        }
        free(errs.e);
        fx_free(app);
        free(src);
    }
    return bad ? 1 : 0;
}
