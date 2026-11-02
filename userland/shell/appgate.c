/* The gate before an app runs, and Settings > Security (appgate.h). Everything here is drawn and run by the
 * system: no app or design can draw, click, fill in or read any of it. */
#include "app.h"
#include "appgate.h"
#include "fx/review.h"
#include "../lib/http.h"
#include "../lib/json.h"
#include "fm/fm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define D (A.d)


static font *F(float px) { return fm_host_font(D.font_body, px); }

/* ---- the second opinion, in a child: small models on this computer (the reflex server) ---- */

static float ask_local(void *ctx, int model, const char *req, const char *code) {
    (void)ctx, (void)model, (void)code;
    sbuf b;
    sb_init(&b);
    sb_add(&b, req);
    char auth[120];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", ai_local_key());
    const char *h[] = {"Content-Type: application/json", auth, NULL};
    struct http_response r;
    http_request("POST", "http://127.0.0.1:8092/v1/chat/completions", h, b.p, b.len, 90, &r);
    sb_free(&b);
    float yes = 0, no = 0;
    json *j = r.status == 200 && r.body ? json_parse(r.body, r.body_len) : NULL;
    json *ch = j ? json_path(j, "choices") : NULL;
    ch = ch ? ch->child : NULL;
    json *lp = ch ? json_path(ch, "logprobs.content") : NULL;
    json *top = lp && lp->child ? json_get(lp->child, "top_logprobs") : NULL;
    for (json *t = top ? top->child : NULL; t; t = t->next) {
        const char *tk = json_str(json_get(t, "token"), "");
        while (*tk == ' ')
            tk++;
        json *l = json_get(t, "logprob");
        if (l && !strcasecmp(tk, "yes"))
            yes += expf((float)l->num);
        else if (l && !strcasecmp(tk, "no"))
            no += expf((float)l->num);
    }
    if (j)
        json_free(j);
    http_response_free(&r);
    return yes + no > 0 ? yes / (yes + no) : -1;
}

static struct {
    bool done, allow;
    const char *path;
    const fx_app *app;
    const char *src;
    fx_manifest m;
    fx_gate g;
    fx_grants old;
    bool on[FX_MAX_CAPS], is_new[FX_MAX_CAPS], was_held[FX_MAX_CAPS];
    int mismatch;
    char warn[600];
    bool warnings;
    job *review;
    fx_scorecard sc;
    bool reviewed, told, logged;
    uint64_t opened;
} G;

static void review_child(void *arg, char **result) {
    (void)arg;
    fx_scorecard sc;
    fx_review(G.app, G.src, 1, ask_local, NULL, &sc);
    char buf[300];
    snprintf(buf, sizeof buf, "%d %d %f %f %f %f %f %f %f %f %f %d %d", sc.done, sc.models, sc.score[0], sc.score[1],
             sc.score[2], sc.score[3], sc.raw[0], sc.raw[1], sc.raw[2], sc.raw[3], sc.drift, sc.injection, sc.level);
    *result = strdup(buf);
}

static void review_poll(void) {
    char *res = NULL;
    if (!G.review || !job_poll(G.review, &res))
        return;
    job_free(G.review);
    G.review = NULL;
    G.reviewed = true;
    int done = 0, inj = 0;
    if (res && sscanf(res, "%d %d %f %f %f %f %f %f %f %f %f %d %d", &done, &G.sc.models, &G.sc.score[0], &G.sc.score[1],
                      &G.sc.score[2], &G.sc.score[3], &G.sc.raw[0], &G.sc.raw[1], &G.sc.raw[2], &G.sc.raw[3], &G.sc.drift,
                      &inj, &G.sc.level) == 13) {
        G.sc.done = done, G.sc.injection = inj;
    }
    free(res);
    LOG("appgate: second opinion: %s, level %d%s\n", G.sc.done ? "done" : "unavailable", G.sc.level,
        G.sc.injection ? ", its text tries to steer the review" : "");
}

