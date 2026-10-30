#define _GNU_SOURCE
#include "settings.h"
#include "app.h"
#include "fm/fm.h"
#include "keymap.h"
#include "mind.h"
#include "models.h"
#include "tray.h"
#include "wallpaper.h"
#include "wl/wl.h"
#include "../lib/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* Settings: a sidebar of pages, drawn in the design's own type and colours.
 * Every choice takes effect at once, is written to settings.conf (or, for
 * how the desktop looks, into the design itself), and comes back at boot. */

#define D (A.d)
#define CONF STATE_DIR "/settings.conf"

/* ======================================================================
 * Kept choices
 * ==================================================================== */

static struct {
    char key[32], val[96];
} kv[48];
static int nkv;
static bool kv_loaded;

static void kv_load(void) {
    kv_loaded = true;
    nkv = 0;
    FILE *f = fopen(CONF, "r");
    char line[160];
    while (f && fgets(line, sizeof line, f) && nkv < 48) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        snprintf(kv[nkv].key, sizeof kv[nkv].key, "%s", line);
        snprintf(kv[nkv].val, sizeof kv[nkv].val, "%s", eq + 1);
        nkv++;
    }
    if (f)
        fclose(f);
}

const char *settings_get(const char *key, const char *fallback) {
    if (!kv_loaded)
        kv_load();
    for (int i = 0; i < nkv; i++)
        if (!strcmp(kv[i].key, key))
            return kv[i].val;
    return fallback;
}

int settings_get_int(const char *key, int fallback) {
    const char *v = settings_get(key, NULL);
    return v && *v ? atoi(v) : fallback;
}

void settings_set(const char *key, const char *value) {
    if (!kv_loaded)
        kv_load();
    int i = 0;
    while (i < nkv && strcmp(kv[i].key, key))
        i++;
    if (i == nkv) {
        if (nkv == 48)
            return;
        nkv++;
        snprintf(kv[i].key, sizeof kv[i].key, "%s", key);
    } else if (!strcmp(kv[i].val, value)) {
        return;
    }
    snprintf(kv[i].val, sizeof kv[i].val, "%s", value);
    /* written whole each time: a handful of short lines */
    FILE *f = fopen(CONF ".new", "w");
    if (!f)
        return;
    fputs("# Settings, as chosen in the Settings app.\n", f);
    for (int k = 0; k < nkv; k++)
        fprintf(f, "%s=%s\n", kv[k].key, kv[k].val);
    fclose(f);
    rename(CONF ".new", CONF);
}

void settings_set_int(const char *key, int value) {
    char b[24];
    snprintf(b, sizeof b, "%d", value);
    settings_set(key, b);
}

/* ======================================================================
 * Light and dark belong to the theme (theme.c); until it is linked in,
 * the switch says the design decides.
 * ==================================================================== */

void theme_set_dark(bool dark) __attribute__((weak));
bool theme_is_dark(void) __attribute__((weak));

bool appearance_can_switch(void) { return theme_set_dark != NULL; }

bool appearance_dark(void) {
    if (theme_is_dark)
        return theme_is_dark();
    return color_luma(D.surface) < 0.4f;
}

void appearance_set_dark(bool dark) {
    if (theme_set_dark)
        theme_set_dark(dark);
}

/* ======================================================================
 * The screen
 * ==================================================================== */

bool shell_set_resolution(int w, int h) {
    if (w == A.W && h == A.H)
        return true;
    int ow = A.W, oh = A.H;
    if (!display_set_mode(w, h))
        return false;
    A.W = display_width();
    A.H = display_height();
    canvas *f = canvas_new(A.W, A.H);
    if (f) {
        canvas_free(A.frame);
        A.frame = f;
    }
    if (A.wall) {
        canvas_free(A.wall);
        A.wall = canvas_new(A.W, A.H);
        wallpaper_paint(A.wall, &D);
    }
    input_set_screen(A.W, A.H);
    wls_set_screen(A.W, A.H);
    A.in.mx = A.in.mx * A.W / (ow ? ow : 1);
    A.in.my = A.in.my * A.H / (oh ? oh : 1);
    tray_close();
    /* Lay the design out at the new size now, so windows can be fitted to
     * the space it leaves rather than to last frame's. */
    rect wk = {0, 0, A.W, A.H};
    if (A.ui) {
        fm_layout(A.ui, wk);
        if (A.ui->work.w > 200 && A.ui->work.h > 200)
            wk = A.ui->work;
    }
    for (int i = 0; i < WIN_MAX; i++) {
        struct window *win = &A.win[i];
        if (!win->open)
            continue;
        if (win->maximized) {
            win->r = rect_inset(wk, 12);
            continue;
        }
        /* same place in proportion, never larger than the screen, never off it */
        if (win->r.w > wk.w - 24) win->r.w = wk.w - 24;
        if (win->r.h > wk.h - 24) win->r.h = wk.h - 24;
        win->r.x = win->r.x * A.W / (ow ? ow : 1);
        win->r.y = win->r.y * A.H / (oh ? oh : 1);
        if (win->r.x + win->r.w > wk.x + wk.w - 8) win->r.x = wk.x + wk.w - 8 - win->r.w;
        if (win->r.y + win->r.h > wk.y + wk.h - 8) win->r.y = wk.y + wk.h - 8 - win->r.h;
        if (win->r.x < wk.x + 8) win->r.x = wk.x + 8;
        if (win->r.y < wk.y + 8) win->r.y = wk.y + 8;
    }
    LOG("display %dx%d\n", A.W, A.H);
    return true;
}

static const struct mode {
    int w, h;
    const char *name;
} modes[] = {{1024, 768, "XGA"}, {1280, 720, "HD"}, {1600, 900, "HD+"}, {1920, 1080, "Full HD"}};
#define NMODES 4

static struct {
    bool pending;
    int ow, oh;
    uint64_t at;
} trial;

#define TRIAL_MS 15000

static void trial_revert(bool timed_out) {
    trial.pending = false;
    shell_set_resolution(trial.ow, trial.oh);
    if (timed_out) {
        char b[120];
        snprintf(b, sizeof b, "Back to %d × %d: the new resolution wasn't kept.", trial.ow, trial.oh);
        notify("Display", b, "display");
    }
}

static void trial_keep(void) {
    trial.pending = false;
    char b[24];
    snprintf(b, sizeof b, "%dx%d", A.W, A.H);
    settings_set("display", b);
}

void settings_tick(void) {
    if (trial.pending && A.now - trial.at > TRIAL_MS)
        trial_revert(true);
}

/* ======================================================================
 * Boot
 * ==================================================================== */

void settings_boot(void) {
    kv_load(); /* the disk has been read by now */
    const char *disp = settings_get("display", NULL);
    /* The VM launcher says how much room there is: "WxH" in full screen (the
     * screen itself: that wins), "max WxH" in a window (what fits on the
     * person's screen: their own choice if it fits, else the largest that does). */
    static char host[32], fit[24];
    FILE *hf = fopen("/etc/os/display", "r");
    int mw, mh;
    if (hf) {
        if (fgets(host, sizeof host, hf)) {
            if (host[0] >= '0' && host[0] <= '9') {
                disp = host;
            } else if (sscanf(host, "max %dx%d", &mw, &mh) == 2) {
                int sw, sh;
                bool fits = disp && sscanf(disp, "%dx%d", &sw, &sh) == 2 && sw <= mw && sh <= mh;
                if (!fits) {
                    int best = -1;
                    for (int i = 0; i < NMODES; i++)
                        if (modes[i].w <= mw && modes[i].h <= mh)
                            best = i;
                    if (best >= 0) {
                        snprintf(fit, sizeof fit, "%dx%d", modes[best].w, modes[best].h);
                        disp = fit;
                    }
                }
            }
        }
        fclose(hf);
    }
    int w, h;
    if (disp && sscanf(disp, "%dx%d", &w, &h) == 2 && (w != A.W || h != A.H)) {
        if (display_mode_possible(w, h) && shell_set_resolution(w, h))
            LOG("settings: display %dx%d\n", w, h);
        else
            LOG("settings: display %s is not possible here\n", disp);
    }
    const char *kb = settings_get("keyboard", NULL);
    if (kb && keymap_set(kb))
        LOG("settings: keyboard %s\n", keymap_current());
    input_set_repeat(settings_get_int("repeat_delay", 500), settings_get_int("repeat_rate", 30));
    if (settings_get("volume", NULL))
        sound_restore();
}

