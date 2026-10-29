#define _GNU_SOURCE
#include "stylist.h"
#include "app.h"
#include "fm/fm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* The second half of making a design: the model looks at what it made.
 *
 * Every provider gets the lint: what the layout engine measured as broken,
 * one line each, and a round to repair it. A model that can see also gets
 * pictures (the desktop at rest, and its menu open over a window) and is
 * asked to be its own art director, for one round (each round writes the
 * whole document again, minutes with a big model). A revision is
 * kept only if it still parses into a usable desktop and measures no worse
 * than what came before, so a round can only make things better. */

#define STATUS_PATH "/tmp/os-design-status"

void stylist_status(const char *fmt, ...) {
    char tmp[64], line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    snprintf(tmp, sizeof tmp, STATUS_PATH ".%d", (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fputs(line, f);
    fclose(f);
    rename(tmp, STATUS_PATH); /* whole lines only, never half of one */
}

bool stylist_read_status(char *out, size_t n) {
    FILE *f = fopen(STATUS_PATH, "r");
    if (!f)
        return false;
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    fclose(f);
    return got > 0;
}

void stylist_clear_status(void) { unlink(STATUS_PATH); }

#define SHOW_PATH "/tmp/os-design-show"

void stylist_show(const char *text, size_t len, bool whole) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, SHOW_PATH ".%d", (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "%s %llu\n", whole ? "WHOLE" : "SOFAR", (unsigned long long)now_ms());
    fwrite(text, 1, len, f);
    fclose(f);
    rename(tmp, SHOW_PATH);
}

char *stylist_read_show(uint64_t *seen, bool *whole) {
    FILE *f = fopen(SHOW_PATH, "r");
    if (!f)
        return NULL;
    char kind[8];
    unsigned long long at = 0;
    if (fscanf(f, "%7s %llu", kind, &at) != 2 || at == *seen) {
        fclose(f);
        return NULL;
    }
    fgetc(f); /* the newline */
    sbuf b;
    sb_init(&b);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sb_addn(&b, buf, n);
    fclose(f);
    *seen = at;
    *whole = !strcmp(kind, "WHOLE");
    return b.p ? b.p : strdup("");
}

void stylist_clear_show(void) { unlink(SHOW_PATH); }

static void log_lint(const char *what, const stylist_look *l) {
    LOG("stylist: %s: %d findings, score %d\n", what, l->lint.count, l->lint.score);
    /* one log line per finding, so the serial log reads well */
    char *copy = strdup(l->lint.text);
    for (char *save, *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        LOG("stylist-lint %s\n", line);
    free(copy);
}

/* Models that think slowly on this machine get the lint in the log only:
 * a second whole document would keep the person waiting for minutes. */
static bool worth_a_round(const struct ai_config *cfg) { return strcmp(cfg->provider, "builtin") != 0; }

static const char *critique_words(bool pictures, const stylist_look *l) {
    static char buf[10000];
    if (pictures)
        snprintf(buf, sizeof buf,
                 "Before they see it, look at what you made, as a demanding art director would. Picture 1 is %s. "
                 "Picture 2 is %s. Both are the real rendering at %dx%d, scaled down.\n\n"
                 "The layout engine measured these problems:\n%s\n"
                 "Fix every one of them, then everything you can see: hierarchy and type scale, alignment and "
                 "spacing rhythm, things that look cramped, cut off, crowded, accidental or empty, colour harmony, "
                 "contrast, and whether it feels made for this person. Keep what already works; don't start over "
                 "unless it is truly wrong.\n\n"
                 "Answer with the complete corrected <facet> document and nothing else: no prose, no code fences.",
                 l->shows[0], l->shows[1], A.W > 0 ? A.W : 1600, A.H > 0 ? A.H : 900,
                 l->lint.text[0] ? l->lint.text : "(nothing measurable; judge it by eye)\n");
    else
        snprintf(buf, sizeof buf,
                 "Before they see it: your document was laid out at %dx%d and drawn, and the layout engine measured "
                 "these problems:\n%s\n"
                 "Fix each one with the smallest change that does it, keep everything else as it is, and answer "
                 "with the complete corrected <facet> document and nothing else: no prose, no code fences.",
                 A.W > 0 ? A.W : 1600, A.H > 0 ? A.H : 900, l->lint.text);
    if (l->problems[0]) {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof buf - n, "\nAlso: %s.", l->problems);
    }
    return buf;
}

/* A round's reply as it comes, as progress (the draft stays on show). */
struct round_progress {
    uint64_t start, at;
    const char *what;
};

static void round_sofar(const char *text, size_t len, void *ctx) {
    struct round_progress *p = ctx;
    uint64_t t = now_ms();
    if (t - p->at < 500)
        return;
    p->at = t;
    size_t lines = 0;
    for (size_t i = 0; i < len; i++)
        lines += text[i] == '\n';
    stylist_status("%s: %zu lines, %llu s", p->what, lines, (unsigned long long)((t - p->start) / 1000));
}

char *stylist_refine(const struct ai_config *cfg, const char *system, const char *request, const char *reply) {
    char *best = stylist_expand_base(reply);
    if (!best)
        return NULL;
    bool see = ai_sees_images(cfg);
    stylist_status("Looking over what I made");
    uint64_t t0 = now_ms();
    stylist_look look;
    stylist_look_at(best, see, &look);
    LOG("stylist: looked in %d ms%s\n", (int)(now_ms() - t0), see ? ", with pictures" : "");
    if (!look.ok) { /* the screen's own checks report this and ask again */
        LOG("stylist: the design doesn't hold together: %s\n", look.problems);
        return best;
    }
    log_lint("first draft", &look);
    int rounds = !worth_a_round(cfg) ? 0 : see || look.lint.score > 0 ? 1 : 0;
    int best_score = look.lint.score;
    for (int r = 0; r < rounds; r++) {
        if (see)
            stylist_status(r == 0 ? "Looking at it the way you will" : "A second look at the details");
        else
            stylist_status("Fixing %d small thing%s I noticed", look.lint.count, look.lint.count == 1 ? "" : "s");
        bool pics = see && look.npictures == 2;
        struct ai_image imgs[2];
        for (int i = 0; i < 2; i++)
            imgs[i] = (struct ai_image){"image/jpeg", look.jpeg_b64[i]};
        struct ai_message m[3] = {
            {"user", request, NULL, 0},
            {"assistant", best, NULL, 0},
            {"user", critique_words(pics, &look), pics ? imgs : NULL, pics ? 2 : 0},
        };
        char *rep = NULL, err[512];
        struct round_progress rp = {now_ms(), 0, see ? "Polishing it" : "Fixing the small things"};
        int rc = ai_chat_stream(cfg, system, m, 3, 32000, round_sofar, &rp, &rep, err, sizeof err);
        stylist_look_free(&look);
        if (rc != 0) {
            LOG("stylist: round %d didn't come back: %s\n", r + 1, err);
            break;
        }
        bool cut = ai_last_cut && !strcasestr(rep, "</facet>");
        char *next = cut ? NULL : stylist_expand_base(rep);
        free(rep);
        if (!next) {
            LOG("stylist: round %d gave no whole document; keeping the last one\n", r + 1);
            break;
        }
        stylist_status("Checking the changes");
        stylist_look nl;
        stylist_look_at(next, see && r + 1 < rounds, &nl);
        if (!nl.ok) {
            LOG("stylist: round %d doesn't parse (%s); keeping the last one\n", r + 1, nl.problems);
            free(next);
            break;
        }
        char what[32];
        snprintf(what, sizeof what, "round %d", r + 1);
        log_lint(what, &nl);
        if (nl.lint.score > best_score) {
            LOG("stylist: round %d measures worse (%d > %d); keeping the last one\n", r + 1, nl.lint.score,
                best_score);
            stylist_look_free(&nl);
            free(next);
            break;
        }
        free(best);
        best = next;
        best_score = nl.lint.score;
        stylist_show(best, strlen(best), true); /* the preview shows the better one */
        look = nl;
        if (!see && best_score == 0)
            break;
        if (see && r + 1 < rounds && nl.npictures < 2)
            break; /* nothing to show next time */
    }
    stylist_look_free(&look);
    stylist_status("Almost there");
    return best;
}

/* ---- the companion's small edits ---- */

static bool line_known(const char *text, const char *line) {
    /* the same element with the same kind of trouble: compare the start
     * (the selector and the first words), not the numbers */
    char head[48];
    snprintf(head, sizeof head, "%.40s", line);
    char *digit = strpbrk(head, "0123456789");
    if (digit)
        *digit = 0;
    return strstr(text, head) != NULL;
}

char *stylist_check_edit(const char *before, struct fm_doc *after) {
    char *now = fm_serialize(after);
    if (!now)
        return NULL;
    uint64_t t0 = now_ms();
    stylist_look a, b;
    stylist_look_at(before, false, &a);
    stylist_look_at(now, false, &b);
    free(now);
    LOG("stylist: measured the edit in %d ms\n", (int)(now_ms() - t0));
    if (!b.ok || !b.lint.text[0])
        return NULL;
    sbuf o;
    sb_init(&o);
    char *copy = strdup(b.lint.text);
    for (char *save, *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (!a.ok || !line_known(a.lint.text, line)) {
            sb_add(&o, line);
            sb_add(&o, "\n");
        }
    free(copy);
    if (o.p)
        LOG("stylist: the edit left:\n%s", o.p);
    return o.p;
}
