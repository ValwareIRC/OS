#define _GNU_SOURCE
#include "caption.h"
#include "app.h"
#include "fm/fm.h"
#include "theme.h"
#include "voice.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define SAY_FILE "/run/os-say"

/* The companion's caption: see caption.h. Where it goes is worked out every
 * frame, because windows move: near the companion's light, on whichever
 * side covers no window (and never the focused one), gliding when it has
 * to move. */

#define FADE_IN 360.0f
#define FADE_OUT 520.0f
#define MAX_LINES 7

static struct {
    char text[800];
    int flags;
    uint64_t start, until; /* when it appears (after the desktop settles) and when it goes */
    uint64_t leaving;      /* fading out since; 0 while it stays */
    rect card, close;      /* where it was drawn last frame */
    float x, y;            /* its position, easing toward where it should be */
    bool placed;
    rect anchor;
    uint64_t anchor_at;
    bool shown;
} C;

static bool companion_open(void) { return A.win[WIN_COMPANION].open && !A.win[WIN_COMPANION].minimized; }

bool caption_say(const char *text, int flags) {
    if (!text)
        return false;
    while (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r')
        text++;
    if (!*text || companion_open())
        return false;
    if ((flags & CAPTION_IF_QUIET) && caption_showing())
        return false;
    snprintf(C.text, sizeof C.text, "%s", text);
    for (size_t l = strlen(C.text); l && strchr(" \t\r\n", C.text[l - 1]); l--)
        C.text[l - 1] = 0;
    C.flags = flags;
    /* The space arrives first; words wait until it has settled. */
    uint64_t settle = A.screen == SCR_DESKTOP ? A.screen_since + 3000 : A.now + 3000;
    bool already = caption_showing() && !C.leaving;
    C.start = already ? A.now - (uint64_t)FADE_IN : (A.now > settle ? A.now : settle);
    /* long enough to read twice at an easy pace, but not to linger */
    size_t n = strlen(C.text);
    uint64_t read = 3200 + 62 * (uint64_t)n;
    C.until = C.start + (read < 5000 ? 5000 : read > 22000 ? 22000 : read);
    C.leaving = 0;
    if (!already)
        C.placed = false;
    if (flags & CAPTION_SPEAK)
        voice_say(C.text);
    LOG("caption: %.120s\n", C.text);
    return true;
}

void caption_dismiss(void) {
    if (C.text[0] && !C.leaving)
        C.leaving = A.now;
}

bool caption_showing(void) { return C.text[0] != 0; }

void caption_anchor(rect orb) {
    C.anchor = orb;
    C.anchor_at = A.now;
}

/* Something on the desktop that opens the conversation, when there is no
 * orb: an app item for the companion, or a button that opens it. */
static bool find_launcher(fm_node *n, rect *out, int depth) {
    for (; n && depth < 14; n = n->next) {
        if (n->kind != FM_ELEM || !n->laid_out || n->st.hidden || (fm_layer_of(n) == 2 && !n->open))
            continue;
        const char *what = fm_attr_get(n, "what");
        if ((n->generated && n->app == APP_COMPANION) ||
            (what && (strcasestr(what, "open:companion") || !strncasecmp(what, "ask", 3)))) {
            if (n->vis.w > 0 && n->vis.h > 0) {
                *out = n->vis;
                return true;
            }
        }
        if (find_launcher(n->first, out, depth + 1))
            return true;
    }
    return false;
}

static int overlap(rect a, rect b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0;
}

/* The design's own controls (dock icons, taskbar buttons, menu items): the
 * card must not sit over them either, or a click meant for them opens the
 * conversation instead. */
static int controls(fm_node *n, rect *out, int k, int max, int depth) {
    for (; n && k < max && depth < 14; n = n->next) {
        if (n->kind != FM_ELEM || !n->laid_out || n->st.hidden || (fm_layer_of(n) == 2 && !n->open))
            continue;
        static const char *const live[] = {"volume", "network", "notifications", "quicksettings", "clock", "search",
                                           "button", "orb", "windowlist", NULL};
        bool control = n->generated || fm_attr_get(n, "what");
        for (int i = 0; live[i] && !control; i++)
            control = !strcmp(n->tag, live[i]);
        if (control && n->vis.w > 0 && n->vis.h > 0)
            out[k++] = n->vis;
        else
            k = controls(n->first, out, k, max, depth + 1);
    }
    return k;
}

static int focused_window(void) {
    int best = -1;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized && (best < 0 || A.win[w].z > A.win[best].z))
            best = w;
    return best;
}

