#define _GNU_SOURCE
#include "settings.h"
#include "chats.h"
#include "lock.h"
#include "clock.h"
#include "voice.h"
#include "launcher.h"
#include "iconart.h"
#include "files.h"
#include "priv.h"
#include "appgate.h"
#include "nerve.h"
#include "app.h"
#include "emblem.h"
#include "fm/fm.h"
#include "keymap.h"
#include "mind.h"
#include "models.h"
#include "tray.h"
#include "wallpaper.h"
#include "appapi.h"
#include "body.h"
#include "presence.h"
#include "wl/wl.h"
#include "../lib/json.h"

#include <math.h>
#include <fcntl.h>
#include <dirent.h>
#include <ftw.h>
#include <sys/statvfs.h>
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
/* keys to their accounts (any setting named *_key), apart: the companion's view of the state folder leaves this
 * file out (agent.c) */
#define SECRETS STATE_DIR "/secrets.conf"

static bool secret_name(const char *key) {
    size_t l = strlen(key);
    return l > 4 && !strcmp(key + l - 4, "_key");
}

/* ======================================================================
 * Kept choices
 * ==================================================================== */

static struct {
    char key[32], val[96];
} kv[48];
static int nkv;
static bool kv_loaded;

static void kv_write(void);

static void kv_load(void) {
    kv_loaded = true;
    nkv = 0;
    bool moved = false;
    for (int pass = 0; pass < 2; pass++) {
        FILE *f = fopen(pass ? SECRETS : CONF, "r");
        char line[160];
        while (f && fgets(line, sizeof line, f) && nkv < 48) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '#')
                continue;
            *eq = 0;
            moved |= !pass && secret_name(line); /* kept with the settings before: moved */
            snprintf(kv[nkv].key, sizeof kv[nkv].key, "%s", line);
            snprintf(kv[nkv].val, sizeof kv[nkv].val, "%s", eq + 1);
            nkv++;
        }
        if (f)
            fclose(f);
    }
    if (moved)
        kv_write();
}

/* written whole each time: a handful of short lines, the keys apart */
static void kv_write(void) {
    for (int pass = 0; pass < 2; pass++) {
        const char *path = pass ? SECRETS : CONF;
        char tmp[96];
        snprintf(tmp, sizeof tmp, "%s.new", path);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
        if (!f)
            continue;
        fputs(pass ? "# Keys to their accounts, as typed in Settings.\n" : "# Settings, as chosen in the Settings app.\n", f);
        for (int k = 0; k < nkv; k++)
            if (secret_name(kv[k].key) == (pass == 1))
                fprintf(f, "%s=%s\n", kv[k].key, kv[k].val);
        fclose(f);
        rename(tmp, path);
    }
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
    kv_write();
}

void settings_set_int(const char *key, int value) {
    char b[24];
    snprintf(b, sizeof b, "%d", value);
    settings_set(key, b);
}

/* the mouse as they set it (sys.c does it) */
static void mouse_apply(void) {
    input_set_mouse(settings_get_int("mouse_speed", 100) / 100.0f, settings_get_int("mouse_accel", 1) != 0,
                    settings_get_int("mouse_natural", 0) != 0, settings_get_int("mouse_left", 0) != 0);
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
    mouse_apply();
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
        draw_text(A.frame, SMALL(), c.x + 256, y + (38 - LH(SMALL())) / 2, "This design has a fixed colour scheme.", P.muted);
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
                 {"Less movement", "A static wallpaper and minimal animation", A.prof.reduce_motion}};
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
    if (D.rationale[0]) { /* why it looks like this, in the words of whoever designed it */
        y += 12;
        y = settings_section(c, y, "About this design");
        y += note_text(c, y, D.rationale);
        char faces[160];
        snprintf(faces, sizeof faces, "Type: %s for headings, %s for text.", D.font_heading, D.font_body);
        y += note_text(c, y, faces);
    }
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
    y = settings_section(c, y, "Voice");
    {
        const char *mix = settings_get("voice.mix", ""), *name = settings_get("voice.name", "");
        char line[300];
        snprintf(line, sizeof line, "%s%s", mix[0] ? "A mix of " : name[0] ? name : "The stand-in voice (none chosen yet)",
                 mix[0] ? mix : "");
        draw_text(A.frame, BODY(), c.x, y, line, P.ink);
        y += LH(BODY()) + 4;
        y += note_text(c, y, "Ask your companion for a voice of its own: it mixes its voices into a new one and "
                             "shapes it with you. Or shape it here.");
        y += 12;
        static const struct {
            const char *label, *key, *lo, *hi;
            float min, max, step;
        } knobs[] = {{"Pitch", "voice.pitch", "lower", "higher", -12, 12, 0.5f},
                     {"Character", "voice.formant", "bigger", "smaller", -6, 6, 0.5f},
                     {"Warmth", "voice.warmth", "brighter", "warmer", -1, 1, 0.05f},
                     {"Breath", "voice.breath", "none", "airy", 0, 1, 0.05f}};
        for (int i = 0; i < 4; i++) {
            float v = (float)atof(settings_get(knobs[i].key, "0"));
            row_label(c, y, 40, knobs[i].label, NULL);
            char val[32];
            snprintf(val, sizeof val, "%s", v == 0 ? "as it is" : v < 0 ? knobs[i].lo : knobs[i].hi);
            draw_text(A.frame, SMALL(), c.x + c.w - text_width(SMALL(), val), y + (40 - LH(SMALL())) / 2, val, P.muted);
            float t = (v - knobs[i].min) / (knobs[i].max - knobs[i].min);
            rect sl = {c.x + 140, y + 9, c.w - 140 - 90, 22};
            if (log_now) {
                char nm[32];
                snprintf(nm, sizeof nm, "voice-%s", knobs[i].key + 6);
                LOGRECT(nm, sl);
            }
            if (ctl_slider(2301 + i, sl, &t, &P) == CTL_RELEASED) {
                float nv = knobs[i].min + t * (knobs[i].max - knobs[i].min);
                nv = roundf(nv / knobs[i].step) * knobs[i].step;
                if (fabsf(nv) < knobs[i].step * 0.75f)
                    nv = 0; /* it snaps to as it is */
                char num[16];
                snprintf(num, sizeof num, "%.2f", (double)nv);
                settings_set(knobs[i].key, num);
                LOG("settings: %s %s\n", knobs[i].key, num);
            }
            y += 46;
        }
        static const char *const fx_l[] = {"None", "Robot", "Whisper", "Radio", "Phone", "Room", "Hall"};
        static const char *const fx_v[] = {"none", "robot", "whisper", "radio", "phone", "room", "hall"};
        int cur = 0;
        for (int i = 0; i < 7; i++)
            if (!strcmp(settings_get("voice.effect", "none"), fx_v[i]))
                cur = i;
        draw_text(A.frame, BODY(), c.x, y + 9, "Effect", P.muted);
        rect er = {c.x + 140, y, c.w - 140, 38};
        if (log_now)
            LOGRECT("voice-effect", er);
        int pick = ctl_segmented(er, fx_l, 7, cur, SMALL(), &P);
        if (pick != cur) {
            settings_set("voice.effect", fx_v[pick]);
            LOG("settings: voice.effect %s\n", fx_v[pick]);
        }
        y += 52;
        rect hb = {c.x, y, 120, 38}, rb = {c.x + 132, y, 160, 38};
        if (log_now)
            LOGRECT("voice-hear", hb);
        if (ctl_button(hb, "Hear it", BODY(), true, voice_can_speak(), &P))
            voice_say("This is how I sound now. Is it right?");
        if (ctl_button(rb, "Back to plain", BODY(), false, voice_shape_now()[0] != 0, &P))
            voice_shape("reset");
        y += 38 + 26;
    }
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