/* the companion speaks up when something is wrong: told only counts, kinds, lines and numbers */
static void tell_companion(void) {
    if (G.told || !G.warnings)
        return;
    bool wrong = G.mismatch > 0 || (G.sc.done && G.sc.level > 0) || G.m.forbidden;
    if (!wrong)
        return;
    G.told = true;
    char *note = fx_review_note(&G.m, G.mismatch, G.sc.done ? &G.sc : NULL);
    companion_note(note, true);
    free(note);
}

/* ---- the dialog ---- */

static void check_box(rect r, bool on, bool locked, color ink, color accent) {
    float rad = 5;
    if (on)
        fill_round(A.frame, r, rad, locked ? color_alpha(accent, 0.55f) : accent);
    else
        stroke_round(A.frame, r, rad, 1.5f, color_alpha(ink, 0.5f));
    if (on) { /* a tick */
        color c = readable_on(accent);
        draw_line(A.frame, r.x + r.w * 0.25f, r.y + r.h * 0.52f, r.x + r.w * 0.43f, r.y + r.h * 0.7f, 2, c);
        draw_line(A.frame, r.x + r.w * 0.43f, r.y + r.h * 0.7f, r.x + r.w * 0.76f, r.y + r.h * 0.32f, 2, c);
    }
}


static void gate_draw(void) {
    review_poll();
    if (G.reviewed || !G.review)
        tell_companion();
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), 0.55f));
    int w = 600 < A.W - 40 ? 600 : A.W - 40;
    font *title = F(D.body_px + 6), *body = F(D.body_px), *small = F(D.body_px - 2);
    color bg = D.surface | 0xff000000u, ink = D.surface_text, muted = color_alpha(ink, 0.62f), accent = D.accent;
    /* measure, then draw: everything in one panel, scrolled never (it is short by design) */
    char head[200], sub[500];
    snprintf(head, sizeof head, "Allow \xe2\x80\x9c%s\xe2\x80\x9d to run?", G.m.app);
    const char *file = strrchr(G.path, '/') ? strrchr(G.path, '/') + 1 : G.path;
    snprintf(sub, sizeof sub, "%s (%s). It says it is %s %s app. What it can do was worked out from its code, not from "
             "what it says.", file, G.path, strchr("aeiou", G.m.category[0]) ? "an" : "a", G.m.category);
    bool update = G.g.nheld > 0 && G.g.nnew > 0;
    int pad = 28, cw = w - pad * 2, lh = font_line_height(body);
    int h = pad;
    h += font_line_height(title) + 8;
    h += draw_text_wrapped(NULL, small, (rect){0, 0, cw, 400}, sub, muted, 1.3f) + 18;
    int rows = 0;
    for (int i = 0; i < G.m.n; i++)
        rows++;
    char says[FX_MAX_CAPS][400];
    int rh[FX_MAX_CAPS];
    for (int i = 0; i < G.m.n; i++) {
        fx_cap_says(&G.m.c[i], says[i], sizeof says[i]);
        rh[i] = draw_text_wrapped(NULL, body, (rect){0, 0, cw - 40, 400}, says[i], ink, 1.3f) + lh + 14;
        h += rh[i];
    }
    if (update)
        h += 2 * (lh + 10);
    if (!G.m.n)
        h += lh * 2 + 10;
    int wh = 0;
    if (G.warnings && G.mismatch)
        wh = draw_text_wrapped(NULL, body, (rect){0, 0, cw - 32, 400}, G.warn, ink, 1.3f) + 28, h += wh + 14;
    char op[700] = "";
    if (!G.warnings) {
        snprintf(op, sizeof op, "Warnings and second opinions are off (Settings > Security). What it can do is listed "
                 "above all the same.");
    } else if (G.review) {
        snprintf(op, sizeof op, "A second opinion from small models on this computer: reading its code...");
    } else if (G.sc.done) {
        snprintf(op, sizeof op, G.sc.injection
                                    ? "Small models on this computer read its code with and without its comments, and its "
                                      "comments changed their answers: something written in it is trying to sway this "
                                      "review. Treat it with suspicion."
                                    : "Small models on this computer read its code with and without its comments: nothing "
                                      "written in it tries to sway the review.");
    } else {
        snprintf(op, sizeof op, "No second opinion: the small models on this computer aren't ready (Settings > AI).");
    }
    int oh = draw_text_wrapped(NULL, small, (rect){0, 0, cw, 400}, op, muted, 1.3f);
    h += oh + 22 + 44 + pad;
    rect r = {(A.W - w) / 2, (A.H - h) / 2, w, h};
    shadow_round(A.frame, (rect){r.x, r.y + 16, r.w, r.h}, 22, 48, RGBA(0, 0, 0, 140));
    fill_round(A.frame, r, 18, bg);
    stroke_round(A.frame, r, 18, 1, color_alpha(ink, 0.14f));
    int x = r.x + pad, y = r.y + pad;
    draw_text(A.frame, title, x, y, head, ink);
    y += font_line_height(title) + 8;
    y += draw_text_wrapped(A.frame, small, (rect){x, y, cw, 400}, sub, muted, 1.3f) + 18;
    if (!G.m.n) {
        draw_text_wrapped(A.frame, body, (rect){x, y, cw, 200}, "It needs no permissions: it can't reach your files or "
                          "the internet.", ink, 1.3f);
        y += lh * 2 + 10;
    }
    for (int pass = 0; pass < (update ? 2 : 1); pass++) {
        if (update) {
            draw_text(A.frame, F(D.body_px - 1), x, y, pass == 0 ? "THIS UPDATE ALSO ASKS TO" : "IT ALREADY COULD",
                      pass == 0 ? accent : muted);
            y += lh + 10;
        }
        for (int i = 0; i < G.m.n; i++) {
            bool show = !update || (pass == 0 ? G.is_new[i] : G.was_held[i]);
            if (!show)
                continue;
            const fx_cap *c = &G.m.c[i];
            bool locked = c->required || (update && pass == 1);
            rect box = {x, y + 2, 20, 20};
            check_box(box, G.on[i], locked, ink, accent);
            draw_text_wrapped(A.frame, body, (rect){x + 34, y, cw - 40, 400}, says[i], ink, 1.3f);
            int th = rh[i] - lh - 14;
            char tag[160];
            snprintf(tag, sizeof tag, "%s%s  \xc2\xb7  line %d", c->required ? "needed for it to open" : "optional",
                     c->dynamic ? "  \xc2\xb7  an address or path it makes while running" : "", c->line);
            draw_text(A.frame, small, x + 34, y + th + 2, tag, muted);
            rect hit = {x, y, cw, rh[i] - 6};
            if (!locked && A.in.clicked && pt_in(hit, A.in.mx, A.in.my))
                G.on[i] = !G.on[i];
            y += rh[i];
        }
    }
    if (G.warnings && G.mismatch) {
        rect wb = {x, y, cw, wh};
        color wc = G.mismatch > 1 ? RGBA(214, 58, 48, 255) : RGBA(214, 150, 40, 255);
        fill_round(A.frame, wb, 12, color_alpha(wc, 0.16f));
        stroke_round(A.frame, wb, 12, 1.5f, color_alpha(wc, 0.8f));
        draw_text_wrapped(A.frame, body, (rect){x + 16, y + 14, cw - 32, wh}, G.warn, ink, 1.3f);
        y += wh + 14;
    }
    draw_text_wrapped(A.frame, small, (rect){x, y, cw, oh + 4}, op,
                      G.sc.injection && G.warnings ? RGBA(214, 58, 48, 255) : muted, 1.3f);
    y += oh + 22;
    rect allow = {r.x + w - pad - 150, y, 150, 44}, deny = {allow.x - 12 - 150, y, 150, 44};
    float rad = 12;
    if (ui_button(deny, "Don't allow", body, color_alpha(ink, 0.1f), ink, rad, true))
        G.allow = false, G.done = true;
    /* Allow waits a moment after the dialog appears, so a click meant for something else can't land on it */
    bool ready = A.now - G.opened > 900;
    if (ui_button(allow, update ? "Allow update" : "Allow", body, accent, readable_on(accent), rad, ready))
        G.allow = true, G.done = true;
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            G.allow = false, G.done = true;
    if (!G.logged && ready) {
        G.logged = true;
        LOGRECT("appgate-allow", allow);
        LOGRECT("appgate-deny", deny);
    }
    A.in.clicked = A.in.pressed = false;
    fm_animating = true;
}