/* ======================================================================
 * Pages
 * ==================================================================== */

static palette P; /* this frame's colours for the content area */
/* Where things are goes to the log now and then, for tests (tests/settings_test.py). */
static bool log_now;

const palette *settings_palette(void) { return &P; }

font *settings_font(int role) {
    switch (role) {
    case SF_SMALL: return fm_host_font(D.font_body, 13);
    case SF_SECTION: return fm_host_font(D.font_heading, 16);
    case SF_TITLE: return fm_host_font(D.font_heading, 28);
    default: return fm_host_font(D.font_body, 15);
    }
}

static font *BODY(void) { return settings_font(SF_BODY); }
static font *SMALL(void) { return settings_font(SF_SMALL); }
static int LH(font *f) { return font_line_height(f); }

int settings_section(rect c, int y, const char *title) {
    font *f = settings_font(SF_SECTION);
    draw_text(A.frame, f, c.x, y, title, P.ink);
    return y + LH(f) + 12;
}

/* A line of quieter text, wrapped; returns the height used. */
static int note_text(rect c, int y, const char *s) {
    return draw_text_wrapped(A.frame, SMALL(), (rect){c.x, y, c.w, 400}, s, P.muted, 1.35f);
}

/* A row with a label on the left and something on the right. */
static void row_label(rect c, int y, int h, const char *label, const char *sub) {
    font *f = BODY();
    if (sub) {
        draw_text(A.frame, f, c.x, y + h / 2 - LH(f) + 2, label, P.ink);
        draw_text(A.frame, SMALL(), c.x, y + h / 2 + 2, sub, P.muted);
    } else {
        draw_text(A.frame, f, c.x, y + (h - LH(f)) / 2, label, P.ink);
    }
}

/* ---- Appearance ---- */

static canvas *thumbs[WP_COUNT];
static uint32_t thumbs_key;

static uint32_t wp_key(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++)
        h = (h ^ D.wp[i]) * 16777619u;
    return (h ^ (uint32_t)D.wp_seed) * 16777619u ^ (uint32_t)(D.wp_intensity * 100);
}

static fm_node *wallpaper_node(void) {
    if (!A.ui || !A.ui->root)
        return NULL;
    for (fm_node *n = A.ui->root->first; n; n = n->next)
        if (n->kind == FM_ELEM && !strcasecmp(n->tag, "wallpaper"))
            return n;
    fm_node *n = fm_node_new(A.ui, "wallpaper");
    fm_append(A.ui->root, n);
    return n;
}

static void keep_look(void) {
    if (A.ui)
        A.ui->changed = true; /* the desktop saves the document */
    design_save(&A.d, STATE_DIR "/design.json");
}

static void apply_wallpaper(int kind, const color *wp) {
    D.wallpaper = (enum wallpaper_kind)kind;
    if (wp)
        for (int i = 0; i < 4; i++)
            D.wp[i] = wp[i] | 0xff000000u;
    if (A.wall)
        wallpaper_paint(A.wall, &D);
    fm_node *n = wallpaper_node();
    if (n) {
        char cs[64];
        snprintf(cs, sizeof cs, "#%06x #%06x #%06x #%06x", D.wp[0] & 0xffffff, D.wp[1] & 0xffffff, D.wp[2] & 0xffffff,
                 D.wp[3] & 0xffffff);
        fm_attr_set(n, "kind", wallpaper_name(kind));
        fm_attr_set(n, "colors", cs);
    }
    keep_look();
}

/* Replaces the stylesheet line Settings wrote before (marked), so choices
 * don't pile up in the document. */
static void css_setting(const char *mark, const char *css) {
    if (!A.ui)
        return;
    char tag[48];
    snprintf(tag, sizeof tag, "/* settings: %s */", mark);
    if (A.ui->css) {
        char *m = strstr(A.ui->css, tag);
        if (m) {
            char *start = m;
            while (start > A.ui->css && start[-1] != '\n')
                start--;
            char *end = m + strlen(tag);
            memmove(start, end, strlen(end) + 1);
        }
    }
    char line[256];
    snprintf(line, sizeof line, "%s %s", css, tag);
    fm_css_patch(A.ui, line);
}

static void apply_accent(color c) {
    D.accent = c | 0xff000000u;
    char css[96];
    snprintf(css, sizeof css, ":root { --accent: #%06x; }", c & 0xffffff);
    css_setting("accent", css);
    keep_look();
}

static const struct {
    const char *name;
    color c[4];
} wp_sets[] = {
    {"Dusk", {RGBA(27, 20, 48, 255), RGBA(226, 114, 91, 255), RGBA(246, 194, 139, 255), RGBA(13, 10, 26, 255)}},
    {"Sea", {RGBA(11, 42, 60, 255), RGBA(42, 157, 143, 255), RGBA(168, 218, 220, 255), RGBA(6, 25, 35, 255)}},
    {"Forest", {RGBA(19, 38, 27, 255), RGBA(90, 143, 92, 255), RGBA(201, 217, 166, 255), RGBA(10, 20, 14, 255)}},
    {"Ember", {RGBA(42, 15, 10, 255), RGBA(209, 73, 91, 255), RGBA(237, 174, 73, 255), RGBA(18, 6, 10, 255)}},
    {"Paper", {RGBA(239, 233, 223, 255), RGBA(216, 203, 184, 255), RGBA(180, 154, 122, 255), RGBA(207, 196, 179, 255)}},
    {"Night", {RGBA(7, 11, 24, 255), RGBA(58, 74, 140, 255), RGBA(157, 180, 255, 255), RGBA(2, 4, 10, 255)}},
};
#define NSETS 6

static const color accents[] = {RGBA(232, 131, 107, 255), RGBA(242, 166, 90, 255), RGBA(107, 191, 138, 255),
                                RGBA(63, 167, 214, 255),  RGBA(123, 108, 246, 255), RGBA(224, 93, 158, 255),
                                RGBA(60, 60, 68, 255)};
#define NACCENTS 7

/* The design's own colours, remembered the first time they are changed so
 * there is always a way back. */
static void remember_design(void) {
    char b[64];
    if (!settings_get("design_wp", NULL)) {
        snprintf(b, sizeof b, "%d %06x %06x %06x %06x", D.wallpaper, D.wp[0] & 0xffffff, D.wp[1] & 0xffffff,
                 D.wp[2] & 0xffffff, D.wp[3] & 0xffffff);
        settings_set("design_wp", b);
    }
    if (!settings_get("design_accent", NULL)) {
        snprintf(b, sizeof b, "%06x", D.accent & 0xffffff);
        settings_set("design_accent", b);
    }
}

static bool design_wp(int *kind, color c[4]) {
    const char *s = settings_get("design_wp", NULL);
    unsigned a[4];
    if (!s || sscanf(s, "%d %x %x %x %x", kind, &a[0], &a[1], &a[2], &a[3]) != 5)
        return false;
    for (int i = 0; i < 4; i++)
        c[i] = a[i] | 0xff000000u;
    return true;
}

static void swatch_set(rect r, const color c[4], bool sel, const char *name) {
    bool hover = ui_hover(r);
    if (sel || hover)
        stroke_round(A.frame, rect_inset(r, -3), r.h / 2.0f + 3, sel ? 2 : 1, sel ? P.accent : color_alpha(P.ink, 0.3f));
    fill_round_vgradient(A.frame, r, r.h / 2.0f, c[1], c[0]);
    for (int i = 0; i < 4; i++)
        fill_circle(A.frame, r.x + r.h / 2.0f + i * (r.h * 0.42f), r.y + r.h / 2.0f, r.h * 0.2f, c[i]);
    draw_text(A.frame, SMALL(), r.x + r.h / 2 + (int)(4 * r.h * 0.42f) + 4, r.y + (r.h - LH(SMALL())) / 2, name,
              readable_on(color_mix(c[0], c[1], 0.5f)));
}

