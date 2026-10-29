#define _GNU_SOURCE
#include "preview.h"
#include "app.h"
#include "fm/fm.h"
#include "stylist.h"
#include "theme.h"
#include "wallpaper.h"
#include "wm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* See preview.h. The proposal is a document of its own, drawn offscreen at
 * the screen's size by the same engine as the desktop (with the shell's
 * design, document and frame pointed at it for the moment), then scaled
 * into the window. While a design is being written, the reply so far is
 * read from the design job (stylist_show) a few times a second and each
 * version morphs out of the one before. */

#define D (A.d)
#define MORPH_MS 650.0f
#define CONTROLS_H 116

enum pv_state { PV_NONE, PV_DRAFTING, PV_READY, PV_FAILED };
enum pv_action { PV_ACT_NONE, PV_ACT_APPLY, PV_ACT_DISCARD, PV_ACT_STOP };

static struct {
    enum pv_state state;
    fm_doc *doc;          /* the proposal: a copy of the desktop, edited, or a finished design */
    design d;
    fm_doc *live;         /* the design being written, shown while it grows */
    design live_d;
    bool live_whole;      /* `live` is complete (the stylist is polishing it) */
    bool changed;         /* there is something to apply */
    enum pv_action action; /* done at the start of the next frame, never mid-draw */

    canvas *full, *wall, *shot, *fade;
    design wall_for;
    bool wall_ok;
    uint64_t drawn_at, fade_at, read_at, status_at;
    bool redraw, before, before_drawn, window;
    int last_mx, last_my;
    int mx, my;           /* the pointer over the picture, in screen coordinates */

    uint64_t seen;        /* the last version of the reply read */
    char stage[64];       /* what the writing is on: colours, places, windows */
    char status[240];     /* what the stylist is doing */
    char failed[300];
    bool waiting;         /* they spoke while it was drafting */
    int convo_from;       /* the companion's messages from here on belong to this preview */

    char pointed[48];     /* id of what they pointed at ("" none) */
    char pointed_label[64];
    char pointed_what[300];
    struct textfield say;
    rect img;
} PV;

static fm_doc *shown(void) { return PV.live ? PV.live : PV.doc; }
static const design *shown_design(void) { return PV.live ? &PV.live_d : &PV.d; }
static bool onboarding(void) { return A.screen != SCR_DESKTOP; }

/* ---- documents ---- */

static fm_doc *copy_doc(fm_doc *src) {
    char *m = fm_serialize(src);
    fm_doc *d = m ? fm_parse(m) : NULL;
    free(m);
    if (d)
        fm_morph_from(d, src, 0); /* starts exactly as the desktop looks */
    return d;
}

static void begin_fade(void) {
    if (!PV.shot)
        return;
    if (!PV.fade || PV.fade->w != PV.shot->w || PV.fade->h != PV.shot->h) {
        canvas_free(PV.fade);
        PV.fade = canvas_new(PV.shot->w, PV.shot->h);
    }
    if (!PV.fade)
        return;
    memcpy(PV.fade->px, PV.shot->px, sizeof(uint32_t) * (size_t)PV.shot->w * (size_t)PV.shot->h);
    PV.fade_at = A.now;
}

static void show_window(void) {
    if (!onboarding())
        desktop_open_window(WIN_PREVIEW);
}

static void reset(void) {
    if (PV.live)
        fm_free(PV.live);
    if (PV.doc)
        fm_free(PV.doc);
    PV.live = PV.doc = NULL;
    PV.state = PV_NONE;
    PV.changed = PV.live_whole = PV.waiting = PV.before = false;
    PV.failed[0] = PV.pointed[0] = PV.stage[0] = 0;
    PV.say.len = 0;
    PV.say.buf[0] = 0;
}

fm_doc *preview_doc(void) { return PV.state != PV_NONE ? PV.doc : NULL; }
bool preview_showing(void) { return PV.state != PV_NONE; }

fm_doc *preview_target(void) {
    if (!PV.doc) {
        if (!A.ui)
            return NULL;
        PV.doc = copy_doc(A.ui);
        if (!PV.doc)
            return NULL;
        PV.d = A.d;
        if (PV.state == PV_NONE) {
            PV.state = PV_READY;
            PV.convo_from = A.companion.n;
        }
        PV.failed[0] = 0;
        LOG("preview: opened on a copy of the desktop\n");
    }
    show_window();
    PV.redraw = true;
    return PV.doc;
}