/* ---- forbidden: never runs ---- */

static struct {
    bool done;
    char text[600];
} NO;

static void no_draw(void) {
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), 0.55f));
    int w = 520;
    font *body = F(D.body_px);
    int th = draw_text_wrapped(NULL, body, (rect){0, 0, w - 56, 400}, NO.text, D.surface_text, 1.35f);
    rect r = {(A.W - w) / 2, (A.H - th - 128) / 2, w, th + 128};
    fill_round(A.frame, r, 18, D.surface | 0xff000000u);
    draw_text_wrapped(A.frame, body, (rect){r.x + 28, r.y + 28, w - 56, th}, NO.text, D.surface_text, 1.35f);
    rect ok = {r.x + w - 28 - 130, r.y + r.h - 28 - 44, 130, 44};
    if (ui_button(ok, "OK", body, D.accent, readable_on(D.accent), 12, true))
        NO.done = true;
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC || A.in.keys[i] == K_ENTER)
            NO.done = true;
}

bool appgate_admit(const char *path, const fx_app *app, const char *src, fx_grants *out) {
    memset(&G, 0, sizeof G);
    G.path = path, G.app = app, G.src = src;
    fx_manifest_of(app, &G.m);
    memset(out, 0, sizeof *out);
    if (G.m.forbidden) {
        snprintf(NO.text, sizeof NO.text, "\xe2\x80\x9c%s\xe2\x80\x9d can't run on this computer: %s. No app may do that, "
                 "whatever it is allowed.", G.m.app, G.m.forbidden_why);
        NO.done = false;
        LOG("appgate: %s refused outright: %s\n", path, G.m.forbidden_why);
        main_modal(no_draw, &NO.done);
        return false;
    }
    security_load_grants(path, &G.old);
    fx_gate_of(&G.m, &G.old, &G.g);
    if (G.g.can_run) {
        *out = G.old;
        LOG("appgate: %s runs with what it was allowed (%d)\n", path, G.g.nheld);
        return true;
    }
    for (int i = 0; i < G.g.nnew; i++)
        G.is_new[G.g.newcaps[i]] = true;
    for (int i = 0; i < G.g.nheld; i++)
        G.was_held[G.g.held[i]] = true;
    G.warnings = security_warnings_on() && !security_app_quiet(path);
    G.mismatch = fx_mismatch(&G.m, G.warn, sizeof G.warn);
    for (int i = 0; i < G.m.n; i++) {
        const fx_cap *c = &G.m.c[i];
        /* what the rules warn about starts unticked when it is optional: they opt in, never out */
        bool warned = G.mismatch > 0 && (c->kind == FXC_INTERNET || ((c->kind == FXC_READ || c->kind == FXC_WRITE) &&
                                                                     !strcmp(c->what, "~")));
        G.on[i] = G.was_held[i] || c->required || (G.is_new[i] && !warned); /* refused optional ones stay refused */
    }
    if (G.warnings)
        G.review = job_start(review_child, NULL); /* without a model it answers "unavailable" quickly */
    G.opened = A.now;
    LOG("appgate: asking about %s: %d new, %d held, rules level %d\n", path, G.g.nnew, G.g.nheld, G.mismatch);
    main_modal(gate_draw, &G.done);
    if (G.review) { /* answered before the review came back */
        job_free(G.review);
        G.review = NULL;
    }
    if (!G.allow) {
        LOG("appgate: %s not allowed\n", path);
        return false;
    }
    fx_grants g = G.old;
    for (int i = 0; i < G.m.n; i++) {
        char k[220];
        fx_cap_key(&G.m.c[i], k, sizeof k);
        fx_grants_set(&g, k, G.on[i]);
    }
    security_save_grants(path, &g);
    *out = g;
    LOG("appgate: %s allowed\n", path);
    return true;
}