static int page_appearance(rect c, int y) {
    int y0 = y;
    y = settings_section(c, y, "Wallpaper");
    /* living thumbnails: painted once per colour set, moving under the pointer */
    if (thumbs_key != wp_key()) {
        thumbs_key = wp_key();
        for (int k = 0; k < WP_COUNT; k++) {
            if (!thumbs[k])
                thumbs[k] = canvas_new(152, 92);
            design t = D;
            t.wallpaper = (enum wallpaper_kind)k;
            if (thumbs[k])
                wallpaper_paint(thumbs[k], &t);
        }
    }
    int tw = 152, th = 92, gap = 14, cols = (c.w + gap) / (tw + gap);
    if (cols < 1)
        cols = 1;
    if (cols > 6)
        cols = 6;
    tw = (c.w - gap * (cols - 1)) / cols;
    th = tw * 92 / 152;
    font *sf = SMALL();
    for (int k = 0; k < WP_COUNT; k++) {
        int col = k % cols, row = k / cols;
        rect t = {c.x + col * (tw + gap), y + row * (th + LH(sf) + 18), tw, th};
        bool sel = (int)D.wallpaper == k, hover = ui_hover(t);
        float rad = 12;
        if (thumbs[k]) {
            design dd = D;
            dd.wallpaper = (enum wallpaper_kind)k;
            if (hover && !A.prof.reduce_motion && wallpaper_animated(&dd)) {
                canvas live = {thumbs[k]->w, thumbs[k]->h, thumbs[k]->px, 0, 0, thumbs[k]->w, thumbs[k]->h};
                wallpaper_frame(&live, &dd, (float)A.now / 1000.0f);
            }
            /* scaled into place with rounded corners */
            int ox0 = A.frame->cx0, oy0 = A.frame->cy0, ox1 = A.frame->cx1, oy1 = A.frame->cy1;
            for (int j = 0; j < th; j++) {
                int dy = t.y + j;
                if (dy < oy0 || dy >= oy1)
                    continue;
                const uint32_t *srow = &thumbs[k]->px[(size_t)(j * thumbs[k]->h / th) * (size_t)thumbs[k]->w];
                uint32_t *drow = &A.frame->px[(size_t)dy * (size_t)A.frame->w];
                /* round the corners by skipping pixels outside the radius */
                for (int i = 0; i < tw; i++) {
                    int dx = t.x + i;
                    if (dx < ox0 || dx >= ox1)
                        continue;
                    float cx = i < rad ? rad - i - 0.5f : i >= tw - rad ? i - (tw - rad) + 0.5f : 0;
                    float cy = j < rad ? rad - j - 0.5f : j >= th - rad ? j - (th - rad) + 0.5f : 0;
                    if (cx > 0 && cy > 0 && cx * cx + cy * cy > rad * rad)
                        continue;
                    drow[dx] = srow[i * thumbs[k]->w / tw] | 0xff000000u;
                }
            }
        }
        if (sel) {
            stroke_round(A.frame, rect_inset(t, -3), rad + 3, 2.5f, P.accent);
            fill_circle(A.frame, t.x + t.w - 16.0f, t.y + 16.0f, 11, P.accent);
            ctl_symbol(A.frame, (rect){t.x + t.w - 26, t.y + 6, 20, 20}, SYM_CHECK, 1, P.on_accent);
        } else if (hover) {
            stroke_round(A.frame, rect_inset(t, -2), rad + 2, 1.5f, color_alpha(P.ink, 0.4f));
        }
        char name[32];
        snprintf(name, sizeof name, "%s", wallpaper_name(k));
        if (name[0] >= 'a' && name[0] <= 'z')
            name[0] = (char)(name[0] - 32);
        draw_text(A.frame, sf, t.x + 2, t.y + th + 6, name, sel ? P.ink : P.muted);
        if (hover && A.in.clicked && !sel) {
            remember_design();
            apply_wallpaper(k, NULL);
        }
    }
    y += ((WP_COUNT + cols - 1) / cols) * (th + LH(sf) + 18) + 8;

    y = settings_section(c, y, "Colours");
    int sw = 150, sh = 34, x = c.x;
    color own[4];
    int own_kind;
    bool has_own = design_wp(&own_kind, own);
    if (!has_own)
        memcpy(own, D.wp, sizeof own);
    for (int i = -1; i < NSETS; i++) {
        const color *cs = i < 0 ? own : wp_sets[i].c;
        if (x + sw > c.x + c.w) {
            x = c.x;
            y += sh + 14;
        }
        rect r = {x, y, sw, sh};
        bool sel = !memcmp(cs, D.wp, sizeof own);
        swatch_set(r, cs, sel, i < 0 ? "Designed" : wp_sets[i].name);
        if (ui_hover(r) && A.in.clicked && !sel) {
            remember_design();
            apply_wallpaper(D.wallpaper, cs);
        }
        x += sw + 14;
    }
    y += sh + 26;

    y = settings_section(c, y, "Accent");
    unsigned own_acc = 0;
    const char *da = settings_get("design_accent", NULL);
    color designed = da && sscanf(da, "%x", &own_acc) == 1 ? own_acc | 0xff000000u : D.accent | 0xff000000u;
    x = c.x;
    for (int i = -1; i < NACCENTS; i++) {
        color col = i < 0 ? designed : accents[i];
        float cx = x + 17.0f, cy = y + 17.0f;
        rect hit = {x, y, 34, 34};
        bool sel = (D.accent | 0xff000000u) == (col | 0xff000000u), hover = ui_hover(hit);
        if (sel || hover)
            draw_arc(A.frame, cx, cy, 18, 0, 6.2832f, sel ? 2.5f : 1.2f, sel ? P.ink : color_alpha(P.ink, 0.4f));
        fill_circle(A.frame, cx, cy, 13, col);
        if (sel)
            ctl_symbol(A.frame, (rect){x + 8, y + 8, 18, 18}, SYM_CHECK, 1, readable_on(col));
        if (hover && A.in.clicked && !sel) {
            remember_design();
            apply_accent(col);
        }
        x += 46;
    }
    draw_text(A.frame, SMALL(), x + 8, y + (34 - LH(SMALL())) / 2, "The first is the one your design came with.", P.muted);
    y += 34 + 26;

    y = settings_section(c, y, "Mode");
    static const char *const modes_l[] = {"Light", "Dark"};
    bool dark = appearance_dark();
    int pick = ctl_segmented((rect){c.x, y, 240, 38}, modes_l, 2, dark ? 1 : 0, BODY(), &P);
    if (pick != (dark ? 1 : 0) && appearance_can_switch())
        appearance_set_dark(pick == 1);
    if (!appearance_can_switch())
        draw_text(A.frame, SMALL(), c.x + 256, y + (38 - LH(SMALL())) / 2, "This design sets its own light.", P.muted);
    y += 38 + 26;

    y = settings_section(c, y, "Text size");
    static const char *const sizes[] = {"Small", "Default", "Large", "Larger"};
    static const float scales[] = {0.9f, 1.0f, 1.15f, 1.3f};
    float ts = A.prof.text_scale > 0.5f ? A.prof.text_scale : 1.0f;
    int cur = 1;
    for (int i = 0; i < 4; i++)
        if (fabsf(ts - scales[i]) < 0.04f)
            cur = i;
    int np = ctl_segmented((rect){c.x, y, 420, 38}, sizes, 4, cur, BODY(), &P);
    if (np != cur) {
        A.prof.text_scale = scales[np];
        portrait_save(&A.prof, STATE_DIR "/portrait.txt");
        if (A.ui)
            A.ui->changed = true;
    }
    y += 38 + 26;

    y = settings_section(c, y, "Comfort");
    struct {
        const char *label, *sub;
        bool on;
    } rows[2] = {{"Strong contrast", "Text reads at 7:1 against whatever is behind it", A.prof.high_contrast},
                 {"Less movement", "Still wallpaper, no glides or swells", A.prof.reduce_motion}};
    for (int i = 0; i < 2; i++) {
        row_label(c, y, 48, rows[i].label, rows[i].sub);
        if (ctl_switch((rect){c.x + c.w - 52, y + 12, 48, 26}, rows[i].on, &P)) {
            if (i == 0) {
                A.prof.high_contrast = !A.prof.high_contrast;
                fm_contrast_floor = A.prof.high_contrast ? 7.0f : 4.5f;
            } else {
                A.prof.reduce_motion = !A.prof.reduce_motion;
                D.motion = A.prof.reduce_motion ? 0 : 0.6f;
            }
            portrait_save(&A.prof, STATE_DIR "/portrait.txt");
            if (A.ui)
                A.ui->changed = true;
        }
        y += 56;
    }
    y += 10;
    char line[120];
    snprintf(line, sizeof line, "Ask %s for a whole new look", D.ai_name[0] ? D.ai_name : "your companion");
    int bw = text_width(BODY(), line) + 40;
    if (ctl_button((rect){c.x, y, bw, 40}, line, BODY(), true, !A.redesigning, &P))
        design_begin("Give the whole space a fresh take, still true to who they are.");
    y += 40 + 10;
    return y - y0;
}