void preview_edited(void) {
    if (!PV.doc)
        return;
    PV.doc->morph_until = PV.doc->clock + MORPH_MS;
    PV.changed = true;
    PV.redraw = true;
}

void preview_drafting(void) {
    if (PV.state == PV_NONE)
        PV.convo_from = A.companion.n;
    PV.state = PV_DRAFTING;
    PV.failed[0] = PV.stage[0] = PV.status[0] = 0;
    PV.seen = 0;
    PV.live_whole = false;
    if (!PV.doc && A.ui && A.have_design) { /* the desktop as it is, until the new one arrives */
        PV.doc = copy_doc(A.ui);
        PV.d = A.d;
    }
    PV.redraw = true;
    show_window();
    LOG("preview: drafting\n");
}

/* The companion says what it did, and acts on anything they said while it
 * was drafting. */
static void tell_companion_draft_is_in(void) {
    char note[700];
    if (onboarding())
        snprintf(note, sizeof note,
                 "(The first draft of their space is in front of them now, in a preview: nothing is final until "
                 "they press \"Make it mine\". %sIn two or three short sentences, tell them why a few things are the "
                 "way they are, tied to what they told you, and that they can change anything first: by telling "
                 "you, or by clicking something in the picture to point at it.)",
                 PV.waiting ? "First act on what they said while you were drafting, with ui tags on the preview. " : "");
    else
        snprintf(note, sizeof note,
                 "(The redesign is ready in the preview window; nothing is applied until they press Apply. %sIn one or "
                 "two sentences tell them what you changed and that they can apply it or keep adjusting.)",
                 PV.waiting ? "First act on what they said while you were drafting, with ui tags on the preview. " : "");
    PV.waiting = false;
    companion_note(note, true);
}

void preview_draft(fm_doc *doc, const design *d) {
    fm_doc *from = shown();
    if (from)
        fm_morph_from(doc, from, MORPH_MS);
    if (!PV.live && from)
        begin_fade(); /* nothing was streamed: the old picture gives way */
    if (PV.live)
        fm_free(PV.live);
    if (PV.doc && PV.doc != doc)
        fm_free(PV.doc);
    PV.live = NULL;
    PV.doc = doc;
    PV.d = *d;
    if (onboarding())
        A.d = *d; /* the companion talks about it with its names and colours */
    PV.state = PV_READY;
    PV.changed = true;
    PV.redraw = true;
    show_window();
    LOG("preview: draft ready\n");
    tell_companion_draft_is_in();
}

void preview_failed(const char *why) {
    if (PV.live) {
        fm_free(PV.live);
        PV.live = NULL;
        begin_fade();
    }
    snprintf(PV.failed, sizeof PV.failed, "%s", why && *why ? why : "the design never came back");
    PV.state = PV_FAILED;
    PV.redraw = true;
    LOG("preview: failed: %s\n", PV.failed);
}

/* The reply so far, as a document that can be drawn: cut back to the last
 * whole tag (or rule, inside the stylesheet), with an empty desktop when
 * there is none yet so the wallpaper and colours show first. */