/* The best place for a w x h card near the anchor. Returns its score: 0
 * means it covers nothing; >= 1e6 means it covers the focused window. */
static double place(rect a, int w, int h, int *ox, int *oy) {
    int gap = 14, m = 12;
    int ax = a.x + a.w / 2, ay = a.y + a.h / 2;
    rect work = A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H};
    int cand[18][2] = {
        {m, A.H - m - h},                  /* the screen's own corners, when the free space has none to spare */
        {A.W - m - w, A.H - m - h},
        {A.W - m - w, m},
        {m, m},
        {ax - w / 2, a.y - gap - h},       /* above */
        {a.x + a.w - w, a.y - gap - h},    /* above, its right edge on the anchor's */
        {a.x, a.y - gap - h},              /* above, left edges together */
        {ax - w / 2, a.y + a.h + gap},     /* below */
        {a.x + a.w - w, a.y + a.h + gap},
        {a.x, a.y + a.h + gap},
        {a.x - gap - w, ay - h / 2},       /* left */
        {a.x + a.w + gap, ay - h / 2},     /* right */
        {a.x - gap - w, a.y + a.h - h},
        {a.x + a.w + gap, a.y + a.h - h},
        {work.x + work.w - w - 20, work.y + work.h - h - 20}, /* corners of the free space */
        {work.x + 20, work.y + work.h - h - 20},
        {work.x + work.w - w - 20, work.y + 20},
        {work.x + 20, work.y + 20},
    };
    int fw = focused_window();
    static rect ctl[160];
    int nctl = A.ui ? controls(A.ui->desktop, ctl, 0, 160, 0) : 0;
    double best = 1e18;
    for (int i = 0; i < 18; i++) {
        int x = cand[i][0], y = cand[i][1];
        if (x < m) x = m;
        if (y < m) y = m;
        if (x + w > A.W - m) x = A.W - m - w;
        if (y + h > A.H - m) y = A.H - m - h;
        rect r = {x, y, w, h};
        double s = 0;
        for (int k = 0; k < WIN_MAX; k++)
            if (A.win[k].open && !A.win[k].minimized)
                s += overlap(r, A.win[k].r) * (k == fw ? 1e6 : 4.0);
        s += overlap(r, a) * 50.0; /* never over the light itself */
        for (int c = 0; c < nctl; c++)
            s += overlap(r, ctl[c]) * 2e5;
        double dx = x + w / 2.0 - ax, dy = y + h / 2.0 - ay;
        s += sqrt(dx * dx + dy * dy) * 2;
        if (s < best) {
            best = s;
            *ox = x, *oy = y;
        }
    }
    return best;
}