/* ---- Notifications ---- */

static int page_notify(rect c, int y) {
    int y0 = y;
    font *f = BODY();
    y = settings_section(c, y, "Do not disturb");
    row_label(c, y, 48, "Do not disturb", "Notifications are kept, but none pops up");
    rect sw = {c.x + c.w - 52, y + 12, 48, 26};
    if (log_now)
        LOGRECT("notify-dnd", sw);
    if (ctl_switch(sw, notify_dnd_by_hand(), &P))
        notify_set_dnd(!notify_dnd_by_hand());
    y += 60;
    /* by the hours */
    int from = settings_get_int("dnd_from", -1), to = settings_get_int("dnd_to", -1);
    bool hours = from >= 0 && to >= 0;
    row_label(c, y, 48, "Every day, at set hours", hours ? (notify_dnd_hours() ? "On now" : "Not now") : "Off");
    rect hs = {c.x + c.w - 52, y + 12, 48, 26};
    if (log_now)
        LOGRECT("notify-hours", hs);
    if (ctl_switch(hs, hours, &P)) {
        settings_set_int("dnd_from", hours ? -1 : 22);
        settings_set_int("dnd_to", hours ? -1 : 7);
        hours = !hours, from = hours ? 22 : -1, to = hours ? 7 : -1;
    }
    y += 58;
    if (hours) {
        struct {
            const char *label, *key;
            int *v;
        } ends[] = {{"From", "dnd_from", &from}, {"Until", "dnd_to", &to}};
        for (int e = 0; e < 2; e++) {
            draw_text(A.frame, f, c.x, y + (36 - LH(f)) / 2, ends[e].label, P.muted);
            struct tm tm = {.tm_hour = *ends[e].v};
            char hh[24];
            fm_strftime(hh, sizeof hh, "%H:00", &tm);
            rect minus = {c.x + 90, y, 36, 36}, plus = {c.x + 90 + 36 + 110, y, 36, 36};
            if (ctl_button(minus, "-", f, false, true, &P))
                settings_set_int(ends[e].key, (*ends[e].v + 23) % 24);
            draw_text(A.frame, f, c.x + 90 + 36 + (110 - text_width(f, hh)) / 2, y + (36 - LH(f)) / 2, hh, P.ink);
            if (ctl_button(plus, "+", f, false, true, &P))
                settings_set_int(ends[e].key, (*ends[e].v + 1) % 24);
            y += 44;
        }
        y += 8;
    }
    y += 10;
    y = settings_section(c, y, "Apps that may notify you");
    char all[1024];
    snprintf(all, sizeof all, "%s", notify_apps());
    int shown = 0;
    for (char *save, *app = strtok_r(all, ",", &save); app; app = strtok_r(NULL, ",", &save)) {
        row_label(c, y, 44, app, NULL);
        bool on = notify_app_allowed(app);
        rect as = {c.x + c.w - 52, y + 9, 48, 26};
        if (ctl_switch(as, on, &P))
            notify_app_allow(app, !on);
        y += 48;
        shown++;
    }
    if (!shown)
        y += note_text(c, y, "No app has sent a notification yet. Each one that does appears here, with a switch.");
    y += 8;
    y += note_text(c, y, "The system's own notifications and your companion's always come through.");
    return y - y0 + 10;
}

/* ---- Apps ---- */

/* The apps they installed (Flatpak, from Flathub), each removable; which app opens web links and each kind of
 * file (~/.config/mimeapps.list, the freedesktop way: the portal and every app read it). */
#define MAX_APPS 96
struct inst_app {
    char id[128], name[96], mime[1024];
};
static struct {
    struct inst_app *a;
    int n, removing_req;
    char removing[128], msg[160];
    uint64_t at;
} AP;

static void apps_read(void) {
    if (AP.a && A.now - AP.at < 3000)
        return;
    AP.at = A.now;
    if (!AP.a)
        AP.a = calloc(MAX_APPS, sizeof *AP.a);
    AP.n = 0;
    const char *dirs[] = {FLATPAK_DISK_EXPORTS "/applications", FLATPAK_SYSTEM_EXPORTS "/applications"};
    for (int k = 0; k < 2; k++) {
        DIR *d = opendir(dirs[k]);
        struct dirent *e;
        while (d && (e = readdir(d)) && AP.n < MAX_APPS) {
            size_t l = strlen(e->d_name);
            if (l < 9 || strcmp(e->d_name + l - 8, ".desktop"))
                continue;
            struct inst_app *x = &AP.a[AP.n];
            snprintf(x->id, sizeof x->id, "%.*s", (int)(l - 8), e->d_name);
            bool dup = false;
            for (int i = 0; i < AP.n; i++)
                dup |= !strcmp(AP.a[i].id, x->id);
            if (dup)
                continue;
            char p[400], line[1200];
            snprintf(p, sizeof p, "%s/%s", dirs[k], e->d_name);
            FILE *f = fopen(p, "r");
            x->name[0] = x->mime[0] = 0;
            bool in_entry = false;
            while (f && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0] == '[')
                    in_entry = !strcmp(line, "[Desktop Entry]");
                else if (in_entry && !strncmp(line, "Name=", 5) && !x->name[0])
                    snprintf(x->name, sizeof x->name, "%s", line + 5);
                else if (in_entry && !strncmp(line, "MimeType=", 9))
                    snprintf(x->mime, sizeof x->mime, "%s", line + 9);
            }
            if (f)
                fclose(f);
            if (!x->name[0])
                snprintf(x->name, sizeof x->name, "%s", x->id);
            AP.n++;
        }
        if (d)
            closedir(d);
    }
}

/* the kinds of thing an app can be the default for, and the types each covers */
static const struct {
    const char *label, *types;
} kinds[] = {
    {"Web links", "x-scheme-handler/http x-scheme-handler/https text/html"},
    {"Email links", "x-scheme-handler/mailto"},
    {"Pictures", "image/png image/jpeg image/gif image/webp image/svg+xml image/bmp"},
    {"Music", "audio/mpeg audio/ogg audio/flac audio/x-wav audio/mp4"},
    {"Videos", "video/mp4 video/webm video/x-matroska video/quicktime"},
    {"PDF documents", "application/pdf"},
    {"Text", "text/plain"},
};
#define NKINDS ((int)(sizeof kinds / sizeof kinds[0]))