static void take_sofar(const char *text, bool whole) {
    static const struct {
        const char *tag, *words;
    } stages[] = {{"<meta", "Finding the words"},           {"<apps", "Naming things"},
                  {"<wallpaper", "Painting the wallpaper"}, {"<style", "Choosing colours and type"},
                  {"<desktop", "Putting things in place"},  {"<template", "Shaping the windows"}};
    const char *best = NULL, *words = NULL;
    for (size_t i = 0; i < sizeof stages / sizeof stages[0]; i++) {
        const char *p = strcasestr(text, stages[i].tag);
        if (p && (!best || p > best))
            best = p, words = stages[i].words;
    }
    snprintf(PV.stage, sizeof PV.stage, "%s", whole ? "" : words ? words : "Starting");

    stylist_quiet = true;
    char *t = stylist_expand_base(text);
    stylist_quiet = false;
    if (!t)
        return;
    sbuf b;
    sb_init(&b);
    if (!whole) {
        char *st = strcasestr(t, "<style");
        if (st && !strcasestr(st, "</style>")) {
            char *brace = strrchr(st, '}'), *open_end = strchr(st, '>');
            if (brace)
                brace[1] = 0;
            else if (open_end)
                open_end[1] = 0;
            else
                *st = 0;
            sb_add(&b, t);
            if (brace || open_end)
                sb_add(&b, "\n</style>\n");
        } else {
            char *gt = strrchr(t, '>');
            if (gt)
                gt[1] = 0;
            sb_add(&b, t);
        }
        if (!strcasestr(b.p ? b.p : "", "<desktop"))
            sb_add(&b, "\n<desktop/>\n");
    } else {
        sb_add(&b, t);
    }
    free(t);
    fm_doc *doc = fm_parse(b.p ? b.p : "");
    sb_free(&b);
    design d;
    char probs[300];
    if (!doc || !design_from_facet(&d, doc, &A.prof, probs, sizeof probs)) {
        if (doc)
            fm_free(doc);
        return;
    }
    fm_doc *from = shown();
    if (from)
        fm_morph_from(doc, from, MORPH_MS);
    if (!PV.live && from)
        begin_fade(); /* the desktop as it was gives way to the new one */
    if (PV.live)
        fm_free(PV.live);
    PV.live = doc;
    PV.live_d = d;
    PV.live_whole = whole;
    PV.redraw = true;
}

/* ---- every frame ---- */

void preview_frame(void) {
    design_poll(); /* the design job: finished, failed, or a retry */
    if (PV.action != PV_ACT_NONE) {
        enum pv_action act = PV.action;
        PV.action = PV_ACT_NONE;
        if (act == PV_ACT_STOP) { /* stop writing; keep what was there before */
            if (A.design_job) {
                job_free(A.design_job);
                A.design_job = NULL;
                A.design_attempt = 0;
            }
            if (PV.live) {
                fm_free(PV.live);
                PV.live = NULL;
                begin_fade();
            }
            PV.live_whole = PV.waiting = false;
            PV.state = PV.doc ? PV_READY : PV_NONE;
            PV.redraw = true;
            LOG("preview: stopped\n");
            if (PV.state == PV_NONE && !onboarding() && A.win[WIN_PREVIEW].open)
                wm_close(WIN_PREVIEW);
        }
        if (act == PV_ACT_DISCARD) {
            if (A.design_job) {
                job_free(A.design_job); /* stops it */
                A.design_job = NULL;
                A.design_attempt = 0;
            }
            reset();
            LOG("preview: discarded\n");
            if (!onboarding() && A.win[WIN_PREVIEW].open)
                wm_close(WIN_PREVIEW);
        } else if (act == PV_ACT_APPLY && (PV.live_whole || PV.doc)) {
            if (A.design_job) { /* the stylist was still polishing: what they see is what they get */
                job_free(A.design_job);
                A.design_job = NULL;
                A.design_attempt = 0;
            }
            fm_doc *doc = PV.live && PV.live_whole ? PV.live : PV.doc;
            design d = PV.live && PV.live_whole ? PV.live_d : PV.d;
            if (doc == PV.live)
                PV.live = NULL;
            else
                PV.doc = NULL;
            reset();
            LOG("preview: applied\n");
            bool first = onboarding();
            if (!first && A.win[WIN_PREVIEW].open)
                wm_close(WIN_PREVIEW);
            desktop_adopt(doc, &d, first);
        }
    }
    if (PV.state == PV_NONE)
        return;
    if (PV.state == PV_DRAFTING && A.now - PV.read_at > 200) {
        PV.read_at = A.now;
        bool whole = false;
        char *t = stylist_read_show(&PV.seen, &whole);
        if (t) {
            take_sofar(t, whole);
            free(t);
        }
    }
    if (A.now - PV.status_at > 300) {
        PV.status_at = A.now;
        if (PV.state != PV_DRAFTING || !stylist_read_status(PV.status, sizeof PV.status))
            PV.status[0] = 0;
    }
}

void preview_closed(void) {
    if (PV.state == PV_NONE)
        return;
    PV.action = PV_ACT_DISCARD;
}

void preview_apply(void) {
    if (PV.state == PV_NONE)
        return;
    PV.action = PV_ACT_APPLY;
    if (!onboarding())
        theme_crossfade(); /* this frame, the old look, fades out over the new */
}