static float ease(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

/* How visible it is now (0..1); clears it once it has gone. */
static float presence(void) {
    if (!C.text[0] || A.now < C.start)
        return 0;
    float in = ease((float)(A.now - C.start) / FADE_IN);
    if (!C.leaving && !(C.flags & CAPTION_STICKY) && A.now > C.until)
        C.leaving = A.now;
    if (C.leaving) {
        float t = (float)(A.now - C.leaving) / FADE_OUT;
        if (t >= 1) {
            C.text[0] = 0;
            C.shown = false;
            return 0;
        }
        return in * (1 - t * t);
    }
    return in;
}

/* Programs outside the shell (scripts, tests, the terminal) can have the
 * companion say a line: write it to SAY_FILE and it is shown, then removed. */
static void say_file(void) {
    static uint64_t checked;
    if (A.now - checked < 500)
        return;
    checked = A.now;
    FILE *f = fopen(SAY_FILE, "r");
    if (!f)
        return;
    char line[800];
    size_t n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = 0;
    unlink(SAY_FILE);
    caption_say(line, 0);
}

void caption_input(void) {
    /* Someone else wrote straight to the old subtitle: show it the new way. */
    if (A.caption[0]) {
        caption_say(A.caption, 0);
        A.caption[0] = 0;
    }
    say_file();
    if (!C.shown || C.leaving || !pt_in(C.card, A.in.mx, A.in.my))
        return;
    /* Reading it: it stays while the pointer rests on it. */
    if (!(C.flags & CAPTION_STICKY) && C.until < A.now + 1500)
        C.until = A.now + 1500;
    if (A.in.clicked) {
        if (pt_in(C.close, A.in.mx, A.in.my)) {
            caption_dismiss();
        } else {
            C.text[0] = 0;
            C.shown = false;
            fm_host_action(A.ui, NULL, "open:companion");
        }
    }
    /* the click was for the card, not what lies under it */
    A.in.clicked = A.in.pressed = false;
}

void caption_draw(void) {
    if (companion_open()) {
        C.text[0] = 0; /* the conversation shows it */
        C.shown = false;
        return;
    }
    /* still being spoken: stay until the voice is done */
    if (C.text[0] && !C.leaving && voice_speaking() && C.until < A.now + 1200)
        C.until = A.now + 1200;
    float alpha = presence();
    C.shown = alpha > 0.02f;
    if (!C.shown)
        return;
    design *D = &A.d;
    float scale = A.prof.text_scale > 0.5f ? A.prof.text_scale : 1;
    /* Words are read here, not admired: display faces give way to Inter. */
    static const char *const display[] = {"Fraunces", "Syne", "Unbounded", "Caveat", "PlayfairDisplay", NULL};
    const char *face = D->font_body[0] ? D->font_body : "Inter";
    for (int i = 0; display[i]; i++)
        if (!strcasecmp(face, display[i]))
            face = "Inter";
    font *f = font_get(face, 16.5f * scale), *nf = font_get(D->font_heading[0] ? D->font_heading : "Inter", 13 * scale);
    int lh = font_line_height(f);

    /* the anchor: the orb drawn this frame, else a launcher, else a corner */
    rect a;
    if (C.anchor_at == A.now && C.anchor.w > 0)
        a = C.anchor;
    else if (!A.ui || !find_launcher(A.ui->desktop, &a, 0)) {
        rect work = A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H};
        a = (rect){work.x + work.w - 40, work.y + work.h - 40, 16, 16};
    }

    /* full card first; a single line beside the light if that would cover
     * the window they're working in */
    int pad = 18, head = (int)(22 * scale);
    int w = (int)(A.W * 0.34f);
    if (w > 440 * scale) w = (int)(440 * scale);
    if (w < 300) w = 300;
    int th = draw_text_wrapped(NULL, f, (rect){0, 0, w - pad * 2, 4000}, C.text, 0, 1.35f);
    int maxh = (int)(lh * 1.35f * MAX_LINES);
    bool cut = th > maxh;
    if (cut)
        th = maxh;
    int h = pad + head + 6 + th + pad - 4;
    int x, y;
    bool compact = place(a, w, h, &x, &y) >= 1e6;
    char line[200] = "";
    if (compact) {
        int cw = w + 120;
        h = lh + 22;
        /* as much of the first words as fits, then an ellipsis */
        int n = 0, len = (int)strlen(C.text);
        while (n < len && n < (int)sizeof line - 4 && C.text[n] != '\n') {
            int next = n + 1;
            while (next < len && (C.text[next] & 0xc0) == 0x80)
                next++;
            if (text_width_n(f, C.text, next) > cw - 2 * pad - 30 - 26)
                break;
            n = next;
        }
        snprintf(line, sizeof line, "%.*s%s", n, C.text, n < len ? "..." : "");
        w = text_width(f, line) + 2 * pad + 30 + 26;
        place(a, w, h, &x, &y);
    }
    /* glide to a new place rather than jump */
    if (!C.placed) {
        C.x = (float)x, C.y = (float)y;
        C.placed = true;
    } else {
        C.x += ((float)x - C.x) * 0.22f;
        C.y += ((float)y - C.y) * 0.22f;
    }
    float rise = (1 - ease((float)(A.now - C.start) / FADE_IN)) * 10;
    rect card = {(int)lroundf(C.x), (int)lroundf(C.y + rise), w, h};
    C.card = card;
    float radius = fminf(D->radius + 8, 20);
    if (compact)
        radius = h / 2.0f;

    /* Material: frosted, nearly solid surface, so it reads over anything. */
    color surf = D->surface | 0xff000000u;
    shadow_round(A.frame, (rect){card.x, card.y + 8, card.w, card.h}, radius, 28, RGBA(0, 0, 0, (int)(70 * alpha)));
    if (alpha > 0.6f)
        blur_fast(A.frame, card, radius, 10);
    fill_round(A.frame, card, radius, color_alpha(surf, 0.93f * alpha));
    bool dark = color_luma(surf) < 0.4f;
    stroke_round(A.frame, card, radius, 1, RGBA(255, 255, 255, (int)((dark ? 34 : 120) * alpha)));
    color ink = theme_fit(D->surface_text, surf, 7.0f);
    color accent = theme_fit(D->accent, surf, 3.0f);

    /* a trail of light back to where the companion lives */
    float ax = a.x + a.w / 2.0f, ay = a.y + a.h / 2.0f;
    float cx = fminf(fmaxf(ax, card.x + radius), card.x + card.w - radius);
    float cy = fminf(fmaxf(ay, (float)card.y), (float)(card.y + card.h));
    float dist = hypotf(ax - cx, ay - cy);
    if (dist > 16 && dist < 220) {
        fill_circle(A.frame, cx + (ax - cx) * 0.38f, cy + (ay - cy) * 0.38f, 4.5f, color_alpha(surf, 0.9f * alpha));
        fill_circle(A.frame, cx + (ax - cx) * 0.68f, cy + (ay - cy) * 0.68f, 2.8f, color_alpha(surf, 0.8f * alpha));
    }

    /* the close control */
    int cs = (int)(22 * scale);
    C.close = compact ? (rect){card.x + card.w - cs - 10, card.y + (h - cs) / 2, cs, cs}
                      : (rect){card.x + card.w - cs - 10, card.y + 10, cs, cs};
    bool over_close = pt_in(C.close, A.in.mx, A.in.my);
    if (over_close)
        fill_circle(A.frame, C.close.x + cs / 2.0f, C.close.y + cs / 2.0f, cs / 2.0f, color_alpha(ink, 0.12f * alpha));
    float k = cs * 0.2f, mx = C.close.x + cs / 2.0f, my = C.close.y + cs / 2.0f;
    color xc = color_alpha(ink, (over_close ? 0.9f : 0.5f) * alpha);
    draw_line(A.frame, mx - k, my - k, mx + k, my + k, 1.6f, xc);
    draw_line(A.frame, mx - k, my + k, mx + k, my - k, 1.6f, xc);

    canvas_clip(A.frame, card);
    if (compact) {
        ui_orb(A.frame, card.x + pad + 6.0f, card.y + h / 2.0f, 6, D->accent, D->accent2, voice_speaking());
        draw_text(A.frame, f, card.x + pad + 22, card.y + (h - lh) / 2, line, color_alpha(ink, alpha));
    } else {
        ui_orb(A.frame, card.x + pad + 6.0f, card.y + pad + head / 2.0f - 2, 6.5f, D->accent, D->accent2, voice_speaking());
        draw_text(A.frame, nf, card.x + pad + 22, card.y + pad + (head - font_line_height(nf)) / 2 - 2,
                  D->ai_name[0] ? D->ai_name : "Your companion", color_alpha(accent, alpha));
        rect tb = {card.x + pad, card.y + pad + head + 6, card.w - pad * 2, th};
        canvas_clip(A.frame, (rect){card.x, tb.y, card.w, th});
        draw_text_wrapped(A.frame, f, tb, C.text, color_alpha(ink, alpha), 1.35f);
        canvas_clip(A.frame, card);
        if (cut) /* more than fits: the conversation has the rest */
            fill_vgradient(A.frame, (rect){card.x + 1, tb.y + th - lh, card.w - 2, lh}, color_alpha(surf, 0),
                           color_alpha(surf, 0.95f * alpha));
    }
    canvas_unclip(A.frame);

    static uint64_t logged;
    if (A.now - logged > 1000) {
        logged = A.now;
        LOGRECT("caption", card);
        LOGRECT("caption-close", C.close);
    }
}