/* ---- Display ---- */

static int page_display(rect c, int y) {
    int y0 = y;
    static int possible[NMODES]; /* 0 unknown, 1 yes, -1 no */
    if (trial.pending) {
        rect b = {c.x, y, c.w, 64};
        fill_round(A.frame, b, 16, color_alpha(P.accent, 0.16f));
        stroke_round(A.frame, b, 16, 1, color_alpha(P.accent, 0.5f));
        int left = (int)((TRIAL_MS - (A.now - trial.at) + 999) / 1000);
        char msg[96];
        snprintf(msg, sizeof msg, "Keep this resolution? Going back in %d s.", left < 0 ? 0 : left);
        draw_text(A.frame, BODY(), b.x + 18, b.y + (b.h - LH(BODY())) / 2, msg, P.ink);
        rect keep = {b.x + b.w - 110, b.y + 13, 96, 38}, back = {keep.x - 108, b.y + 13, 96, 38};
        if (log_now)
            LOGRECT("settings-keep", keep);
        if (ctl_button(back, "Go back", BODY(), false, true, &P))
            trial_revert(false);
        else if (ctl_button(keep, "Keep", BODY(), true, true, &P))
            trial_keep();
        y += 64 + 24;
    }
    y = settings_section(c, y, "Resolution");
    int gap = 14, cw = (c.w - gap * 3) / 4, ch = cw * 3 / 4 + 44;
    if (cw < 120) { /* narrow: two by two */
        cw = (c.w - gap) / 2;
        ch = cw * 3 / 5 + 44;
    }
    int per_row = (c.w + gap) / (cw + gap);
    for (int i = 0; i < NMODES; i++) {
        const struct mode *m = &modes[i];
        if (!possible[i])
            possible[i] = display_mode_possible(m->w, m->h) ? 1 : -1;
        rect r = {c.x + (i % per_row) * (cw + gap), y + (i / per_row) * (ch + gap), cw, ch};
        bool cur = m->w == A.W && m->h == A.H, ok = possible[i] > 0 || cur, hover = ok && ui_hover(r);
        fill_round(A.frame, r, 16, color_alpha(P.ink, cur ? 0.1f : hover ? 0.08f : 0.04f));
        if (cur)
            stroke_round(A.frame, r, 16, 2, P.accent);
        /* the screen's shape, to scale */
        float maxw = cw - 40.0f, maxh = ch - 64.0f, sc = fminf(maxw / 1920.0f, maxh / 1080.0f);
        int sw = (int)(m->w * sc), sh = (int)(m->h * sc);
        rect scr = {r.x + (r.w - sw) / 2, r.y + 14 + ((int)maxh - sh) / 2, sw, sh};
        fill_round_vgradient(A.frame, scr, 5, color_alpha(cur ? P.accent : P.ink, ok ? 0.5f : 0.15f),
                             color_alpha(cur ? P.accent : P.ink, ok ? 0.25f : 0.08f));
        char label[32];
        snprintf(label, sizeof label, "%d × %d", m->w, m->h);
        font *f = BODY();
        draw_text_centered(A.frame, f, (rect){r.x, r.y + r.h - 46, r.w, 22}, label, ok ? P.ink : P.muted);
        draw_text_centered(A.frame, SMALL(), (rect){r.x, r.y + r.h - 24, r.w, 18}, cur ? "Now" : ok ? m->name : "Not here",
                           cur ? P.accent : P.muted);
        char nm[32];
        snprintf(nm, sizeof nm, "settings-mode-%dx%d", m->w, m->h);
        if (log_now)
            LOGRECT(nm, r);
        if (hover && A.in.clicked && !cur) {
            int ow = A.W, oh = A.H;
            if (shell_set_resolution(m->w, m->h)) {
                if (!trial.pending)
                    trial.ow = ow, trial.oh = oh;
                trial.pending = true;
                trial.at = A.now;
                return 0; /* everything moved; draw again next frame */
            }
            toast("The screen couldn't switch to %d × %d.", m->w, m->h);
        }
    }
    y += ((NMODES + per_row - 1) / per_row) * (ch + gap) + 12;
    bool any = false;
    for (int i = 0; i < NMODES; i++)
        any |= possible[i] > 0 && !(modes[i].w == A.W && modes[i].h == A.H);
    y += note_text(c, y, any ? "A new resolution is tried first: keep it, or it goes back by itself after 15 seconds. "
                               "The one you keep is used every time the computer starts."
                             : "This graphics card keeps the resolution the computer started with.");
    return y - y0 + 10;
}

/* ---- Sound ---- */

static int page_sound(rect c, int y) {
    int y0 = y;
    bool on = sound_available();
    y = settings_section(c, y, "Volume");
    rect mb = {c.x, y, 44, 44};
    if (ctl_tile(mb, sound_muted(), &P) && on)
        sound_set_muted(!sound_muted());
    ctl_symbol(A.frame, rect_inset(mb, 10), sound_muted() ? SYM_SPEAKER_MUTED : SYM_SPEAKER, sound_volume(),
               sound_muted() ? P.on_accent : P.ink);
    float v = sound_muted() ? 0 : sound_volume();
    rect sl = {c.x + 60, y + 11, c.w - 60 - 70, 22};
    int res = ctl_slider(2001, sl, &v, &P);
    if (log_now)
        LOGRECT("settings-volume", sl);
    if (on && res == CTL_CHANGING)
        sound_set_volume(v, false);
    else if (on && res == CTL_RELEASED)
        sound_set_volume(v, true);
    char pct[16];
    snprintf(pct, sizeof pct, sound_muted() ? "Muted" : "%d%%", (int)lroundf(sound_volume() * 100));
    draw_text(A.frame, BODY(), c.x + c.w - text_width(BODY(), pct), y + (44 - LH(BODY())) / 2, pct, P.muted);
    y += 44 + 20;
    row_label(c, y, 48, "Mute", "Silence everything for now");
    if (ctl_switch((rect){c.x + c.w - 52, y + 12, 48, 26}, sound_muted(), &P) && on)
        sound_set_muted(!sound_muted());
    y += 64;
    rect tb = {c.x, y, 200, 40};
    if (ctl_button(tb, "Play a test sound", BODY(), true, on, &P))
        sound_test();
    if (log_now)
        LOGRECT("settings-test-sound", tb);
    y += 40 + 26;
    y = settings_section(c, y, "Output");
    y += note_text(c, y, on ? "Built-in audio (Intel High Definition Audio)." : "No sound card was found on this computer.");
    return y - y0 + 10;
}

/* ---- Keyboard ---- */

static struct textfield kb_test;

