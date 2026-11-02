#pragma once
/* A second opinion on a Facet app, from small decision models on this computer (never the companion's own
 * model or service), and what the companion is allowed to know of it.
 *
 * The scores that count come from the app normalised (fx_normalize): no comments, every name the app made up
 * replaced by a neutral one (v1, f2...), every text in it replaced by an opaque token (S3). Whatever the file
 * says, in comments, strings, names or its description, can't reach them. The file as written is also reviewed
 * with its comments and without them: if those two differ by more than a threshold, its comments are steering
 * the reviewer, likely prompt injection, flagged, and the warning raised.
 *
 * Small models are unreliable judges of what code does (measured: see fx_review), so they are kept in their
 * place: each question comes with worked examples; a model can't say an app sends data when its code can't reach
 * the network, or destroys files when it can't write any; their answers go to the companion as numbers, and only
 * comments steering them raise a warning. What an app can do is decided by the analysis and the rules.
 *
 * The result is a scorecard of numbers and fixed values only. The companion is told the scorecard and the
 * manifest as counts, kinds and line numbers (fx_review_note): no name, path, address, string or comment from
 * the file. It may speak up about it; the person decides. */
#include "caps.h"

/* The questions, each answered as the probability of "yes" by every model asked (decision-model style). */
enum {
    FXQ_SENDS,      /* sends what they type, or what is in their files, to a server */
    FXQ_DESTROYS,   /* deletes or overwrites files beyond what using it obviously does */
    FXQ_HIDDEN,     /* does things its visible interface doesn't show */
    FXQ_OBSCURE,    /* written to hide what it does */
    FXQ_COUNT
};
extern const char *const fx_review_questions[FXQ_COUNT];

typedef struct fx_scorecard {
    bool done;
    int models;                 /* how many answered */
    float score[FXQ_COUNT];     /* normalised code: 0..1, averaged over the models and phrasings */
    float raw[FXQ_COUNT];       /* the file as written, comments and all */
    float bare[FXQ_COUNT];      /* the same, its comments taken out */
    float drift;                /* the largest difference between those two: what its comments did to the reviewer */
    bool injection;             /* drift past the threshold: something in the text steers the reviewer */
    int level;                  /* 0 fine, 1 worth a note, 2 warn */
} fx_scorecard;

#define FX_INJECTION_DRIFT 0.25f

/* the app's code as the reviewers see it (malloc'd) */
char *fx_normalize(const fx_app *app);

/* The request for one question about some code (a chat completion asking for one token, with worked
 * examples first), as JSON (malloc'd); phrasing 0 or 1. */
char *fx_review_request(const char *code, int question, int phrasing);
/* Asks one model: the request as above (code is also given alone, for stand-ins in tests); the probability of
 * yes, or -1 if it couldn't answer. */
typedef float (*fx_review_ask)(void *ctx, int model, const char *request, const char *code);
/* the file without its comments (malloc'd) */
char *fx_strip_comments(const char *src);

/* Reviews: every question, each model in 0..models-1, two phrasings each; normalised, raw and without comments. */
void fx_review(const fx_app *app, const char *raw_source, int models, fx_review_ask ask, void *ctx, fx_scorecard *out);

/* What the companion may be told: the manifest as counts, kinds, required/optional and line numbers, the rule
 * warning's level, and the scorecard's numbers. Nothing from the file itself. (malloc'd) */
char *fx_review_note(const fx_manifest *m, int mismatch_level, const fx_scorecard *sc);