void preview_discard(void) {
    if (PV.state != PV_NONE)
        PV.action = PV_ACT_DISCARD;
}

const char *preview_prompt(void) {
    if (PV.state == PV_NONE)
        return "\nChanges to the look go through a preview: your style, set, add and remove tags open a preview "
               "window showing the change, and nothing is real until they press Apply there. So say \"have a look "
               "in the preview\", never that it is done. Press, open and close act on the real desktop.";
    if (onboarding())
        return "\nRIGHT NOW: they are looking at the first draft of their space in a preview, before it becomes "
               "theirs. The document above IS that draft. Your style, set, add and remove tags change it while "
               "they watch. When they are happy (\"perfect\", \"make it mine\", \"let's go\"), end with "
               "[[preview:apply]]. If they point at something, their message names it.";
    return "\nRIGHT NOW: a preview window is open with proposed changes; nothing is applied yet. The document above "
           "IS the preview. Your style, set, add and remove tags change the preview while they watch. When they "
           "say it's right (\"apply it\", \"keep it\", \"perfect\"), end with [[preview:apply]]; to drop it, "
           "[[preview:discard]]. If they point at something, their message names it.";
}

/* ---- drawing ---- */

static void paint_wall(const design *d) {
    if (!PV.wall)
        PV.wall = canvas_new(A.W, A.H);
    if (!PV.wall)
        return;
    const design *w = &PV.wall_for;
    if (PV.wall_ok && w->wallpaper == d->wallpaper && !memcmp(w->wp, d->wp, sizeof d->wp) &&
        w->wp_seed == d->wp_seed && w->wp_intensity == d->wp_intensity && w->bg == d->bg)
        return;
    wallpaper_paint(PV.wall, d);
    PV.wall_for = *d;
    PV.wall_ok = true;
}

static rect sample_window(fm_doc *doc) {
    rect wk = doc->work.w > 300 && doc->work.h > 240 ? doc->work : (rect){0, 0, A.W, A.H};
    int ww = wk.w * 46 / 100, wh = wk.h * 52 / 100;
    return (rect){wk.x + wk.w - ww - wk.w / 12, wk.y + (wk.h - wh) / 2, ww, wh};
}

/* Draws a document at the screen's size, as the desktop would. */
static void draw_full(fm_doc *doc, const design *d, float dt) {
    if (!PV.full)
        PV.full = canvas_new(A.W, A.H);
    if (!PV.full)
        return;
    canvas_unclip(PV.full);
    paint_wall(d);
    if (PV.wall)
        canvas_blit(PV.full, PV.wall, 0, 0);
    canvas *saved_frame = A.frame;
    fm_doc *saved_ui = A.ui;
    input_state saved_in = A.in;
    static design saved_d;
    saved_d = A.d;
    A.d = *d;
    A.ui = doc;
    A.frame = PV.full;
    memset(&A.in, 0, sizeof A.in);
    A.in.mx = PV.mx;
    A.in.my = PV.my;
    doc->in = &A.in;
    fm_layout(doc, (rect){0, 0, A.W, A.H});
    fm_animate(doc, dt);
    fm_input(doc, 3); /* hover only: what they are pointing at */
    fm_render_layer(doc, PV.full, 0);
    if (PV.window) {
        rect body;
        fm_render_window_frame(doc, PV.full, WIN_FILES, sample_window(doc), true, &body);
        canvas_clip(PV.full, body);
        stylist_placeholder_view("files", body);
        canvas_unclip(PV.full);
    }
    fm_render_layer(doc, PV.full, 1);
    fm_render_layer(doc, PV.full, 2);
    doc->in = NULL;
    A.frame = saved_frame;
    A.ui = saved_ui;
    A.in = saved_in;
    A.d = saved_d;
}

/* The desktop as it is, for comparing. */
static void draw_before(void) {
    if (!PV.full)
        PV.full = canvas_new(A.W, A.H);
    if (!PV.full || !A.ui || !A.wall)
        return;
    canvas_unclip(PV.full);
    canvas_blit(PV.full, A.wall, 0, 0);
    canvas *saved_frame = A.frame;
    A.frame = PV.full;
    fm_layout(A.ui, (rect){0, 0, A.W, A.H});
    fm_render_layer(A.ui, PV.full, 0);
    fm_render_layer(A.ui, PV.full, 1);
    fm_render_layer(A.ui, PV.full, 2);
    A.frame = saved_frame;
}