static int page_keyboard(rect c, int y) {
    int y0 = y;
    y = settings_section(c, y, "Layout");
    int n = keymap_count(), colw = (c.w - 12) / 2;
    for (int i = 0; i < n; i++) {
        rect r = {c.x + (i % 2) * (colw + 12), y + (i / 2) * 50, colw, 44};
        bool sel = !strcmp(keymap_id(i), keymap_current()), hover = ui_hover(r);
        fill_round(A.frame, r, 14, sel ? color_alpha(P.accent, 0.18f) : color_alpha(P.ink, hover ? 0.08f : 0.04f));
        if (sel)
            stroke_round(A.frame, r, 14, 1.5f, P.accent);
        draw_text(A.frame, BODY(), r.x + 16, r.y + (r.h - LH(BODY())) / 2, keymap_name(i), P.ink);
        char code[8];
        snprintf(code, sizeof code, "%s", keymap_id(i));
        for (char *p = code; *p; p++)
            *p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        draw_text(A.frame, SMALL(), r.x + r.w - 16 - text_width(SMALL(), code), r.y + (r.h - LH(SMALL())) / 2, code,
                  sel ? P.accent : P.muted);
        char nm[32];
        snprintf(nm, sizeof nm, "settings-kb-%s", keymap_id(i));
        if (log_now)
            LOGRECT(nm, r);
        if (hover && A.in.clicked && !sel) {
            keymap_set(keymap_id(i));
            settings_set("keyboard", keymap_id(i));
        }
    }
    y += ((n + 1) / 2) * 50 + 14;
    rect tf = {c.x, y, c.w, 46};
    ui_textfield(tf, &kb_test, true, "Type here to try the layout", BODY(), color_alpha(P.ink, 0.06f), P.ink, P.muted, 14);
    if (log_now)
        LOGRECT("settings-kb-test", tf);
    if (ui_type(&kb_test)) {
        kb_test.len = 0;
        kb_test.buf[0] = 0;
    }
    y += 46 + 8;
    y += note_text(c, y, "Right Alt (AltGr) types the third symbol on a key. Accent keys wait for the next letter.");
    y += 22;
    y = settings_section(c, y, "Repeat");
    int delay = settings_get_int("repeat_delay", 500), rate = settings_get_int("repeat_rate", 30);
    char val[32];
    row_label(c, y, 40, "Delay before repeating", NULL);
    snprintf(val, sizeof val, "%d ms", delay);
    draw_text(A.frame, BODY(), c.x + c.w - text_width(BODY(), val), y + (40 - LH(BODY())) / 2, val, P.muted);
    float t = (delay - 150) / 850.0f;
    int r1 = ctl_slider(2101, (rect){c.x + 230, y + 9, c.w - 230 - 90, 22}, &t, &P);
    if (r1 != CTL_IDLE) {
        delay = 150 + (int)lroundf(t * 850 / 10) * 10;
        settings_set_int("repeat_delay", delay);
        input_set_repeat(delay, rate);
    }
    y += 50;
    row_label(c, y, 40, "Repeat speed", NULL);
    snprintf(val, sizeof val, "%d a second", rate);
    draw_text(A.frame, BODY(), c.x + c.w - text_width(BODY(), val), y + (40 - LH(BODY())) / 2, val, P.muted);
    t = (rate - 2) / 58.0f;
    int r2 = ctl_slider(2102, (rect){c.x + 230, y + 9, c.w - 230 - 90, 22}, &t, &P);
    if (r2 != CTL_IDLE) {
        rate = 2 + (int)lroundf(t * 58);
        settings_set_int("repeat_rate", rate);
        input_set_repeat(delay, rate);
    }
    y += 50;
    return y - y0;
}

/* ---- Mind ---- */

enum { MIND_SHOW, MIND_CHOOSE, MIND_CONNECT, MIND_MODELS };
static struct {
    int step;
    const struct ai_provider *p;
    struct textfield url, key;
    int focus;
    job *job;
    char status[300];
    char **models;
    int n, sel;
    bool failed;
} mind;
static struct ai_config mind_cfg; /* read by the child after fork */

/* The same check onboarding makes: list the models this address and key
 * can use (and wait for the built-in mind to wake). */
static void mind_child(void *arg, char **result) {
    (void)arg;
    char **ids, err[512];
    int n = ai_list_models(&mind_cfg, &ids, err, sizeof err);
    for (int tries = 0; n < 0 && !strcmp(mind_cfg.provider, "builtin") && tries < 1200 && (tries < 10 || mind_builtin_alive());
         tries++) {
        sleep(1);
        n = ai_list_models(&mind_cfg, &ids, err, sizeof err);
    }
    sbuf out;
    sb_init(&out);
    if (n < 0) {
        sb_add(&out, "ERR\n");
        sb_add(&out, err);
    } else {
        sb_add(&out, "OK\n");
        for (int i = 0; i < n; i++) {
            sb_add(&out, ids[i]);
            sb_add(&out, "\n");
        }
    }
    *result = out.p;
}

static void mind_reset(void) {
    if (mind.models)
        ai_free_models(mind.models, mind.n);
    mind.models = NULL;
    mind.n = mind.sel = 0;
    mind.status[0] = 0;
    mind.failed = false;
}

static void mind_pick(const struct ai_provider *p) {
    mind_reset();
    mind.p = p;
    mind.step = MIND_CONNECT;
    snprintf(mind.url.buf, sizeof mind.url.buf, "%s", p->base_url ? p->base_url : "");
    mind.url.len = (int)strlen(mind.url.buf);
    /* the same service again keeps its key */
    if (A.provider == p)
        snprintf(mind.key.buf, sizeof mind.key.buf, "%s", A.cfg.api_key);
    else
        mind.key.buf[0] = 0;
    mind.key.len = (int)strlen(mind.key.buf);
    mind.key.secret = true;
    mind.focus = p->url_editable ? 0 : 1;
    if (!strcmp(p->id, "builtin"))
        mind.step = MIND_MODELS; /* which model: the built-in one, or one to download */
}

static void mind_use(const char *model) {
    struct ai_config c;
    memset(&c, 0, sizeof c);
    snprintf(c.provider, sizeof c.provider, "%s", mind.p->id);
    snprintf(c.base_url, sizeof c.base_url, "%s", mind.p->url_editable ? mind.url.buf : mind.p->base_url ? mind.p->base_url : "");
    snprintf(c.api_key, sizeof c.api_key, "%s", mind.key.buf);
    snprintf(c.model, sizeof c.model, "%s", model && *model ? model : mind.p->default_model ? mind.p->default_model : "");
    A.cfg = c;
    A.provider = mind.p;
    ai_config_save(&A.cfg);
    LOG("mind %s (%s), chosen in Settings\n", A.provider->id, A.cfg.model);
    char b[200];
    snprintf(b, sizeof b, "%s now thinks with %s%s%s.", D.ai_name[0] ? D.ai_name : "Your companion", mind.p->name,
             A.cfg.model[0] ? ", " : "", A.cfg.model);
    notify("Mind changed", b, "settings");
    mind_reset();
    mind.step = MIND_SHOW;
}

static bool is_local(void) {
    return A.provider && (!strcmp(A.provider->id, "ollama") || !strcmp(A.provider->id, "builtin") ||
                          !strcmp(A.provider->id, "claude-pc") || strstr(A.cfg.base_url, "10.0.2.2") ||
                          strstr(A.cfg.base_url, "localhost") || strstr(A.cfg.base_url, "127.0.0.1"));
}

static int mind_card(rect c, int y, const struct ai_provider *p, int w, int x) {
    rect r = {x, y, w, 70};
    bool ok = p->available, hover = ok && ui_hover(r), cur = A.provider == p;
    fill_round(A.frame, r, 14, color_alpha(P.ink, hover ? 0.1f : 0.05f));
    if (cur)
        stroke_round(A.frame, r, 14, 1.5f, P.accent);
    ctl_text_fit(A.frame, BODY(), r.x + 14, r.y + 12, r.w - 90, p->name, ok ? P.ink : P.muted);
    ctl_text_fit(A.frame, SMALL(), r.x + 14, r.y + 14 + LH(BODY()), r.w - 28, ok ? p->blurb : p->unavailable_reason ? p->unavailable_reason : p->blurb, P.muted);
    const char *tag = cur ? "Now" : !p->needs_key ? "No key" : NULL;
    if (tag)
        draw_text(A.frame, SMALL(), r.x + r.w - 14 - text_width(SMALL(), tag), r.y + 13, tag, cur ? P.accent : P.muted);
    char nm[48];
    snprintf(nm, sizeof nm, "settings-mind-%s", p->id);
    if (log_now)
        LOGRECT(nm, r);
    (void)c;
    if (hover && A.in.clicked)
        mind_pick(p);
    return 70;
}

/* A model on this computer was chosen: the mind is the one here, thinking with it. */
static void settings_use_model(int i) {
    (void)i;
    const struct ai_provider *p = ai_find_provider("builtin");
    if (!p)
        return;
    mind.p = p;
    mind_builtin_start();
    mind_use(NULL);
}

void companion_rename(const char *raw); /* desktop.c */