static bool app_handles(const struct inst_app *x, const char *types) {
    char t[200];
    snprintf(t, sizeof t, "%s", types);
    for (char *save, *m = strtok_r(t, " ", &save); m; m = strtok_r(NULL, " ", &save)) {
        char all[1100], want[90];
        snprintf(all, sizeof all, ";%s;", x->mime); /* "a;b;" as ";a;b;;": each type between semicolons */
        snprintf(want, sizeof want, ";%s;", m);
        if (strstr(all, want))
            return true;
    }
    return false;
}

#define MIMEAPPS home_path("/.config/mimeapps.list")

/* the default for a type, from mimeapps.list ("" none) */
static void default_for(const char *type, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(MIMEAPPS, "r");
    char line[600];
    bool in = false;
    size_t tl = strlen(type);
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[')
            in = !strcmp(line, "[Default Applications]");
        else if (in && !strncmp(line, type, tl) && line[tl] == '=') {
            snprintf(out, n, "%s", line + tl + 1);
            char *semi = strchr(out, ';');
            if (semi)
                *semi = 0;
            size_t l = strlen(out);
            if (l > 8 && !strcmp(out + l - 8, ".desktop"))
                out[l - 8] = 0;
        }
    }
    if (f)
        fclose(f);
}

/* app id the default for every type of kind k (NULL: none chosen); the rest of the file kept as it was */
static void set_default(int k, const char *id) {
    char *old = NULL;
    FILE *f = fopen(MIMEAPPS, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        old = calloc(1, (size_t)(sz > 0 ? sz : 0) + 1);
        if (old && sz > 0 && fread(old, 1, (size_t)sz, f) != (size_t)sz)
            old[0] = 0;
        fclose(f);
    }
    mkdir(home_path("/.config"), 0700);
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", MIMEAPPS);
    FILE *o = fopen(tmp, "w");
    if (!o) {
        free(old);
        return;
    }
    fprintf(o, "[Default Applications]\n");
    char t[200];
    snprintf(t, sizeof t, "%s", kinds[k].types);
    if (id)
        for (char *save, *m = strtok_r(t, " ", &save); m; m = strtok_r(NULL, " ", &save))
            fprintf(o, "%s=%s.desktop\n", m, id);
    /* the rest: other types' defaults, and every other section, as they were */
    bool in = false;
    for (char *save, *line = old ? strtok_r(old, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '[') {
            in = !strcmp(line, "[Default Applications]");
            if (!in)
                fprintf(o, "%s\n", line);
            continue;
        }
        if (in) {
            char *eq = strchr(line, '=');
            bool ours = false;
            if (eq) {
                char ty[120];
                snprintf(ty, sizeof ty, " %.*s ", (int)(eq - line), line);
                char all[220];
                snprintf(all, sizeof all, " %s ", kinds[k].types);
                ours = strstr(all, ty) != NULL;
            }
            if (!ours)
                fprintf(o, "%s\n", line);
        } else
            fprintf(o, "%s\n", line);
    }
    free(old);
    if (fclose(o) == 0)
        rename(tmp, MIMEAPPS);
    LOG("settings: %s open with %s\n", kinds[k].label, id ? id : "(not chosen)");
}

static int def_open = -1; /* the kind whose choices are showing */

static int page_apps(rect c, int y) {
    int y0 = y;
    font *f = BODY();
    apps_read();
    /* a removal's answer */
    if (AP.removing_req > 0) {
        bool ok;
        char *text = NULL;
        if (priv_take(AP.removing_req, &ok, &text)) {
            AP.removing_req = 0;
            snprintf(AP.msg, sizeof AP.msg, ok ? "Removed." : "It couldn't be removed: %s", text ? text : "");
            LOG("settings: removing %s: %s\n", AP.removing, ok ? "done" : "failed");
            free(text);
            AP.removing[0] = 0;
            AP.at = 0;
            launcher_apps_changed();
        }
    }
    y = settings_section(c, y, "Installed");
    if (!AP.n)
        y += note_text(c, y, "No apps installed yet. The launcher finds them on Flathub: search for what you want "
                             "and choose Install.");
    static char confirm[128];
    for (int i = 0; i < AP.n; i++) {
        struct inst_app *x = &AP.a[i];
        rect row = {c.x, y, c.w, 52};
        if (pt_in(row, A.in.mx, A.in.my))
            fill_round(A.frame, row, 8, color_alpha(P.ink, 0.04f));
        rect ir = {row.x + 8, row.y + 10, 32, 32};
        if (!app_icon_draw(A.frame, ir, x->id, NULL, 2, LOOK_COLOR, P.ink, 1, false))
            fill_round(A.frame, ir, 8, color_alpha(P.ink, 0.10f));
        draw_text(A.frame, f, row.x + 52, row.y + 8, x->name, P.ink);
        draw_text(A.frame, SMALL(), row.x + 52, row.y + 10 + LH(f), x->id, P.muted);
        bool busy = !strcmp(AP.removing, x->id);
        bool asking = !strcmp(confirm, x->id);
        const char *lab = busy ? "Removing..." : asking ? "Remove it" : "Remove";
        int bw = text_width(f, lab) + 32;
        rect br = {row.x + row.w - bw - 8, row.y + 8, bw, 36};
        if (log_now && i < 6) {
            char nm[32];
            snprintf(nm, sizeof nm, "apps-remove-%d", i);
            LOGRECT(nm, br);
        }
        if (ctl_button(br, lab, f, asking, !busy && priv_available() && !AP.removing[0], &P)) {
            if (!asking)
                snprintf(confirm, sizeof confirm, "%s", x->id); /* a second press removes it */
            else {
                char m[200];
                snprintf(m, sizeof m, "uninstall %s", x->id);
                AP.removing_req = priv_send(m);
                snprintf(AP.removing, sizeof AP.removing, "%s", x->id);
                confirm[0] = 0;
                AP.msg[0] = 0;
            }
        }
        if (asking)
            draw_text(A.frame, SMALL(), br.x - 16 - text_width(SMALL(), "Its settings go with it."),
                      br.y + (36 - LH(SMALL())) / 2, "Its settings go with it.", P.muted);
        y += 56;
    }
    if (AP.msg[0]) {
        draw_text(A.frame, SMALL(), c.x, y, AP.msg, P.muted);
        y += LH(SMALL()) + 6;
    }
    y += 16;
    y = settings_section(c, y, "Opens with");
    for (int k = 0; k < NKINDS; k++) {
        char first[200], cur[128];
        snprintf(first, sizeof first, "%s", kinds[k].types);
        first[strcspn(first, " ")] = 0;
        default_for(first, cur, sizeof cur);
        const char *cur_name = NULL;
        int nh = 0;
        for (int i = 0; i < AP.n; i++)
            if (app_handles(&AP.a[i], kinds[k].types)) {
                nh++;
                if (!strcmp(AP.a[i].id, cur))
                    cur_name = AP.a[i].name;
            }
        row_label(c, y, 44, kinds[k].label, NULL);
        const char *shown = cur_name ? cur_name : nh ? "The first that can" : k == 6 ? "Notes" : "Nothing installed opens these";
        rect pick = {c.x + 200, y + 4, c.w - 200, 36};
        if (log_now) {
            char nm[32];
            snprintf(nm, sizeof nm, "apps-default-%d", k);
            LOGRECT(nm, pick);
        }
        if (nh && ctl_button(pick, shown, f, false, true, &P))
            def_open = def_open == k ? -1 : k;
        else if (!nh)
            draw_text(A.frame, f, pick.x + 12, pick.y + (36 - LH(f)) / 2, shown, P.muted);
        y += 48;
        if (def_open == k) { /* its choices, under it */
            for (int i = 0; i < AP.n; i++) {
                if (!app_handles(&AP.a[i], kinds[k].types))
                    continue;
                rect r = {pick.x, y, pick.w, 36};
                bool hover = pt_in(r, A.in.mx, A.in.my), sel = !strcmp(AP.a[i].id, cur);
                if (hover || sel)
                    fill_round(A.frame, r, 8, color_alpha(sel ? P.accent : P.ink, sel ? 0.16f : 0.05f));
                draw_text(A.frame, f, r.x + 12, r.y + (36 - LH(f)) / 2, AP.a[i].name, P.ink);
                if (hover && A.in.clicked) {
                    set_default(k, AP.a[i].id);
                    def_open = -1;
                }
                y += 40;
            }
            y += 6;
        }
    }
    y += note_text(c, y, "Used when an app opens a link or a file. Files you open from Files open in this "
                         "system's own apps where it has one (Notes for text, the viewer for pictures).");
    return y - y0 + 10;
}