static void scale_into_shot(rect img) {
    if (!PV.shot || PV.shot->w != img.w || PV.shot->h != img.h) {
        canvas_free(PV.shot);
        PV.shot = canvas_new(img.w, img.h);
    }
    if (PV.shot && PV.full)
        canvas_scale(PV.shot, PV.full);
}

/* The old picture, fading out over the new one. */
static void draw_fade(rect img) {
    if (!PV.fade || !PV.fade_at || PV.fade->w != img.w || PV.fade->h != img.h)
        return;
    float t = (float)(A.now - PV.fade_at) / 420.0f;
    if (t >= 1) {
        PV.fade_at = 0;
        return;
    }
    float e = 1 - t * t * (3 - 2 * t);
    uint32_t a = (uint32_t)(e * 256), ia = 256 - a;
    canvas *c = A.frame;
    int x0 = img.x > c->cx0 ? img.x : c->cx0, y0 = img.y > c->cy0 ? img.y : c->cy0;
    int x1 = img.x + img.w < c->cx1 ? img.x + img.w : c->cx1, y1 = img.y + img.h < c->cy1 ? img.y + img.h : c->cy1;
    for (int y = y0; y < y1; y++) {
        uint32_t *dst = c->px + (size_t)y * c->w, *src = PV.fade->px + (size_t)(y - img.y) * PV.fade->w - img.x;
        for (int x = x0; x < x1; x++) {
            uint32_t p = dst[x], q = src[x];
            uint32_t rb = (((q & 0xff00ff) * a + (p & 0xff00ff) * ia) >> 8) & 0xff00ff;
            uint32_t g = (((q & 0x00ff00) * a + (p & 0x00ff00) * ia) >> 8) & 0x00ff00;
            dst[x] = 0xff000000u | rb | g;
        }
    }
}

/* Where its controls are, for tests: logged when they move, and now and then. */
static void log_rect(int k, const char *name, rect r) {
    static rect last[5];
    static uint64_t at[5];
    if (!memcmp(&last[k], &r, sizeof r) && A.now - at[k] < 3000)
        return;
    last[k] = r;
    at[k] = A.now;
    LOGRECT(name, r);
}

static rect to_img(rect v) {
    rect i = PV.img;
    return (rect){i.x + v.x * i.w / A.W, i.y + v.y * i.h / A.H, v.w * i.w / A.W, v.h * i.h / A.H};
}

static bool authored(const fm_node *n) { return n->id[0] && n->id[0] != '_'; }

/* What they pointed at: its own id (to outline it), a few words for the chip,
 * and for the companion a description with a selector it can use in a tag
 * (the element's id, or the nearest id above it and the tags down to it). */
static void describe_pointed(fm_doc *doc, fm_node *n) {
    char *t = fm_text_of(n);
    if (t) {
        for (char *p = t; *p; p++)
            if (*p == '\n' || *p == '\t')
                *p = ' ';
        char *s = t + strspn(t, " ");
        memmove(t, s, strlen(s) + 1);
        if (strlen(t) > 32)
            strcpy(t + 29, "...");
    }
    const char *label = fm_attr_get(n, "label");
    int app = n->generated && n->app >= 0 ? n->app : -1;
    snprintf(PV.pointed, sizeof PV.pointed, "%s", n->id);
    if (app >= 0)
        snprintf(PV.pointed_label, sizeof PV.pointed_label, "%s", fm_host_app_label(app));
    else if (label && *label)
        snprintf(PV.pointed_label, sizeof PV.pointed_label, "%s", label);
    else if (t && *t)
        snprintf(PV.pointed_label, sizeof PV.pointed_label, "%s", t);
    else
        snprintf(PV.pointed_label, sizeof PV.pointed_label, "the %s", n->tag);
    char sel[160] = "";
    if (authored(n)) {
        snprintf(sel, sizeof sel, "#%s", n->id);
    } else {
        const char *path[6];
        int k = 0;
        const fm_node *p = n;
        while (p && k < 6 && !authored(p) && p != doc->desktop) {
            path[k++] = p->tag;
            p = p->parent;
        }
        size_t o = 0;
        if (p && authored(p))
            o += (size_t)snprintf(sel + o, sizeof sel - o, "#%s", p->id);
        while (k-- > 0 && o < sizeof sel)
            o += (size_t)snprintf(sel + o, sizeof sel - o, "%s%s", o ? " " : "", path[k]);
    }
    snprintf(PV.pointed_what, sizeof PV.pointed_what, "a <%s>%s%s%s%s%s (selector: %s)", n->tag,
             t && *t ? " showing \"" : "", t && *t ? t : "", t && *t ? "\"" : "", app >= 0 ? " for the app " : "",
             app >= 0 ? fm_host_app_id(app) : "", sel);
    free(t);
}