/* their companion's name: chosen in onboarding, theirs to change any time */
static void name_row(rect c, int *y, font *f) {
    static struct textfield nm;
    static char shown[64];
    if (strcmp(shown, D.ai_name)) { /* renamed elsewhere (or first time here): start from the name it has */
        snprintf(shown, sizeof shown, "%s", D.ai_name);
        snprintf(nm.buf, sizeof nm.buf, "%s", D.ai_name);
        nm.len = (int)strlen(nm.buf);
    }
    draw_text(A.frame, f, c.x, *y, "Name", P.muted);
    *y += LH(f) + 8;
    rect tf = {c.x, *y, c.w - 160 > 260 ? 320 : c.w - 160, 40};
    ui_textfield(tf, &nm, true, "Their name", f, color_alpha(P.ink, 0.06f), P.ink, P.muted, 12);
    rect b = {tf.x + tf.w + 12, *y, 130, 40};
    if (log_now)
        LOGRECT("settings-name", tf);
    bool enter = ui_type(&nm);
    bool changed = nm.len > 0 && strcmp(nm.buf, D.ai_name);
    if ((ctl_button(b, "Rename", f, changed, changed, &P) || (enter && changed)) && nm.len > 0) {
        companion_rename(nm.buf);
        snprintf(shown, sizeof shown, "%s", D.ai_name);
    }
    *y += 40 + 22;
}