/* ---- Storage ---- */

/* What uses the disk, measured in a process of its own (a walk through every app's files takes a while). */
static long long walked;
static int walk_add(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)p, (void)flag, (void)f;
    walked += (long long)st->st_blocks * 512;
    return 0;
}

static long long tree_size(const char *path) {
    walked = 0;
    nftw(path, walk_add, 32, FTW_PHYS | FTW_MOUNT);
    return walked;
}

#define STORE_KINDS 5
static const char *const store_label[STORE_KINDS] = {"Your files", "Apps", "AI models", "Trash", "Caches"};

static void measure_child(void *arg, char **result) {
    (void)arg;
    long long v[STORE_KINDS];
    long long home = tree_size(home_dir()), trash = tree_size(TRASH_DIR), cache = tree_size(home_path("/.cache"));
    long long their_apps = tree_size(home_path("/.local/share/flatpak")) + tree_size(home_path("/.var"));
    v[0] = home - trash - cache - their_apps;
    v[1] = tree_size("/var/lib/os-data/flatpak") + their_apps;
    v[2] = tree_size("/var/lib/os-data/models");
    v[3] = trash;
    v[4] = cache + tree_size("/var/lib/os-data/figure-cache");
    char *out = NULL;
    if (asprintf(&out, "%lld %lld %lld %lld %lld", v[0] < 0 ? 0 : v[0], v[1], v[2], v[3], v[4]) < 0)
        out = NULL;
    *result = out ? out : strdup("");
}

static struct {
    job *measuring;
    long long v[STORE_KINDS];
    bool known;
    uint64_t at;
} ST;

static void bytes_text(long long b, char *out, size_t n) {
    if (b >= 1000LL * 1000 * 1000)
        snprintf(out, n, "%.1f GB", b / 1e9);
    else if (b >= 1000LL * 1000)
        snprintf(out, n, "%.0f MB", b / 1e6);
    else if (b >= 1000)
        snprintf(out, n, "%.0f KB", b / 1e3);
    else
        snprintf(out, n, "%lld bytes", b);
}

static int page_storage(rect c, int y) {
    int y0 = y;
    font *f = BODY();
    char *res;
    if (ST.measuring && job_poll(ST.measuring, &res)) {
        job_free(ST.measuring);
        ST.measuring = NULL;
        ST.known = res && sscanf(res, "%lld %lld %lld %lld %lld", &ST.v[0], &ST.v[1], &ST.v[2], &ST.v[3], &ST.v[4]) == 5;
        free(res);
        if (ST.known)
            LOG("settings: storage measured: files %lld, apps %lld, models %lld, trash %lld, caches %lld\n", ST.v[0],
                ST.v[1], ST.v[2], ST.v[3], ST.v[4]);
    }
    /* measured on opening the page, and again after a minute shown */
    if (!ST.measuring && (!ST.at || A.now - ST.at > 60000)) {
        ST.at = A.now;
        ST.measuring = job_start(measure_child, NULL);
    }
    struct statvfs sv;
    bool disk = statvfs("/var/lib/os-data", &sv) == 0 && sv.f_blocks && access("/var/lib/os-data/.theirs", F_OK) == 0;
    y = settings_section(c, y, "This computer's disk");
    if (!disk) {
        y += note_text(c, y, "There is no disk for the system here: what you do is kept only until it shuts down.");
        return y - y0 + 10;
    }
    double total = (double)sv.f_blocks * sv.f_frsize, freeb = (double)sv.f_bavail * sv.f_frsize;
    char a[32], b[32], line[120];
    bytes_text((long long)(total - freeb), a, sizeof a);
    bytes_text((long long)total, b, sizeof b);
    snprintf(line, sizeof line, "%s used of %s", a, b);
    draw_text(A.frame, f, c.x, y, line, P.ink);
    bytes_text((long long)freeb, a, sizeof a);
    snprintf(line, sizeof line, "%s free", a);
    draw_text(A.frame, f, c.x + c.w - text_width(f, line), y, line, P.muted);
    y += LH(f) + 10;
    /* the bar: each kind its share, the rest the system's */
    rect bar = {c.x, y, c.w, 14};
    fill_round(A.frame, bar, 7, color_alpha(P.ink, 0.08f));
    static const float shade[STORE_KINDS] = {1.0f, 0.75f, 0.55f, 0.38f, 0.25f};
    int x = bar.x;
    for (int i = 0; ST.known && i < STORE_KINDS; i++) {
        int w = (int)(ST.v[i] / total * bar.w);
        if (w > 0) {
            fill_rect(A.frame, (rect){x, bar.y, w, bar.h}, color_alpha(P.accent, shade[i]));
            x += w;
        }
    }
    y += 14 + 20;
    for (int i = 0; i < STORE_KINDS; i++) {
        fill_round(A.frame, (rect){c.x, y + 15, 12, 12}, 3, color_alpha(P.accent, shade[i]));
        draw_text(A.frame, f, c.x + 22, y + (42 - LH(f)) / 2, store_label[i], P.ink);
        if (ST.known && (i == 3 || i == 4) && ST.v[i] <= 4096) /* (the folders themselves) */
            snprintf(a, sizeof a, "Empty");
        else if (ST.known)
            bytes_text(ST.v[i], a, sizeof a);
        else
            snprintf(a, sizeof a, "Measuring...");
        int bw = 0;
        if (i == 3 || i == 4) { /* the two that can go */
            const char *lab = i == 3 ? "Empty Trash" : "Clear";
            bw = text_width(f, lab) + 32;
            rect br = {c.x + c.w - bw, y + 2, bw, 36};
            if (log_now)
                LOGRECT(i == 3 ? "storage-trash" : "storage-caches", br);
            if (ctl_button(br, lab, f, false, ST.known && ST.v[i] > 4096, &P)) {
                if (i == 3)
                    files_empty_trash();
                else {
                    files_erase_tree(home_path("/.cache"));
                    mkdir(home_path("/.cache"), 0700);
                    LOG("settings: caches cleared\n");
                }
                ST.at = 0; /* measured again */
            }
            bw += 16;
        }
        draw_text(A.frame, f, c.x + c.w - bw - text_width(f, a), y + (42 - LH(f)) / 2, a, P.muted);
        y += 46;
    }
    y += 8;
    y += note_text(c, y, "Apps are removed in Settings > Apps. Caches are only copies apps keep to be faster: "
                         "clearing them loses nothing of yours.");
    return y - y0 + 10;
}