static void send_words(void) {
    char msg[1400];
    if (PV.pointed[0])
        snprintf(msg, sizeof msg, "%s\n(They are pointing at %s in the preview.)", PV.say.buf, PV.pointed_what);
    else
        snprintf(msg, sizeof msg, "%s", PV.say.buf);
    PV.say.len = 0;
    PV.say.buf[0] = 0;
    PV.pointed[0] = 0;
    if (PV.state == PV_DRAFTING) {
        /* It can't be interrupted mid-sentence: what they said is worked in
         * the moment the draft is down. */
        convo_add(&A.companion, "user", msg);
        PV.waiting = true;
    } else {
        companion_tell(msg);
    }
}

/* The companion's last words in this preview, or a hint. */
static void strip_text(char *out, size_t n) {
    const char *ai = A.prof.ai_name[0] ? A.prof.ai_name : D.ai_name[0] ? D.ai_name : "I";
    if (A.companion.pending) {
        snprintf(out, n, "%s%s", ai, strcmp(ai, "I") ? " is on it..." : "'m on it...");
        return;
    }
    if (PV.state == PV_DRAFTING && PV.waiting) {
        snprintf(out, n, "Got it. I'll work that in as soon as this draft is down.");
        return;
    }
    for (int i = A.companion.n - 1; i >= PV.convo_from && i >= 0; i--) {
        struct msg *m = &A.companion.msgs[i];
        if (!strcmp(m->role, "assistant") && m->text[strspn(m->text, " \t\r\n")]) {
            snprintf(out, n, "%s", m->text);
            return;
        }
        if (!strcmp(m->role, "user"))
            break; /* not answered yet */
    }
    if (PV.state == PV_FAILED)
        snprintf(out, n, "That didn't come through (%s). Try again, or tell me what you'd like.", PV.failed);
    else if (PV.state == PV_DRAFTING)
        snprintf(out, n, "Watch it come together. Tell me anything you'd change, or click something to point at it.");
    else if (!PV.changed)
        snprintf(out, n, "What would you like to change? Tell me, or click something in the picture to point at it.");
    else
        snprintf(out, n, "Tell me anything you'd change, or click something to point at it.");
}