/* ---- Settings > Security ---- */

static struct {
    bool done, open;
    struct textfield typed;
    uint64_t opened;
} OFF;

static void off_draw(void) {
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), 0.6f));
    int w = 600 < A.W - 40 ? 600 : A.W - 40, pad = 28, cw = w - pad * 2;
    font *title = F(D.body_px + 5), *body = F(D.body_px);
    color ink = D.surface_text, red = RGBA(214, 58, 48, 255);
    const char *warn = "If someone asked you to turn this off, a person on the phone, a message, a website, or an app, "
                       "stop: you are probably being scammed. Nobody legitimate needs you to do this.\n\nOtherwise, only "
                       "go on if you know exactly what you are doing. With warnings off, nothing will tell you when an "
                       "app reaches for more than it should, and that puts your computer and your files at risk. Apps "
                       "still can't do anything you haven't allowed, and what each may do is still shown.";
    int th = draw_text_wrapped(NULL, body, (rect){0, 0, cw, 600}, warn, ink, 1.35f);
    char ask[200];
    snprintf(ask, sizeof ask, "To turn them off, type: %s", SECURITY_PHRASE);
    int h = pad + font_line_height(title) + 14 + th + 20 + font_line_height(body) + 10 + 46 + 24 + 44 + pad;
    rect r = {(A.W - w) / 2, (A.H - h) / 2, w, h};
    fill_round(A.frame, r, 18, D.surface | 0xff000000u);
    stroke_round(A.frame, r, 18, 2, color_alpha(red, 0.8f));
    int x = r.x + pad, y = r.y + pad;
    draw_text(A.frame, title, x, y, "Turn off warnings and second opinions?", red);
    y += font_line_height(title) + 14;
    draw_text_wrapped(A.frame, body, (rect){x, y, cw, th}, warn, ink, 1.35f);
    y += th + 20;
    draw_text(A.frame, body, x, y, ask, ink);
    y += font_line_height(body) + 10;
    rect field = {x, y, cw, 46};
    ui_textfield(field, &OFF.typed, true, "", body, color_alpha(ink, 0.07f), ink, color_alpha(ink, 0.5f), 10);
    ui_type(&OFF.typed);
    y += 46 + 24;
    uint64_t waited = A.now - OFF.opened;
    bool ok = security_can_turn_off(OFF.typed.buf, OFF.opened, A.now);
    char label[60];
    if (waited < SECURITY_WAIT_MS)
        snprintf(label, sizeof label, "Turn off (%llus)", (unsigned long long)((SECURITY_WAIT_MS - waited + 999) / 1000));
    else
        snprintf(label, sizeof label, "Turn off");
    rect go = {r.x + w - pad - 170, y, 170, 44}, back = {go.x - 12 - 150, y, 150, 44};
    static uint64_t logged_for;
    if (logged_for != OFF.opened) {
        logged_for = OFF.opened;
        LOGRECT("security-turnoff", go);
        LOGRECT("security-field", field);
    }
    if (ui_button(back, "Keep them on", body, D.accent, readable_on(D.accent), 12, true))
        OFF.done = true;
    if (ui_button(go, label, body, color_alpha(red, ok ? 0.9f : 0.3f), RGBA(255, 255, 255, 255), 12, ok)) {
        if (security_turn_off(OFF.typed.buf, OFF.opened, A.now))
            LOG("security: warnings and second opinions turned off by them\n");
        OFF.done = true;
    }
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            OFF.done = true;
    fm_animating = true; /* the countdown */
}