/* ---- Power ---- */

static int minutes_choice(rect c, int y, const char *label, const char *key, int def, int id) {
    static const char *const lab[] = {"Never", "5 min", "10 min", "15 min", "30 min", "1 hour"};
    static const int val[] = {0, 5, 10, 15, 30, 60};
    int cur = 0, now = settings_get_int(key, def);
    for (int i = 0; i < 6; i++)
        if (val[i] == now)
            cur = i;
    draw_text(A.frame, BODY(), c.x, y + 9, label, P.muted);
    rect sr = {c.x + 170, y, 480 < c.w - 170 ? 480 : c.w - 170, 38};
    if (log_now) {
        char nm[32];
        snprintf(nm, sizeof nm, "power-%d", id);
        LOGRECT(nm, sr);
    }
    int pick = ctl_segmented(sr, lab, 6, cur, BODY(), &P);
    if (pick != cur) {
        settings_set_int(key, val[pick]);
        LOG("settings: %s %d\n", key, val[pick]);
    }
    return y + 54;
}

static int page_power(rect c, int y) {
    int y0 = y;
    y = settings_section(c, y, "When you're away");
    y = minutes_choice(c, y, "Screen off after", "screen_off", 10, 0);
    y = minutes_choice(c, y, "Lock after", "lock_after", 0, 1);
    y += note_text(c, y, "Never while something plays full screen. Any key or a move of the mouse brings the screen "
                         "back; with a password, the lock screen asks for it.");
    y += 22;
    y = settings_section(c, y, "Turning off");
    rect off = {c.x, y, 140, 40}, re = {c.x + 152, y, 140, 40};
    if (ctl_button(off, "Shut down", BODY(), false, true, &P))
        A.confirm_power = true, A.power_restart = false;
    if (ctl_button(re, "Restart", BODY(), false, true, &P))
        A.confirm_power = true, A.power_restart = true;
    y += 52;
    y += note_text(c, y, "What you were doing is kept first: the windows you had open come back, and your work in "
                         "Notes is already saved.");
    return y - y0 + 10;
}

/* ---- Mouse ---- */