static int page_mind(rect c, int y) {
    int y0 = y;
    font *f = BODY();
    if (mind.step == MIND_SHOW)
        name_row(c, &y, f);
    if (mind.step == MIND_SHOW) {
        rect card = {c.x, y, c.w, 112};
        fill_round(A.frame, card, 18, color_alpha(P.ink, 0.05f));
        rect ic = {card.x + 18, card.y + 20, 72, 72};
        ui_orb(A.frame, ic.x + 36.0f, ic.y + 36.0f, 20, D.accent, D.accent2, A.companion.pending != NULL);
        font *big = settings_font(SF_SECTION);
        const char *name = A.provider ? A.provider->name : A.cfg.provider[0] ? A.cfg.provider : "No mind yet";
        draw_text(A.frame, fm_host_font(D.font_heading, 22), card.x + 108, card.y + 24, name, P.ink);
        bool here = A.provider && !strcmp(A.provider->id, "builtin");
        draw_text(A.frame, f, card.x + 108, card.y + 30 + LH(big) + 6,
                  here ? models_selected()->name : A.cfg.model[0] ? A.cfg.model : "No model chosen", P.muted);
        y += card.h + 16;
        y += draw_text_wrapped(A.frame, f, (rect){c.x, y, c.w, 200},
                               is_local() ? "Your words stay on your own machines: this computer, or one you run."
                                          : "Your words go to this service when you talk, and nowhere else.",
                               P.ink, 1.35f) + 18;
        rect b = {c.x, y, 200, 40};
        if (log_now)
            LOGRECT("settings-mind-change", b);
        if (ctl_button(b, "Change mind", f, true, true, &P)) {
            mind_reset();
            mind.step = MIND_CHOOSE;
        }
        /* models that run here: download, switch, remove */
        rect mb = {c.x + 212, y, 230, 40};
        if (log_now)
            LOGRECT("settings-mind-models", mb);
        if (ctl_button(mb, "Models on this computer", f, false, true, &P)) {
            mind_reset();
            mind.p = ai_find_provider("builtin");
            mind.step = MIND_MODELS;
        }
        y += 40 + 16;
        y += note_text(c, y, "Changing the mind keeps your desktop, your portrait and your conversation. Only who "
                             "does the thinking changes.");
        return y - y0 + 10;
    }
    if (mind.step == MIND_MODELS) {
        rect back = {c.x, y, 96, 34};
        if (ctl_button(back, "Back", SMALL(), false, true, &P))
            mind.step = MIND_SHOW;
        y += 34 + 18;
        draw_text(A.frame, fm_host_font(D.font_heading, 22), c.x, y, "Models on this computer", P.ink);
        y += 36;
        y += models_list(c, y, &P, f, SMALL(), SMALL(), settings_use_model);
        return y - y0;
    }
    if (mind.step == MIND_CHOOSE) {
        rect back = {c.x, y, 96, 34};
        if (ctl_button(back, "Back", SMALL(), false, true, &P))
            mind.step = MIND_SHOW;
        y += 34 + 18;
        static const char *groups[] = {"Suggested", "Big labs", "Aggregators", "On your network", "Enterprise", "Other"};
        int colw = (c.w - 12) / 2;
        for (int g = 0; g < 6; g++) {
            int k = 0;
            for (int i = 0; i < ai_provider_count; i++) {
                const struct ai_provider *p = &ai_providers[i];
                bool suggested = !strcmp(p->id, "claude-pc") || (!strcmp(p->id, "builtin") && mind_builtin_present());
                if (g == 0 ? !suggested : (suggested || strcmp(p->group, groups[g])))
                    continue;
                if (!strcmp(p->id, "builtin") && !mind_builtin_present())
                    continue;
                if (k == 0)
                    y = settings_section(c, y, groups[g]);
                mind_card(c, y + (k / 2) * 78, p, colw, c.x + (k % 2) * (colw + 12));
                k++;
            }
            if (k)
                y += ((k + 1) / 2) * 78 + 12;
        }
        return y - y0;
    }
    /* connect: the address and key where needed, then a model */
    const struct ai_provider *p = mind.p;
    rect back = {c.x, y, 96, 34};
    if (ctl_button(back, "Back", SMALL(), false, !mind.job, &P)) {
        mind_reset();
        mind.step = MIND_CHOOSE;
    }
    y += 34 + 18;
    draw_text(A.frame, fm_host_font(D.font_heading, 22), c.x, y, p->name, P.ink);
    y += 36;
    y += draw_text_wrapped(A.frame, f, (rect){c.x, y, c.w, 100}, p->blurb, P.muted, 1.3f) + 16;
    rect url_r = {0, 0, 0, 0}, key_r = {0, 0, 0, 0};
    if (p->url_editable) {
        draw_text(A.frame, SMALL(), c.x, y, "Address", P.muted);
        url_r = (rect){c.x, y + 22, c.w, 44};
        ui_textfield(url_r, &mind.url, mind.focus == 0, "https://...", f, color_alpha(P.ink, 0.06f), P.ink, P.muted, 12);
        y += 80;
    }
    if (p->needs_key) {
        draw_text(A.frame, SMALL(), c.x, y, "Key", P.muted);
        key_r = (rect){c.x, y + 22, c.w, 44};
        ui_textfield(key_r, &mind.key, mind.focus == 1, "Paste or type the key", f, color_alpha(P.ink, 0.06f), P.ink,
                     P.muted, 12);
        y += 80;
    }
    if (A.in.clicked) {
        if (p->url_editable && ui_hover(url_r))
            mind.focus = 0;
        if (p->needs_key && ui_hover(key_r))
            mind.focus = 1;
    }
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_TAB && p->url_editable && p->needs_key)
            mind.focus ^= 1;
    bool enter = false;
    if (!mind.job && !mind.models && (p->url_editable || p->needs_key))
        enter = ui_type(mind.focus == 0 && p->url_editable ? &mind.url : &mind.key);
    char *res;
    if (mind.job && job_poll(mind.job, &res)) {
        job_free(mind.job);
        mind.job = NULL;
        if (res && !strncmp(res, "OK\n", 3)) {
            int n = 0;
            for (char *s = res + 3; *s; s++)
                n += *s == '\n';
            mind.models = calloc((size_t)n + 1, sizeof(char *));
            mind.n = 0;
            for (char *save, *line = strtok_r(res + 3, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
                mind.models[mind.n++] = strdup(line);
            const char *pick = ai_pick_model(p, mind.models, mind.n);
            for (int i = 0; i < mind.n; i++)
                if (pick && !strcmp(mind.models[i], pick))
                    mind.sel = i;
            snprintf(mind.status, sizeof mind.status, "Connected.");
            if (!mind.n) {
                free(mind.models);
                mind.models = NULL;
                free(res);
                mind_use(p->default_model);
                return y - y0;
            }
        } else {
            snprintf(mind.status, sizeof mind.status, "%s", res && !strncmp(res, "ERR\n", 4) ? res + 4 : "That didn't work.");
            mind.failed = true;
            LOG("settings: mind check failed: %.200s\n", mind.status);
        }
        free(res);
    }
    if (!mind.models) {
        rect go = {c.x, y, 160, 42};
        if (log_now)
            LOGRECT("settings-mind-connect", go);
        bool g = ctl_button(go, mind.job ? "Connecting..." : "Connect", f, true, !mind.job, &P);
        if ((g || enter) && !mind.job) {
            memset(&mind_cfg, 0, sizeof mind_cfg);
            snprintf(mind_cfg.provider, sizeof mind_cfg.provider, "%s", p->id);
            snprintf(mind_cfg.base_url, sizeof mind_cfg.base_url, "%s", p->url_editable ? mind.url.buf : p->base_url ? p->base_url : "");
            snprintf(mind_cfg.api_key, sizeof mind_cfg.api_key, "%s", mind.key.buf);
            snprintf(mind.status, sizeof mind.status, "Reaching %s...", p->name);
            mind.failed = false;
            mind.job = job_start(mind_child, NULL);
        }
        /* A mind that isn't answering yet (the PC's bridge not started, a
         * server still booting) can still be chosen: it is used once it answers. */
        if (mind.failed && p->default_model && *p->default_model) {
            rect anyway = {c.x + 172, y, 170, 42};
            if (log_now)
                LOGRECT("settings-mind-anyway", anyway);
            if (ctl_button(anyway, "Use it anyway", f, false, true, &P))
                mind_use(p->default_model);
        }
        y += 42 + 12;
        y += draw_text_wrapped(A.frame, SMALL(), (rect){c.x, y, c.w, 120}, mind.status, mind.failed ? P.ink : P.muted, 1.3f) + 10;
        return y - y0;
    }
    y = settings_section(c, y, "Which model?");
    for (int i = 0; i < mind.n && i < 40; i++) {
        rect row = {c.x, y, c.w, 38};
        bool sel = i == mind.sel, hover = ui_hover(row);
        fill_round(A.frame, row, 10, sel ? color_alpha(P.accent, 0.2f) : color_alpha(P.ink, hover ? 0.06f : 0));
        ctl_text_fit(A.frame, f, row.x + 14, row.y + (38 - LH(f)) / 2, row.w - 28, mind.models[i], sel ? P.ink : P.muted);
        if (hover && A.in.clicked)
            mind.sel = i;
        y += 42;
    }
    y += 8;
    rect use = {c.x, y, 180, 42};
    if (log_now)
        LOGRECT("settings-mind-use", use);
    if (ctl_button(use, "Use this mind", f, true, true, &P))
        mind_use(mind.models[mind.sel]);
    y += 42 + 10;
    return y - y0;
}

/* ---- Privacy ---- */

static int page_privacy(rect c, int y) {
    int y0 = y;
    font *f = BODY(), *small = SMALL();
    char line[400];
    snprintf(line, sizeof line, "What %s knows about you", D.ai_name[0] ? D.ai_name : "your companion");
    y = settings_section(c, y, line);
    if (!A.prof.nfacts) {
        draw_text(A.frame, f, c.x, y, "Nothing yet.", P.muted);
        y += LH(f) + 8;
    }
    for (int i = 0; i < A.prof.nfacts; i++) {
        snprintf(line, sizeof line, "%s", A.prof.facts[i].val);
        int th = draw_text_wrapped(NULL, f, (rect){0, 0, c.w - 230, 400}, line, 0, 1.3f);
        rect row = {c.x, y, c.w, th + 18};
        bool hover = ui_hover(row);
        if (hover)
            fill_round(A.frame, row, 10, color_alpha(P.ink, 0.06f));
        draw_text(A.frame, small, c.x + 10, y + 9, A.prof.facts[i].key, P.muted);
        draw_text_wrapped(A.frame, f, (rect){c.x + 120, y + 7, c.w - 230, th + 4}, line, P.ink, 1.3f);
        rect fb = {c.x + c.w - 92, y + 6, 84, 30};
        if (hover && ctl_button(fb, "Forget", small, false, true, &P)) {
            portrait_forget(&A.prof, i);
            portrait_save(&A.prof, STATE_DIR "/portrait.txt");
            break;
        }
        y += row.h + 4;
    }
    y += 6;
    y += note_text(c, y, "Kept only on this computer. It shapes how your space looks and how I talk with you.") + 24;
    y = settings_section(c, y, "Where your words go");
    snprintf(line, sizeof line, "%s, %s. %s", A.provider ? A.provider->name : A.cfg.provider, A.cfg.model,
             is_local() ? "Your words stay on your own machines." : "Your words go to this service when you talk with me.");
    y += draw_text_wrapped(A.frame, f, (rect){c.x, y, c.w, 200}, line, P.ink, 1.3f) + 8;
    snprintf(line, sizeof line, "How present I am: %s. Reshaping the space: %s.",
             A.prof.autonomy[0] ? A.prof.autonomy : "I ask first", A.prof.redesign[0] ? A.prof.redesign : "only when you ask");
    y += note_text(c, y, line) + 24;
    y = settings_section(c, y, "Start again");
    static uint64_t confirm_at;
    bool confirming = confirm_at && A.now - confirm_at < 5000;
    rect fb = {c.x, y, confirming ? 260 : 160, 42};
    palette danger = P;
    if (confirming) {
        danger.accent = RGBA(200, 70, 60, 255);
        danger.on_accent = RGBA(255, 255, 255, 255);
    }
    if (ctl_button(fb, confirming ? "Click again to forget all" : "Forget me", f, confirming, true, &danger)) {
        if (confirming) {
            confirm_at = 0;
            forget_everything();
            return 0;
        }
        confirm_at = A.now;
    }
    y += 42 + 12;
    y += note_text(c, y, "Forget me erases the portrait, the design, your notes and the connection, and starts again.");
    return y - y0 + 10;
}

/* ---- About ---- */

static int page_about(rect c, int y) {
    int y0 = y;
    static uint64_t read_at;
    static char cpu[96], mem[64], up[64], kern[96];
    static int cores;
    static float used = 0;
    if (!read_at || A.now - read_at > 1000) {
        read_at = A.now;
        char line[256];
        FILE *f = fopen("/proc/cpuinfo", "r");
        cores = 0;
        cpu[0] = 0;
        while (f && fgets(line, sizeof line, f)) {
            if (!strncmp(line, "processor", 9))
                cores++;
            char *colon = strchr(line, ':');
            if (!cpu[0] && !strncmp(line, "model name", 10) && colon) {
                snprintf(cpu, sizeof cpu, "%s", colon + 2);
                cpu[strcspn(cpu, "\n")] = 0;
            }
        }
        if (f)
            fclose(f);
        long total = 0, avail = 0;
        f = fopen("/proc/meminfo", "r");
        while (f && fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %ld", &total);
            sscanf(line, "MemAvailable: %ld", &avail);
        }
        if (f)
            fclose(f);
        used = total ? 1 - (float)avail / (float)total : 0;
        snprintf(mem, sizeof mem, "%.1f GB, %.1f GB in use", total / 1048576.0, (total - avail) / 1048576.0);
        double secs = 0;
        f = fopen("/proc/uptime", "r");
        if (f) {
            if (fscanf(f, "%lf", &secs) != 1)
                secs = 0;
            fclose(f);
        }
        long s = (long)secs;
        if (s < 3600)
            snprintf(up, sizeof up, "%ld min", s / 60);
        else if (s < 86400)
            snprintf(up, sizeof up, "%ld h %ld min", s / 3600, (s % 3600) / 60);
        else
            snprintf(up, sizeof up, "%ld days, %ld h", s / 86400, (s % 86400) / 3600);
        /* uname says "Linux" so Linux apps run; the kernel is this system's own */
        snprintf(kern, sizeof kern, "Its own, written for this system (runs Linux apps)");
    }
    draw_text(A.frame, fm_host_font(D.font_heading, 30), c.x, y, "Operating System", P.ink);
    y += 44;
    draw_text(A.frame, BODY(), c.x, y, "Version " OS_VERSION, P.muted);
    y += 40;
    char buf[160], disp[48];
    snprintf(disp, sizeof disp, "%d \xc3\x97 %d", A.W, A.H);
    snprintf(buf, sizeof buf, "%s%s%d cores", cpu[0] ? cpu : "", cpu[0] ? ", " : "", cores);
    const struct net_state *n = net_state();
    struct {
        const char *k, *v;
    } rows[] = {{"Processor", buf}, {"Memory", mem}, {"Up for", up}, {"Display", disp},
                {"Network", n->has_ip ? n->ip : n->link ? "Connecting" : "Offline"}, {"Kernel", kern}};
    for (int i = 0; i < 6; i++) {
        draw_text(A.frame, BODY(), c.x, y, rows[i].k, P.muted);
        ctl_text_fit(A.frame, BODY(), c.x + 150, y, c.w - 150, rows[i].v, P.ink);
        y += LH(BODY()) + 6;
        if (i == 1) { /* how full memory is */
            rect bar = {c.x + 150, y, c.w - 150 > 300 ? 300 : c.w - 150, 6};
            fill_round(A.frame, bar, 3, color_alpha(P.ink, 0.12f));
            fill_round(A.frame, (rect){bar.x, bar.y, (int)(bar.w * used), bar.h}, 3, P.accent);
            y += 14;
        }
        y += 8;
    }
    y += 14;
    y += note_text(c, y, "Kernel, drivers, network stack and desktop written for this operating system. Uses mbed TLS "
                         "(Apache-2.0), stb_truetype (public domain), Lua (MIT), fonts under the SIL Open Font License, "
                         "and Mozilla's CA list.");
    return y - y0 + 10;
}

/* ======================================================================
 * The window
 * ==================================================================== */

struct page {
    char title[40];
    int sym;
    int (*builtin)(rect c, int y);
    void (*draw)(rect r);
};

#define MAX_EXTRA 8
static struct page extra[MAX_EXTRA];
static int nextra;

void settings_register_page(const char *title, void (*draw)(rect r)) {
    if (!title || !draw)
        return;
    for (int i = 0; i < nextra; i++)
        if (!strcmp(extra[i].title, title)) {
            extra[i].draw = draw;
            return;
        }
    if (nextra == MAX_EXTRA)
        return;
    struct page *p = &extra[nextra++];
    snprintf(p->title, sizeof p->title, "%s", title);
    p->sym = strcasestr(title, "sens") ? SYM_SENSE : SYM_GEAR;
    p->draw = draw;
}

static int page_list(struct page *out) {
    static const struct page before[] = {{"Appearance", SYM_PALETTE, page_appearance, NULL},
                                         {"Display", SYM_DISPLAY, page_display, NULL},
                                         {"Sound", SYM_SPEAKER, page_sound, NULL},
                                         {"Keyboard", SYM_KEYBOARD, page_keyboard, NULL},
                                         {"Mind", SYM_MIND, page_mind, NULL}};
    static const struct page after[] = {{"Privacy", SYM_SHIELD, page_privacy, NULL}, {"About", SYM_INFO, page_about, NULL}};
    int n = 0;
    for (size_t i = 0; i < sizeof before / sizeof before[0]; i++)
        out[n++] = before[i];
    for (int i = 0; i < nextra; i++)
        out[n++] = extra[i];
    for (size_t i = 0; i < sizeof after / sizeof after[0]; i++)
        out[n++] = after[i];
    return n;
}

static char current[40] = "Appearance";
static int scroll_of[24];

void settings_open_page(const char *title) {
    if (title)
        snprintf(current, sizeof current, "%s", title);
    if (A.ui)
        fm_host_action(A.ui, NULL, "open:settings");
}

void settings_draw(rect r) {
    static uint64_t logged_at;
    log_now = A.now - logged_at > 1000;
    if (log_now)
        logged_at = A.now;
    int ox0 = A.frame->cx0, oy0 = A.frame->cy0, ox1 = A.frame->cx1, oy1 = A.frame->cy1;
    struct page pages[5 + MAX_EXTRA + 2];
    int np = page_list(pages), cur = 0;
    for (int i = 0; i < np; i++)
        if (!strcmp(pages[i].title, current))
            cur = i;
    int side = r.w > 760 ? 214 : r.w > 560 ? 176 : 0;
    /* sidebar */
    if (side) {
        rect sb = {r.x, r.y, side, r.h};
        color under = pal_sample(sb);
        palette sp = pal_on(under, 1);
        fill_round(A.frame, sb, D.radius * 0.6f + 6, color_alpha(sp.ink, 0.045f));
        int y = r.y + 12;
        font *f = fm_host_font(D.font_body, 15);
        for (int i = 0; i < np; i++) {
            rect row = {r.x + 8, y, side - 16, 40};
            bool sel = i == cur, hover = ui_hover(row);
            if (sel)
                fill_round(A.frame, row, 12, color_alpha(sp.accent, 0.2f));
            else if (hover)
                fill_round(A.frame, row, 12, color_alpha(sp.ink, 0.07f));
            ctl_symbol(A.frame, (rect){row.x + 12, row.y + 10, 20, 20}, pages[i].sym, 0.7f, sel ? sp.accent : sp.muted);
            ctl_text_fit(A.frame, f, row.x + 44, row.y + (40 - font_line_height(f)) / 2, row.w - 52, pages[i].title,
                         sel ? sp.ink : color_mix(sp.ink, sp.muted, 0.35f));
            char nm[64];
            snprintf(nm, sizeof nm, "settings-page-%s", pages[i].title);
            for (char *p = nm; *p; p++)
                if (*p == ' ')
                    *p = '-';
            if (log_now)
                LOGRECT(nm, row);
            if (hover && A.in.clicked && !sel) {
                snprintf(current, sizeof current, "%s", pages[i].title);
                cur = i;
            }
            y += 44;
        }
    } else {
        /* too narrow for a sidebar: a row of page names */
        static const char *names[16];
        for (int i = 0; i < np && i < 16; i++)
            names[i] = pages[i].title;
        palette sp = pal_on(pal_sample(r), 1);
        int pick = ctl_segmented((rect){r.x, r.y, r.w, 34}, names, np < 16 ? np : 16, cur, fm_host_font(D.font_body, 12), &sp);
        if (pick != cur) {
            cur = pick;
            snprintf(current, sizeof current, "%s", pages[cur].title);
        }
    }
    /* the page */
    rect c = side ? (rect){r.x + side + 30, r.y + 4, r.w - side - 38, r.h - 8} : (rect){r.x, r.y + 46, r.w, r.h - 46};
    P = pal_on(pal_sample(c), 1);
    font *tf = settings_font(SF_TITLE);
    draw_text(A.frame, tf, c.x, c.y + 4, pages[cur].title, P.ink);
    int top = c.y + font_line_height(tf) + 22;
    rect area = {c.x - 6, top, c.w + 12, c.y + c.h - top};
    int *scroll = &scroll_of[cur < 24 ? cur : 23];
    if (ui_hover(area) && A.in.wheel && !ctl_dragging())
        *scroll -= A.in.wheel * 48;
    if (*scroll < 0)
        *scroll = 0;
    /* Only what can be seen takes the pointer: content scrolled under the
     * title or the window's edge must not be clickable. */
    input_state saved = A.in;
    if (!pt_in(area, A.in.mx, A.in.my) && !ctl_dragging()) {
        A.in.mx = A.in.my = -10000;
        A.in.clicked = A.in.pressed = false;
    }
    canvas_clip(A.frame, area);
    if (A.frame->cx0 < ox0) A.frame->cx0 = ox0;
    if (A.frame->cy0 < oy0) A.frame->cy0 = oy0;
    if (A.frame->cx1 > ox1) A.frame->cx1 = ox1;
    if (A.frame->cy1 > oy1) A.frame->cy1 = oy1;
    rect content = {c.x, top, c.w, area.h};
    if (pages[cur].builtin) {
        int h = pages[cur].builtin(content, top + 4 - *scroll);
        int max = h - area.h + 24;
        if (*scroll > (max > 0 ? max : 0))
            *scroll = max > 0 ? max : 0;
        if (max > 0) { /* a quiet scroll indicator */
            float frac = (float)area.h / (float)(h + 24), pos = (float)*scroll / (float)max;
            int bh = (int)(area.h * frac), by = area.y + (int)((area.h - bh) * pos);
            fill_round(A.frame, (rect){area.x + area.w - 4, by, 3, bh}, 1.5f, color_alpha(P.ink, 0.18f));
        }
    } else if (pages[cur].draw) {
        pages[cur].draw(content);
    }
    A.in = saved;
    A.frame->cx0 = ox0, A.frame->cy0 = oy0, A.frame->cx1 = ox1, A.frame->cy1 = oy1;
}