void preview_body(rect r, bool focused) {
    if (PV.state == PV_NONE)
        return;
    bool first = onboarding();
    const char *face = first ? "Inter" : D.font_body;
    float px = first ? 16 : (D.body_px >= 13 ? D.body_px : 15);
    font *f = font_get(face, px), *small = font_get(face, px - 2 >= 12 ? px - 2 : 12);
    color accent = first ? (A.prof.has_palette ? A.prof.palette[1] : RGBA(232, 131, 107, 255)) : D.accent;
    float rad = first ? 12 : D.radius * 0.6f + 4;

    /* the picture, as large as the space allows */
    rect area = {r.x, r.y, r.w, r.h - CONTROLS_H};
    int iw = area.w, ih = (int)((float)iw * A.H / A.W);
    if (ih > area.h) {
        ih = area.h;
        iw = (int)((float)ih * A.W / A.H);
    }
    if (iw < 40 || ih < 30)
        return;
    rect img = {area.x + (area.w - iw) / 2, area.y + (area.h - ih) / 2, iw, ih};
    PV.img = img;
    log_rect(4, "preview-image", img);
    bool over = focused && pt_in(img, A.in.mx, A.in.my);
    PV.mx = over ? (A.in.mx - img.x) * A.W / img.w : -10000;
    PV.my = over ? (A.in.my - img.y) * A.H / img.h : -10000;

    fm_doc *sh = shown();
    if (PV.before && A.ui) {
        if (!PV.before_drawn || A.now - PV.drawn_at > 500) {
            draw_before();
            scale_into_shot(img);
            PV.before_drawn = true;
            PV.drawn_at = A.now;
        }
    } else if (sh) {
        PV.before_drawn = false;
        bool moving = sh->morph_until > sh->clock || sh->entrance_at >= 0;
        bool pointer = PV.mx != PV.last_mx || PV.my != PV.last_my;
        bool resized = !PV.shot || PV.shot->w != img.w || PV.shot->h != img.h;
        if (resized || ((PV.redraw || moving || pointer) && A.now - PV.drawn_at >= 90)) {
            float dt = PV.drawn_at ? (float)(A.now - PV.drawn_at) : 16;
            draw_full(sh, shown_design(), dt);
            scale_into_shot(img);
            PV.drawn_at = A.now;
            PV.redraw = false;
            PV.last_mx = PV.mx, PV.last_my = PV.my;
        }
    }
    if ((sh || PV.before) && PV.shot) {
        canvas_blit(A.frame, PV.shot, img.x, img.y);
        draw_fade(img);
    } else {
        /* nothing to show yet: their colours, and a light at work */
        color a = A.prof.has_palette ? A.prof.palette[1] : accent, b = A.prof.has_palette ? A.prof.palette[2] : D.accent2;
        fill_round_vgradient(A.frame, img, 6, color_alpha(a, 0.35f), color_alpha(b, 0.35f));
        ui_orb(A.frame, img.x + img.w / 2.0f, img.y + img.h / 2.0f, img.h / 9.0f, a, b, true);
    }
    stroke_round(A.frame, img, 6, 1, RGBA(0, 0, 0, 40));

    /* what the pointer is on, and what they pointed at */
    if (sh && !PV.before) {
        if (PV.pointed[0]) {
            char sel[56];
            snprintf(sel, sizeof sel, "#%s", PV.pointed);
            fm_node *p = fm_find(sh, sel);
            if (p && p->laid_out) {
                rect pr = to_img(p->vis);
                fill_round(A.frame, pr, 4, color_alpha(accent, 0.16f));
                stroke_round(A.frame, pr, 4, 2, accent);
            }
        }
        fm_node *h = over ? sh->hovered : NULL;
        if (h && h != sh->desktop && h->laid_out) {
            stroke_round(A.frame, to_img(h->vis), 4, 1.5f, color_alpha(accent, 0.85f));
            if (A.in.clicked) {
                describe_pointed(sh, h);
                LOG("preview: pointing at #%s: %s\n", PV.pointed, PV.pointed_what);
            }
        }
    }

    /* while it works: what it is doing, in the corner of the picture */
    if (PV.state == PV_DRAFTING || PV.state == PV_FAILED) {
        char line[260];
        if (PV.state == PV_FAILED)
            snprintf(line, sizeof line, "Didn't come through");
        else if (PV.stage[0] && PV.status[0]) /* what it is on, and how far along */
            snprintf(line, sizeof line, "%s \xc2\xb7 %s", PV.stage, PV.status);
        else
            snprintf(line, sizeof line, "%s", PV.stage[0] ? PV.stage : PV.status[0] ? PV.status : "Thinking it through");
        int tw = text_width(small, line);
        rect pill = {img.x + 12, img.y + img.h - 44, tw + 52, 32};
        fill_round(A.frame, pill, 16, RGBA(20, 20, 24, 170));
        if (PV.state == PV_DRAFTING)
            ui_orb(A.frame, pill.x + 17.0f, pill.y + 16.0f, 8, accent, RGBA(255, 255, 255, 255), true);
        else
            fill_round(A.frame, (rect){pill.x + 11, pill.y + 10, 12, 12}, 6, RGBA(226, 92, 72, 255));
        draw_text(A.frame, small, pill.x + 34, pill.y + (32 - font_line_height(small)) / 2, line, RGBA(255, 255, 255, 235));
    }

    /* the conversation: its last words; on the right, compare, and see a window in it */
    color under = theme_under((rect){r.x, img.y + img.h + 6, r.w, 40});
    color ink = theme_fit(first ? RGBA(40, 34, 30, 255) : D.surface_text, under, 4.5f);
    color muted = theme_fit(first ? RGBA(90, 80, 72, 255) : D.muted, under, 4.5f);
    int chip_x = img.x + img.w, chip_y = img.y + img.h + 8;
    if (sh && !PV.before) {
        const char *wl = PV.window ? "Hide the window" : "Show a window";
        int ww = text_width(small, wl) + 28;
        chip_x -= ww;
        rect wb = {chip_x, chip_y, ww, 30};
        if (ui_button(wb, wl, small, PV.window ? accent : color_alpha(ink, 0.10f), PV.window ? readable_on(accent) : ink,
                      15, true)) {
            PV.window = !PV.window;
            PV.redraw = true;
        }
        chip_x -= 8;
    }
    if (A.ui && !first && (sh || PV.before)) {
        const char *bl = PV.before ? "Back to the new one" : "Show before";
        int bw = text_width(small, bl) + 28;
        chip_x -= bw;
        rect bb = {chip_x, chip_y, bw, 30};
        if (ui_button(bb, bl, small, PV.before ? accent : color_alpha(ink, 0.10f), PV.before ? readable_on(accent) : ink,
                      15, true)) {
            PV.before = !PV.before;
            PV.redraw = true;
            PV.before_drawn = false;
        }
        log_rect(0, "preview-before", bb);
        chip_x -= 8;
    }
    char words[900];
    strip_text(words, sizeof words);
    rect sr = {r.x + 4, img.y + img.h + 10, chip_x - 12 - r.x, CONTROLS_H - 62};
    canvas_clip(A.frame, sr);
    draw_text_wrapped(A.frame, small, sr, words, A.companion.pending ? muted : ink, 1.3f);
    canvas_unclip(A.frame);

    /* what they say, and the decision */
    int bh = 44, y = r.y + r.h - bh;
    font *bf = f;
    const char *primary = first ? "Make it mine" : PV.state == PV_DRAFTING && PV.live_whole ? "Use this" : "Apply";
    const char *secondary = PV.state == PV_DRAFTING ? (first ? NULL : "Stop") : PV.state == PV_FAILED ? "Try again"
                            : first ? NULL : "Discard";
    int pw = text_width(bf, primary) + 40, sw = secondary ? text_width(bf, secondary) + 36 : 0;
    rect pb = {r.x + r.w - pw, y, pw, bh};
    rect sb = {pb.x - 10 - sw, y, sw, bh};
    int input_w = (secondary ? sb.x : pb.x) - 12 - r.x;
    rect in = {r.x, y, input_w, bh};
    if (PV.pointed[0]) { /* a chip: what the next words are about */
        char chip[120];
        snprintf(chip, sizeof chip, "Pointing at %s  \xc3\x97", PV.pointed_label);
        int cw = text_width(small, chip) + 24;
        rect cr = {in.x, y - 34, cw, 28};
        fill_round(A.frame, cr, 14, color_alpha(accent, 0.9f));
        draw_text_centered(A.frame, small, cr, chip, readable_on(accent));
        if (pt_in(cr, A.in.mx, A.in.my) && A.in.clicked)
            PV.pointed[0] = 0;
    }
    char ph[120];
    const char *ai = A.prof.ai_name[0] ? A.prof.ai_name : D.ai_name;
    snprintf(ph, sizeof ph, "Tell %s what to change...", ai[0] ? ai : "me");
    ui_textfield(in, &PV.say, focused, ph, f, color_alpha(ink, 0.08f), ink, muted, rad);
    log_rect(1, "preview-input", in);
    if (focused && ui_type(&PV.say) && PV.say.len)
        send_words();
    bool can_apply = PV.state == PV_READY ? (PV.changed || first)
                                          : PV.state == PV_DRAFTING ? PV.live_whole : PV.doc && PV.changed;
    if (ui_button(pb, primary, bf, accent, readable_on(accent), rad, can_apply))
        preview_apply();
    log_rect(2, "preview-apply", pb);
    if (secondary) {
        if (ui_button(sb, secondary, bf, color_alpha(ink, 0.10f), ink, rad, true)) {
            if (PV.state == PV_FAILED)
                design_begin("(retry)");
            else if (PV.state == PV_DRAFTING)
                PV.action = PV_ACT_STOP;
            else
                preview_discard();
        }
        log_rect(3, "preview-discard", sb);
    }
}