static int page_mouse(rect c, int y) {
    int y0 = y;
    font *f = BODY();
    y = settings_section(c, y, "Pointer");
    int sp = settings_get_int("mouse_speed", 100);
    char val[32];
    row_label(c, y, 40, "Speed", NULL);
    snprintf(val, sizeof val, sp == 100 ? "Normal" : "%d%%", sp);
    draw_text(A.frame, f, c.x + c.w - text_width(f, val), y + (40 - LH(f)) / 2, val, P.muted);
    float t = (sp - 20) / 280.0f;
    rect sl = {c.x + 230, y + 9, c.w - 230 - 90, 22};
    if (log_now)
        LOGRECT("mouse-speed", sl);
    if (ctl_slider(2201, sl, &t, &P) != CTL_IDLE) {
        sp = 20 + (int)lroundf(t * 280 / 10) * 10;
        if (sp > 90 && sp < 110)
            sp = 100; /* it snaps to normal */
        settings_set_int("mouse_speed", sp);
        mouse_apply();
    }
    y += 50;
    struct {
        const char *key, *label, *sub;
        int def;
    } rows[] = {
        {"mouse_accel", "Acceleration", "Fast moves go further, slow ones stay precise", 1},
        {"mouse_natural", "Natural scrolling", "The content moves the way your fingers do, as on a phone", 0},
        {"mouse_left", "Left-handed", "The left and right buttons swap", 0},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        row_label(c, y, 48, rows[i].label, rows[i].sub);
        bool on = settings_get_int(rows[i].key, rows[i].def) != 0;
        rect sw = {c.x + c.w - 52, y + 12, 48, 26};
        if (log_now) {
            char nm[40];
            snprintf(nm, sizeof nm, "%s", rows[i].key);
            for (char *p = nm; *p; p++)
                if (*p == '_')
                    *p = '-';
            LOGRECT(nm, sw);
        }
        if (ctl_switch(sw, on, &P)) {
            settings_set_int(rows[i].key, !on);
            mouse_apply();
            LOG("settings: %s %s\n", rows[i].key, !on ? "on" : "off");
        }
        y += 60;
    }
    if (input_pointer_absolute())
        y += note_text(c, y, "This pointer is a tablet's or a virtual machine's: it goes where it points, so speed and "
                             "acceleration don't change it. Scrolling and the buttons still follow these.");
    return y - y0 + 10;
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
    mind_fallback_forget(); /* (their choice: no stand-in to go back from) */
    ai_config_save(&A.cfg);
    LOG("mind %s (%s), chosen in Settings\n", A.provider->id, A.cfg.model);
    char b[200];
    snprintf(b, sizeof b, "%s now thinks with %s%s%s.", D.ai_name[0] ? D.ai_name : "Your companion", mind.p->name,
             A.cfg.model[0] ? ", " : "", A.cfg.model);
    notify("AI model changed", b, "settings");
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
    static bool typing; /* only once the field is clicked: a name is never changed by keys meant elsewhere */
    if (A.in.clicked)
        typing = ui_hover(tf);
    ui_textfield(tf, &nm, typing, "Their name", f, color_alpha(P.ink, 0.06f), P.ink, P.muted, 12);
    rect b = {tf.x + tf.w + 12, *y, 130, 40};
    if (log_now)
        LOGRECT("settings-name", tf);
    bool enter = typing && ui_type(&nm);
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
    if (mind.step == MIND_SHOW) { /* how I look: made in conversation, never picked from a list */
        y = settings_section(c, y, "Companion appearance");
        rect me = {c.x, y, 96, 96};
        bool shown = presence_mode() != PRESENCE_LIGHT
                         ? false
                         : emblem_draw(A.frame, me.x + 48.0f, me.y + 48.0f, 36, D.accent, D.accent2,
                                       A.companion.pending != NULL);
        int tx = shown ? me.x + me.w + 20 : c.x;
        const char *says = presence_mode() != PRESENCE_LIGHT
                               ? "I have a face now. Tell me if you'd like me to look different: describe it, give me a "
                                 "picture, a link, a 3D model or a VRM, and I'll take it on."
                           : shown ? "This is how I look right now. Tell me if you'd like something else: describe it, "
                                     "give me a picture, a link, a 3D model or a VRM, and I'll take it on."
                                   : "I don't have a look of my own yet. Tell me what you'd like to see, or ask me to "
                                     "come up with something that suits your desktop.";
        int th = draw_text_wrapped(A.frame, f, (rect){tx, y + 4, c.x + c.w - tx, 120}, says, P.ink, 1.35f);
        rect b = {tx, y + 4 + th + 12, 180, 40};
        if (ctl_button(b, "Change it", f, true, true, &P)) {
            desktop_open_window(WIN_COMPANION);
            body_talk_about_it(false);
        }
        int used = th + 12 + 40 + 8;
        y += (shown && used < me.h ? me.h : used) + 24;
    }
    if (mind.step == MIND_SHOW) {
        rect card = {c.x, y, c.w, 112};
        fill_round(A.frame, card, 18, color_alpha(P.ink, 0.05f));
        font *big = settings_font(SF_SECTION);
        const char *name = A.provider ? A.provider->name : A.cfg.provider[0] ? A.cfg.provider : "Not set up";
        draw_text(A.frame, fm_host_font(D.font_heading, 22), card.x + 24, card.y + 24, name, P.ink);
        bool here = A.provider && !strcmp(A.provider->id, "builtin");
        draw_text(A.frame, f, card.x + 24, card.y + 30 + LH(big) + 6,
                  here ? models_selected()->name : A.cfg.model[0] ? A.cfg.model : "No model chosen", P.muted);
        y += card.h + 16;
        y += draw_text_wrapped(A.frame, f, (rect){c.x, y, c.w, 200},
                               is_local() ? "Runs on this computer (or one you run). Nothing is sent elsewhere."
                                          : "Conversations are sent to this provider. Nothing else is.",
                               P.ink, 1.35f) + 18;
        rect b = {c.x, y, 200, 40};
        if (log_now)
            LOGRECT("settings-mind-change", b);
        if (ctl_button(b, "Change provider", f, true, true, &P)) {
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
        y += note_text(c, y, "Changing the provider keeps your desktop, profile and conversation history.");
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
    y = settings_section(c, y, "Model");
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
    if (ctl_button(use, "Use this model", f, true, true, &P))
        mind_use(mind.models[mind.sel]);
    y += 42 + 10;
    return y - y0;
}

/* ---- Privacy ---- */

/* Security: only ever reached by clicking it here (appgate.c) */
static int page_security(rect c, int y) { return security_page(c.x, y, c.w, c.h); }

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
    y += note_text(c, y, "Stored on this computer only.") + 24;
    y = settings_section(c, y, "AI provider");
    snprintf(line, sizeof line, "%s, %s. %s", A.provider ? A.provider->name : A.cfg.provider, A.cfg.model,
             is_local() ? "Runs locally." : "Conversations are sent to this provider.");
    y += draw_text_wrapped(A.frame, f, (rect){c.x, y, c.w, 200}, line, P.ink, 1.3f) + 8;
    snprintf(line, sizeof line, "Speaks up: %s. Redesigns: %s.",
             A.prof.autonomy[0] ? A.prof.autonomy : "asks first", A.prof.redesign[0] ? A.prof.redesign : "only when asked");
    y += note_text(c, y, line) + 24;
    /* its reflexes (reflex.c): what decides, on this computer, what is worth its attention */
    y = settings_section(c, y, "Event filter");
    {
        static const char *const engines_l[] = {"Decision model", "Small LLM", "Rules only"};
        static const char *const engines_v[] = {"jev", "llm", "rules"};
        const char *cur = reflex_engine();
        int ci = !strcmp(cur, "llm") ? 1 : !strcmp(cur, "rules") ? 2 : 0;
        int pick = ctl_segmented((rect){c.x, y, 420, 38}, engines_l, 3, ci, BODY(), &P);
        if (pick != ci)
            settings_set("reflex.engine", engines_v[pick]);
        y += 38 + 10;
        if (ci != 2) {
            static const char *const models_l[] = {"LFM3 1.7B", "Qwen3 1.7B", "Qwen3 0.6B"};
            static const char *const models_v[] = {"lfm3-1.7b", "qwen3-1.7b", "qwen3-0.6b"};
            const char *m = settings_get("reflex.model", "lfm3-1.7b");
            int mi = !strcmp(m, "qwen3-1.7b") ? 1 : !strcmp(m, "qwen3-0.6b") ? 2 : 0;
            int mp = ctl_segmented((rect){c.x, y, 420, 34}, models_l, 3, mi, SMALL(), &P);
            if (mp != mi)
                settings_set("reflex.model", models_v[mp]);
            y += 34 + 10;
        }
        y += note_text(c, y, reflex_status()) + 4;
        y += note_text(c, y, "Decides which system events are worth interrupting for. Runs on this computer only, "
                             "never on the AI provider. Your own actions are never reported back to you.") + 24;
    }
    /* apps that asked to use the companion (the companion API), and what each may */
    if (appapi_app_count()) {
        y = settings_section(c, y, "App access");
        for (int i = 0; i < appapi_app_count(); i++) {
            draw_text(A.frame, f, c.x, y + 8, appapi_app_label(i), P.ink);
            static const char *const what[2] = {"Your AI", "What I know about you"};
            int x = c.x + 180;
            for (int k = 0; k < 2; k++) {
                bool on = appapi_app_allows(i, k == 1) == 1;
                draw_text(A.frame, small, x, y + 10, what[k], P.muted);
                rect sw = {x + text_width(small, what[k]) + 10, y + 4, 46, 28};
                if (ctl_switch(sw, on, &P))
                    appapi_app_set(i, k == 1, !on);
                x = sw.x + sw.w + 28;
            }
            y += 44;
        }
        y += note_text(c, y, "Apps use the AI through this computer and never see your keys.") + 24;
    }
    y = settings_section(c, y, "Reset");
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
    y += note_text(c, y, "Forget me erases your profile, design, notes and AI connection, then restarts setup.") + 18;
    {
        static uint64_t clear_at;
        bool clearing = clear_at && A.now - clear_at < 5000;
        rect cb = {c.x, y, clearing ? 300 : 200, 42};
        if (ctl_button(cb, clearing ? "Click again to delete them" : "Delete all chats", f, clearing, true, &P)) {
            if (clearing) {
                clear_at = 0;
                chats_delete_all();
            } else {
                clear_at = A.now;
            }
        }
        y += 42 + 12;
        char what[220];
        snprintf(what, sizeof what, "Deletes every chat you've had with %s, the one open too. What %s remembers about "
                 "you stays.", D.ai_name[0] ? D.ai_name : "the companion", D.ai_name[0] ? D.ai_name : "it");
        y += note_text(c, y, what) + 18;
    }
    static uint64_t reset_at;
    bool resetting = reset_at && A.now - reset_at < 5000;
    rect rb = {c.x, y, resetting ? 300 : 200, 42};
    palette red = P;
    if (resetting) {
        red.accent = RGBA(200, 70, 60, 255);
        red.on_accent = RGBA(255, 255, 255, 255);
    }
    if (log_now)
        LOGRECT("settings-factory-reset", rb);
    if (ctl_button(rb, resetting ? "Click again to erase everything" : "Reset to factory", f, resetting, true, &red)) {
        if (resetting) {
            reset_at = 0;
            factory_reset();
            return 0;
        }
        reset_at = A.now;
    }
    y += 42 + 12;
    y += note_text(c, y, "Reset to factory erases all data on this computer (profile, conversations, design, files "
                         "and installed apps) and restarts from the first screen.");
    return y - y0 + 10;
}

/* ---- About ---- */

static int page_about(rect c, int y) {
    int y0 = y;
    static uint64_t read_at;
    static char cpu[96], mem[64], up[64], kern[96], disk[96];
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
        struct statvfs sv; /* where their apps and models are kept */
        if (statvfs("/var/lib/os-data", &sv) == 0 && sv.f_blocks && access("/var/lib/os-data/.theirs", F_OK) == 0)
            snprintf(disk, sizeof disk, "%.0f GB free of %.0f GB", (double)sv.f_bavail * sv.f_frsize / 1e9,
                     (double)sv.f_blocks * sv.f_frsize / 1e9);
        else
            snprintf(disk, sizeof disk, "No disk: nothing is kept after it shuts down");
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
    } rows[] = {{"Processor", buf},
                {"Memory", mem},
                {"Graphics", "Drawn by the processor (no graphics driver yet)"},
                {"Storage", disk},
                {"Display", disp},
                {"Network", n->has_ip ? n->ip : n->link ? "Connecting" : "Offline"},
                {"Kernel", kern},
                {"Up for", up}};
    for (int i = 0; i < (int)(sizeof rows / sizeof rows[0]); i++) {
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

/* Date & time: the time and the time zone, each automatic or chosen, and the 12- or 24-hour clock (clock.c). */
static struct textfield zone_q;
static int zone_focus, zone_scroll;
static char sync_msg[160];

static int page_time(rect c, int y) {
    int y0 = y;
    font *f = BODY(), *big = settings_font(SF_SECTION);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char now[64], day[64];
    fm_strftime(now, sizeof now, "%H:%M:%S", &tm);
    fm_strftime(day, sizeof day, "%A %e %B %Y", &tm);
    draw_text(A.frame, big, c.x, y, now, P.ink);
    y += LH(big) + 2;
    char line[160];
    snprintf(line, sizeof line, "%s  \xc2\xb7  %s", day[0] == ' ' ? day + 1 : day, clock_zone());
    draw_text(A.frame, f, c.x, y, line, P.muted);
    y += LH(f) + 22;

    y = settings_section(c, y, "Time");
    row_label(c, y, 48, "Set the time automatically", "From time servers on the internet, at the start and every six hours");
    rect sw = {c.x + c.w - 52, y + 12, 48, 26};
    if (log_now)
        LOGRECT("time-auto", sw);
    if (ctl_switch(sw, clock_auto_time(), &P))
        clock_set_auto_time(!clock_auto_time());
    y += 58;
    rect sb = {c.x, y, 140, 38};
    if (ctl_button(sb, "Set it now", f, false, priv_available(), &P)) {
        clock_sync_now();
        snprintf(sync_msg, sizeof sync_msg, "Asking...");
    }
    if (log_now)
        LOGRECT("time-sync", sb);
    char m[160];
    int r = clock_sync_result(m, sizeof m);
    if (r == 1)
        snprintf(sync_msg, sizeof sync_msg, "Set %s.", m);
    else if (r == -1)
        snprintf(sync_msg, sizeof sync_msg, "Not set: %s.", m);
    if (sync_msg[0])
        draw_text(A.frame, SMALL(), sb.x + sb.w + 16, sb.y + (38 - LH(SMALL())) / 2, sync_msg, P.muted);
    y += 38 + 14;
    row_label(c, y, 48, "24-hour clock", clock_12h() ? "Now 3:05 pm" : "Now 15:05");
    rect hs = {c.x + c.w - 52, y + 12, 48, 26};
    if (log_now)
        LOGRECT("time-24h", hs);
    if (ctl_switch(hs, !clock_12h(), &P))
        clock_set_12h(!clock_12h());
    y += 64;

    y = settings_section(c, y, "Time zone");
    const char *st = clock_auto_status();
    row_label(c, y, 48, "Set the time zone automatically",
              clock_auto_zone() ? (st[0] ? st : "From where this computer is on the internet") : "Chosen below");
    rect zs = {c.x + c.w - 52, y + 12, 48, 26};
    if (log_now)
        LOGRECT("time-zone-auto", zs);
    if (ctl_switch(zs, clock_auto_zone(), &P))
        clock_set_auto_zone(!clock_auto_zone());
    y += 58;
    y += note_text(c, y, "Looking it up sends this computer's internet address to ipapi.co (or ip-api.com, or "
                         "worldtimeapi.org, if it doesn't answer). Choosing one below turns this off.");
    y += 12;
    /* the zones: searched by place or name */
    const struct clock_zone_entry *z;
    int nz = clock_zones(&z);
    rect qf = {c.x, y, c.w < 420 ? c.w : 420, 40};
    if (A.in.clicked)
        zone_focus = pt_in(qf, A.in.mx, A.in.my);
    ui_textfield(qf, &zone_q, zone_focus, "Search: a city, a country or a zone", f, color_alpha(P.ink, 0.06f), P.ink,
                 P.muted, 10);
    if (log_now)
        LOGRECT("time-zone-search", qf);
    if (zone_focus)
        ui_type(&zone_q);
    y += 50;
    int shown = 0, rowh = 40;
    const char *q = zone_q.buf;
    char qq[64]; /* "new york" finds America/New_York */
    snprintf(qq, sizeof qq, "%s", q);
    for (char *p = qq; *p; p++)
        if (*p == ' ')
            *p = '_';
    for (int i = 0; i < nz && shown < 8; i++) {
        bool cur = !strcmp(z[i].name, clock_zone());
        if (q[0] ? !(strcasestr(z[i].name, qq) || strcasestr(z[i].where, q)) : !cur)
            continue;
        rect row = {c.x, y, c.w, rowh - 4};
        bool hover = pt_in(row, A.in.mx, A.in.my);
        if (cur || hover)
            fill_round(A.frame, row, 8, color_alpha(cur ? P.accent : P.ink, cur ? 0.16f : 0.05f));
        char nm[64];
        snprintf(nm, sizeof nm, "%s", z[i].name);
        for (char *p = nm; *p; p++)
            if (*p == '_')
                *p = ' ';
        draw_text(A.frame, f, row.x + 12, row.y + (row.h - LH(f)) / 2, nm, P.ink);
        /* its time now, and where */
        char tzwas[64];
        snprintf(tzwas, sizeof tzwas, "%s", clock_zone());
        setenv("TZ", z[i].name, 1);
        tzset();
        struct tm zt;
        localtime_r(&t, &zt);
        setenv("TZ", tzwas, 1);
        tzset();
        char zh[32], right[120];
        fm_strftime(zh, sizeof zh, "%H:%M", &zt);
        snprintf(right, sizeof right, "%s  \xc2\xb7  %s", z[i].where, zh);
        int rw = text_width(SMALL(), right);
        draw_text(A.frame, SMALL(), row.x + row.w - 12 - rw, row.y + (row.h - LH(SMALL())) / 2, right, P.muted);
        if (log_now && shown < 4) {
            char rn[32];
            snprintf(rn, sizeof rn, "time-zone-row-%d", shown);
            LOGRECT(rn, row);
        }
        if (hover && A.in.clicked && !cur) {
            clock_set_zone(z[i].name);
            zone_q.buf[0] = 0, zone_q.len = 0;
            te_reset(&zone_q.ed, zone_q.buf);
            zone_focus = 0;
        }
        y += rowh;
        shown++;
    }
    if (q[0] && !shown)
        y += note_text(c, y, "No time zone by that name.");
    (void)zone_scroll;
    return y - y0 + 10;
}

/* Users: their account, its password (optional, init keeps it: lock.c), when it locks. */
static struct textfield pw_old = {.secret = true}, pw_new = {.secret = true}, pw_again = {.secret = true};
static int pw_focus;
static char pw_msg[120];

static int page_users(rect c, int y) {
    int y0 = y;
    font *f = BODY();
    y = settings_section(c, y, "Account");
    char line[200];
    snprintf(line, sizeof line, "%s  \xc2\xb7  %s  \xc2\xb7  %s", D.user_name[0] ? D.user_name : account_name(), account_name(),
             home_dir());
    draw_text(A.frame, f, c.x, y, line, P.ink);
    y += LH(f) + 4;
    y += note_text(c, y, "The account is named after the name you gave; a new name takes effect at the next start.");
    y += 18;

    y = settings_section(c, y, "Password");
    bool has = lock_has_password();
    y += note_text(c, y, has ? "It asks for your password when it starts and to open the lock screen."
                             : "No password: anyone at this computer can use it. With one, it asks for it when it starts "
                               "and to open the lock screen.");
    y += 10;
    int fw = c.w < 360 ? c.w : 360, k = 0;
    struct textfield *fields[3] = {&pw_old, &pw_new, &pw_again};
    const char *hints[3] = {"Current password", has ? "New password (empty: none)" : "New password", "The new one again"};
    for (int i = has ? 0 : 1; i < 3; i++, k++) {
        rect r = {c.x, y, fw, 44};
        if (A.in.clicked && pt_in(r, A.in.mx, A.in.my))
            pw_focus = i;
        if (pw_focus < (has ? 0 : 1))
            pw_focus = has ? 0 : 1;
        ui_textfield(r, fields[i], pw_focus == i, hints[i], f, color_alpha(P.ink, 0.06f), P.ink, P.muted, 10);
        if (log_now)
            LOGRECT(i == 0 ? "users-old" : i == 1 ? "users-new" : "users-again", r);
        if (pw_focus == i)
            ui_type(fields[i]);
        y += 52;
    }
    char why[48];
    int res = lock_password_result(why, sizeof why);
    if (res == 1) {
        snprintf(pw_msg, sizeof pw_msg, "%s", pw_new.buf[0] ? "Password set." : "Password taken away.");
        memset(pw_old.buf, 0, sizeof pw_old.buf), memset(pw_new.buf, 0, sizeof pw_new.buf);
        memset(pw_again.buf, 0, sizeof pw_again.buf);
    } else if (res == -1) {
        snprintf(pw_msg, sizeof pw_msg, "%s", !strncmp(why, "wrong", 5) ? "The current password isn't right."
                                         : !strncmp(why, "wait", 4) ? "Too many tries: wait a little."
                                                                       : "It couldn't be kept.");
    }
    const char *label = !has ? "Set password" : pw_new.buf[0] ? "Change password" : "Take the password away";
    bool same = !strcmp(pw_new.buf, pw_again.buf);
    bool ready = same && (has || pw_new.buf[0]) && (!has || pw_old.buf[0]);
    rect setr = {c.x, y, text_width(f, label) + 40, 40};
    if (log_now)
        LOGRECT("users-set", setr);
    if (ctl_button(setr, label, f, true, ready, &P)) {
        lock_password_change(has ? pw_old.buf : "", pw_new.buf);
        pw_msg[0] = 0;
    }
    if (!same && pw_again.buf[0])
        draw_text(A.frame, f, c.x + text_width(f, label) + 60, y + 9, "The two new ones differ.", P.muted);
    else if (pw_msg[0])
        draw_text(A.frame, f, c.x + text_width(f, label) + 60, y + 9, pw_msg, P.muted);
    y += 58;

    y = settings_section(c, y, "Lock");
    static const char *const after_l[] = {"Never", "5 min", "15 min", "30 min", "1 hour"};
    static const int after_v[] = {0, 5, 15, 30, 60};
    int cur = 0, now = settings_get_int("lock_after", 0);
    for (int i = 0; i < 5; i++)
        if (after_v[i] == now)
            cur = i;
    draw_text(A.frame, f, c.x, y + 9, "Lock after", P.muted);
    int pick = ctl_segmented((rect){c.x + 120, y, 420 < c.w - 120 ? 420 : c.w - 120, 38}, after_l, 5, cur, f, &P);
    if (pick != cur)
        settings_set_int("lock_after", after_v[pick]);
    y += 54;
    if (ctl_button((rect){c.x, y, 160, 40}, "Lock now", f, false, true, &P))
        lock_now();
    y += 40 + 10;
    y += note_text(c, y, "Super+L locks it at any time.");
    return y - y0 + 10;
}

/* The About window: the same page, on its own. */
void settings_about_draw(rect r) {
    P = pal_on(pal_sample(r), 1);
    page_about(rect_inset(r, 14), r.y + 18);
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
                                         {"Mouse", SYM_SLIDERS, page_mouse, NULL},
                                         {"Date & time", SYM_CALENDAR, page_time, NULL},
                                         {"AI", SYM_MIND, page_mind, NULL}};
    static const struct page after[] = {{"Users", SYM_SHIELD, page_users, NULL},
                                        {"Notifications", SYM_BELL, page_notify, NULL},
                                        {"Apps", SYM_GEAR, page_apps, NULL},
                                        {"Power", SYM_MOON, page_power, NULL},
                                        {"Storage", SYM_INFO, page_storage, NULL},
                                        {"Security", SYM_SHIELD, page_security, NULL},
                                        {"Privacy", SYM_SHIELD, page_privacy, NULL},
                                        {"About", SYM_INFO, page_about, NULL}};
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
    if (title && strcasecmp(title, "Security")) /* never opened onto Security by anything but their click there */
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
    struct page pages[16 + MAX_EXTRA];
    /* Settings shown again after a while unseen (opened by them, an app, a design or the companion, or brought back):
     * never straight onto Security. It is drawn every frame while it shows, so a gap means it was away. */
    static uint64_t last_drawn;
    bool again = !last_drawn || A.now - last_drawn > 400;
    last_drawn = A.now;
    if (again) {
        if (!strcmp(current, "Security"))
            snprintf(current, sizeof current, "Appearance");
        LOG("settings: opened on %s\n", current);
    }
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