int security_page(int x0, int y0, int w, int h) {
    (void)h;
    font *head = F(D.body_px + 2), *body = F(D.body_px), *small = F(D.body_px - 2);
    color ink = D.surface_text, muted = color_alpha(ink, 0.62f), accent = D.accent;
    int x = x0, y = y0;
    bool on = security_warnings_on();
    draw_text(A.frame, head, x, y, "Apps and what they may do", ink);
    y += font_line_height(head) + 6;
    y += draw_text_wrapped(A.frame, small, (rect){x, y, w, 200},
                           "Every app is checked before it runs: what it can do is worked out from its code, and it gets "
                           "only what you allow. Forgetting an app's permissions asks you again the next time it opens.",
                           muted, 1.3f) + 14;
    char apps[48][300];
    int n = security_list_apps(apps, 48);
    if (!n) {
        draw_text(A.frame, body, x, y, "No apps have asked for anything yet.", muted);
        y += font_line_height(body) + 14;
    }
    for (int i = 0; i < n; i++) {
        fx_grants g;
        security_load_grants(apps[i], &g);
        const char *nm = strrchr(apps[i], '/') ? strrchr(apps[i], '/') + 1 : apps[i];
        draw_text(A.frame, body, x, y, nm, ink);
        draw_text(A.frame, small, x + text_width(body, nm) + 10, y + 3, apps[i], muted);
        y += font_line_height(body) + 4;
        int shown = 0;
        for (int k = 0; k < g.n; k++) {
            fx_cap c;
            if (!fx_cap_from_key(g.key[k], &c))
                continue;
            char says[400];
            fx_cap_says(&c, says, sizeof says);
            /* allowed: as it is; refused: said so, in the muted colour (no glyphs a font might not have) */
            char line[460];
            snprintf(line, sizeof line, "%s%s", g.yes[k] ? "" : "Refused: ", says);
            y += draw_text_wrapped(A.frame, small, (rect){x + 14, y, w - 14, 200}, line, g.yes[k] ? ink : muted, 1.3f) + 2;
            shown++;
        }
        if (!shown) {
            draw_text(A.frame, small, x + 14, y, "nothing that needs a permission", muted);
            y += font_line_height(small) + 2;
        }
        rect forget = {x, y + 6, 200, 34}, quiet = {x + 212, y + 6, 250, 34};
        if (ui_button(forget, "Forget its permissions", small, color_alpha(ink, 0.08f), ink, 9, true)) {
            security_forget_grants(apps[i]);
            LOG("security: forgot what %s may do\n", apps[i]);
        }
        bool q = security_app_quiet(apps[i]);
        if (ui_button(quiet, q ? "Warn me about it again" : "Stop warnings for this app", small, color_alpha(ink, 0.08f), ink,
                      9, true))
            security_set_app_quiet(apps[i], !q);
        y += 6 + 34 + 18;
    }
    y += 10;
    draw_text(A.frame, head, x, y, "Warnings and second opinions", ink);
    y += font_line_height(head) + 6;
    y += draw_text_wrapped(A.frame, small, (rect){x, y, w, 200},
                           on ? "On. You are told when an app reaches for more than its kind usually needs, and small "
                                "models on this computer give a second opinion on its code. To quieten one app, use "
                                "\"Stop warnings for this app\" above instead."
                              : "Off. Apps still get only what you allow, and what each may do is still shown before it "
                                "runs; nothing warns you or gives a second opinion.",
                           on ? muted : RGBA(214, 58, 48, 255), 1.3f) + 12;
    rect b = {x, y, 280, 40};
    static uint64_t logged;
    if (A.now - logged > 2000) {
        logged = A.now;
        LOGRECT("security-toggle", b);
    }
    if (on) {
        if (ui_button(b, "Turn off warnings...", body, color_alpha(ink, 0.08f), RGBA(214, 58, 48, 255), 10, true)) {
            memset(&OFF, 0, sizeof OFF);
            OFF.opened = A.now;
            LOG("security: the turn-off dialog opened\n");
            main_modal(off_draw, &OFF.done);
        }
    } else if (ui_button(b, "Turn warnings back on", body, accent, readable_on(accent), 10, true)) {
        security_turn_on();
        LOG("security: warnings back on\n");
    }
    return y + 40 + 20 - y0;
}
