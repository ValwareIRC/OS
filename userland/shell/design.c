#define _GNU_SOURCE
#include "design.h"
#include "../lib/json.h"
#include "font.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const app_ids[APP_COUNT] = {"files", "companion", "notes", "settings", "about", "terminal", "redesign", "power"};
static const char *const app_names[APP_COUNT] = {"Files", "Companion", "Notes", "Settings", "About", "Terminal", "Redesign",
                                                 "Power off"};

const char *app_default_name(int app) { return app >= 0 && app < APP_COUNT ? app_names[app] : ""; }
const char *app_id(int app) { return app >= 0 && app < APP_COUNT ? app_ids[app] : ""; }

static int lookup(const char *s, const char *const *names, int n, int fallback) {
    if (!s)
        return fallback;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(s, names[i]))
            return i;
    return fallback;
}

static const char *const wp_names[WP_COUNT] = {"gradient", "aurora", "waves", "bokeh", "dunes", "sun", "constellation", "paper",
                                               "nebula", "liquid", "flow", "prism"};
static const char *const icon_style_names[] = {"filled", "line", "glow", "duotone"};
static const char *const material_names[] = {"glass", "frost", "solid", "neon"};
static const char *const dock_names[] = {"bottom", "left", "top", "right"};
static const char *const dock_style_names[] = {"pill", "bar", "orbs"};
static const char *const icon_names[] = {"circle", "squircle", "square", "glyph"};
static const char *const menu_names[] = {"grid", "list", "sheet", "radial"};
static const char *const clock_names[] = {"large", "corner", "poetic", "hidden", "ring"};
static const char *const explorer_names[] = {"list", "icons", "columns"};
static const char *const companion_names[] = {"right", "left", "floating"};

static unsigned hash(const char *s) {
    unsigned h = 2166136261u;
    for (; s && *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

/* Ensures readable text: if contrast is too low, push the text colour
 * toward white or black (whichever the background allows). */
static color readable(color fg, color bg, float min) {
    if (color_contrast(fg, bg) >= min)
        return fg;
    color target = color_luma(bg) > 0.35f ? RGBA(14, 12, 16, 255) : RGBA(250, 248, 245, 255);
    for (float t = 0.1f; t <= 1.0f; t += 0.1f) {
        color c = color_mix(fg, target, t);
        if (color_contrast(c, bg) >= min)
            return c;
    }
    return target;
}

static bool font_installed(const char *family) {
    int n;
    const char *const *names = font_families(&n);
    for (int i = 0; i < n; i++)
        if (!strcasecmp(names[i], family))
            return true;
    return false;
}

static color hsv(float h, float s, float v) {
    float c = v * s, hp = h / 60.0f, x = c * (1 - fabsf(fmodf(hp, 2) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    switch ((int)hp % 6) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    return RGBA((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255), 255);
}

static void default_sections(design *d) {
    d->nsections = 2;
    copy(d->sections[0].title, sizeof d->sections[0].title, "");
    int a[] = {APP_COMPANION, APP_FILES, APP_NOTES, APP_SETTINGS};
    memcpy(d->sections[0].items, a, sizeof a);
    d->sections[0].count = 4;
    copy(d->sections[1].title, sizeof d->sections[1].title, "");
    int b[] = {APP_REDESIGN, APP_ABOUT, APP_POWER};
    memcpy(d->sections[1].items, b, sizeof b);
    d->sections[1].count = 3;
}

void design_from_profile(design *d, const profile *p) {
    memset(d, 0, sizeof *d);
    unsigned h = hash(p->notes) ^ hash(p->mood) ^ hash(p->palette_name);
    color pal[4];
    if (p->has_palette) {
        memcpy(pal, p->palette, sizeof pal);
    } else {
        /* Derive a palette from what was said, rather than a stock one. */
        float hue = (float)(h % 360);
        pal[0] = hsv(hue, 0.45f, 0.16f);
        pal[1] = hsv(fmodf(hue + 25, 360), 0.6f, 0.85f);
        pal[2] = hsv(fmodf(hue + 50, 360), 0.45f, 0.95f);
        pal[3] = hsv(fmodf(hue + 200, 360), 0.4f, 0.35f);
    }
    float l0 = color_luma(pal[0]);
    bool dark = l0 < 0.4f;
    d->bg = pal[0];
    d->bg2 = pal[3];
    d->accent = pal[1];
    d->accent2 = pal[2];
    d->surface = dark ? color_mix(pal[0], RGBA(255, 255, 255, 255), 0.08f) : color_mix(pal[0], RGBA(255, 255, 255, 255), 0.6f);
    d->text = dark ? RGBA(246, 240, 234, 255) : RGBA(28, 24, 26, 255);
    d->surface_text = d->text;
    d->muted = color_mix(d->text, d->bg, 0.4f);
    d->surface_opacity = 0.72f;
    d->radius = 10 + (float)(h % 14);
    d->blur = 10;
    d->shadows = true;
    copy(d->font_heading, sizeof d->font_heading, h & 1 ? "Fraunces" : "Outfit");
    copy(d->font_body, sizeof d->font_body, "Inter");
    d->heading_px = 34;
    d->body_px = 17;
    d->wallpaper = (enum wallpaper_kind)(h % WP_COUNT);
    memcpy(d->wp, pal, sizeof pal);
    d->wp_seed = (int)(h % 1000);
    d->wp_intensity = 0.8f;
    /* Ways in, chosen from what was learned rather than a fixed recipe. */
    d->use_orb = true;
    d->use_dock = h / 3 % 3 != 0;
    d->use_menu = h / 5 % 2 == 0;
    d->use_command_bar = !d->use_menu;
    d->use_desktop_icons = h / 19 % 3 == 0;
    d->hot_corner = d->use_menu ? (int)(h / 23 % 4) : -1;
    d->orb_corner = 3;
    d->dock_menu_button = d->use_menu;
    d->ndock = 0;
    int dock_default[] = {APP_COMPANION, APP_FILES, APP_NOTES, APP_SETTINGS};
    for (int i = 0; i < 4; i++)
        d->dock_items[d->ndock++] = dock_default[i];
    d->ndesk = 0;
    int desk_default[] = {APP_FILES, APP_NOTES, APP_ABOUT};
    for (int i = 0; i < 3; i++)
        d->desk_items[d->ndesk++] = desk_default[i];
    d->desk_area = (int)(h / 29 % 4);
    d->bar_position = (int)(h / 31 % 3);
    copy(d->bar_placeholder, sizeof d->bar_placeholder, "Type what you need...");
    d->dock = (enum dock_pos)(h / 7 % 2 == 0 ? DOCK_BOTTOM : DOCK_LEFT);
    d->dock_style = (enum dock_style)(h / 11 % 3);
    d->icons = (enum icon_shape)(h / 13 % 3);
    d->menu = (enum menu_style)(h / 17 % 3);
    d->clock = (enum clock_style)(h / 37 % 2 ? CLOCK_RING : CLOCK_POETIC);
    d->icon_style = (enum icon_style)(h / 41 % 4);
    /* Things they asked for by name win over chance. */
    const char *said = p->notes;
    if (strcasestr(said, "dock")) {
        d->use_dock = true;
        d->dock = DOCK_BOTTOM;
    }
    if (strcasestr(said, "start menu") || strcasestr(said, "taskbar") || strcasestr(said, " XP") ||
        strcasestr(said, "windows 7") || strcasestr(said, "windows 10")) {
        d->use_menu = d->dock_menu_button = true;
        d->use_dock = true;
        d->dock = DOCK_BOTTOM;
    }
    if (strcasestr(said, "top bar") || strcasestr(said, "menu bar") || strcasestr(said, "gnome")) {
        d->use_command_bar = true;
        d->bar_position = 0;
    }
    if (strcasestr(said, "desktop icons") || strcasestr(said, "icons on the desktop"))
        d->use_desktop_icons = true;
    d->material = MAT_GLASS;
    d->glow = 0.6f;
    d->motion = 0.6f;
    d->explorer = EXPLORER_ICONS;
    d->companion = COMPANION_RIGHT;
    copy(d->ai_name, sizeof d->ai_name, p->ai_name[0] ? p->ai_name : "Companion");
    copy(d->user_name, sizeof d->user_name, p->user_name);
    snprintf(d->greeting, sizeof d->greeting, "%s%s%s", p->user_name[0] ? "Welcome home, " : "Welcome home",
             p->user_name, p->user_name[0] ? "." : ".");
    copy(d->menu_title, sizeof d->menu_title, d->ai_name);
    default_sections(d);
    for (int i = 0; i < APP_COUNT; i++) {
        copy(d->apps[i].label, sizeof d->apps[i].label, app_names[i]);
        snprintf(d->apps[i].glyph, sizeof d->apps[i].glyph, "%c", app_names[i][0]);
    }
    copy(d->apps[APP_COMPANION].label, sizeof d->apps[APP_COMPANION].label, d->ai_name);
    copy(d->rationale, sizeof d->rationale,
         "Built from your choices during setup, without the AI (it could not be reached).");
}

static const char *find_json_object(const char *text, size_t *len) {
    const char *start = strchr(text, '{');
    if (!start)
        return NULL;
    /* Match braces, respecting strings, to find the end of the object. */
    int depth = 0;
    bool in_str = false;
    for (const char *p = start; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == '"')
                in_str = false;
            continue;
        }
        if (*p == '"')
            in_str = true;
        else if (*p == '{')
            depth++;
        else if (*p == '}' && --depth == 0) {
            *len = (size_t)(p - start + 1);
            return start;
        }
    }
    return NULL;
}

static void note(char *problems, int len, const char *what) {
    if (!problems || len <= 0)
        return;
    size_t used = strlen(problems);
    snprintf(problems + used, (size_t)len - used, "%s%s", used ? "; " : "", what);
}

bool design_from_json(design *d, const char *text, const profile *p, char *problems, int plen) {
    if (problems && plen)
        problems[0] = 0;
    size_t len;
    const char *obj = find_json_object(text, &len);
    json *j = obj ? json_parse(obj, len) : NULL;
    design_from_profile(d, p); /* every field starts from the person's own choices */
    if (!j) {
        note(problems, plen, "no JSON object found");
        return false;
    }
    copy(d->rationale, sizeof d->rationale, json_str(json_get(j, "rationale"), ""));

#define STR(field, key)                                                                                  \
    do {                                                                                                 \
        const char *v_ = json_str(json_get(j, key), NULL);                                              \
        if (v_ && *v_)                                                                                   \
            copy(d->field, sizeof d->field, v_);                                                         \
    } while (0)
    STR(ai_name, "ai_name");
    STR(user_name, "user_name");
    /* A model may fill the name with a placeholder; better none than that. */
    if (!strcasecmp(d->user_name, "TheirName") || !strcasecmp(d->user_name, "User") ||
        !strcasecmp(d->user_name, "(not given)") || !strcasecmp(d->user_name, "Them") ||
        !strcasecmp(d->user_name, "Name") || !strcasecmp(d->user_name, "Unknown"))
        d->user_name[0] = 0;
    STR(greeting, "greeting");
    STR(tagline, "tagline");

    json *c = json_get(j, "colors");
    if (!c)
        note(problems, plen, "colors missing");
#define COL(field, key) d->field = color_parse(json_str(json_get(c, key), NULL), d->field)
    COL(bg, "background");
    COL(bg2, "background2");
    COL(surface, "surface");
    COL(surface_text, "surface_text");
    COL(text, "text");
    COL(muted, "muted");
    COL(accent, "accent");
    COL(accent2, "accent2");
    d->surface |= 0xff000000u;

    d->surface_opacity = (float)json_num(json_get(j, "surface_opacity"), d->surface_opacity);
    d->radius = (float)json_num(json_get(j, "corner_radius"), d->radius);
    d->blur = (float)json_num(json_get(j, "glass_blur"), d->blur);
    d->shadows = json_bool(json_get(j, "shadows"), d->shadows);
    d->material = (enum material)lookup(json_str(json_get(j, "material"), NULL), material_names, 4, d->material);
    d->glow = (float)json_num(json_get(j, "glow"), d->glow);
    d->motion = (float)json_num(json_get(j, "motion"), d->motion);

    json *f = json_get(j, "fonts");
    const char *fh = json_str(json_get(f, "heading"), NULL), *fb = json_str(json_get(f, "body"), NULL);
    if (fh) {
        if (font_installed(fh))
            copy(d->font_heading, sizeof d->font_heading, fh);
        else
            note(problems, plen, "heading font not installed");
    }
    if (fb) {
        if (font_installed(fb))
            copy(d->font_body, sizeof d->font_body, fb);
        else
            note(problems, plen, "body font not installed");
    }
    d->heading_px = (float)json_num(json_get(f, "heading_size"), d->heading_px);
    d->body_px = (float)json_num(json_get(f, "body_size"), d->body_px);

    json *w = json_get(j, "wallpaper");
    d->wallpaper = (enum wallpaper_kind)lookup(json_str(json_get(w, "kind"), NULL), wp_names, WP_COUNT, d->wallpaper);
    json *wc = json_get(w, "colors");
    for (int i = 0; i < 4 && i < json_len(wc); i++)
        d->wp[i] = color_parse(json_str(json_at(wc, i), NULL), d->wp[i]);
    d->wp_seed = (int)json_num(json_get(w, "seed"), d->wp_seed);
    d->wp_intensity = (float)json_num(json_get(w, "intensity"), d->wp_intensity);

    json *dk = json_get(j, "dock");
    d->dock = (enum dock_pos)lookup(json_str(json_get(dk, "position"), NULL), dock_names, 4, d->dock);
    d->dock_style = (enum dock_style)lookup(json_str(json_get(dk, "style"), NULL), dock_style_names, 3, d->dock_style);
    d->icons = (enum icon_shape)lookup(json_str(json_get(dk, "icons"), NULL), icon_names, 4, d->icons);
    d->dock_labels = json_bool(json_get(dk, "labels"), d->dock_labels);
    d->icon_style = (enum icon_style)lookup(json_str(json_get(dk, "icon_style"), NULL), icon_style_names, 4, d->icon_style);

    static const char *const corner_names[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
    static const char *const area_names[] = {"left", "right", "bottom", "scattered"};
    static const char *const bar_names[] = {"top", "center", "bottom"};
    json *ln = json_get(j, "launchers");
    if (ln) {
        d->use_dock = json_bool(json_get(ln, "dock"), false);
        d->use_menu = json_bool(json_get(ln, "start_menu"), false);
        d->use_desktop_icons = json_bool(json_get(ln, "desktop_icons"), false);
        d->use_command_bar = json_bool(json_get(ln, "command_bar"), false);
        d->use_orb = json_bool(json_get(ln, "companion_orb"), false);
        d->hot_corner = lookup(json_str(json_get(ln, "hot_corner"), NULL), corner_names, 4, -1);
        d->orb_corner = lookup(json_str(json_get(ln, "orb_corner"), NULL), corner_names, 4, d->orb_corner);
    } else {
        note(problems, plen, "launchers missing");
    }
    json *ditems = json_get(dk, "items");
    if (json_len(ditems) > 0) {
        d->ndock = 0;
        for (json *it = ditems->child; it && d->ndock < APP_COUNT; it = it->next) {
            int a = lookup(json_str(it, NULL), app_ids, APP_COUNT, -1);
            if (a >= 0)
                d->dock_items[d->ndock++] = a;
        }
    }
    d->dock_menu_button = json_bool(json_get(dk, "menu_button"), d->dock_menu_button);
    json *di = json_get(j, "desktop_icons");
    json *dlist = json_get(di, "items");
    if (json_len(dlist) > 0) {
        d->ndesk = 0;
        for (json *it = dlist->child; it && d->ndesk < APP_COUNT; it = it->next) {
            int a = lookup(json_str(it, NULL), app_ids, APP_COUNT, -1);
            if (a >= 0)
                d->desk_items[d->ndesk++] = a;
        }
    }
    d->desk_area = lookup(json_str(json_get(di, "area"), NULL), area_names, 4, d->desk_area);
    json *cb = json_get(j, "command_bar");
    d->bar_position = lookup(json_str(json_get(cb, "position"), NULL), bar_names, 3, d->bar_position);
    const char *ph = json_str(json_get(cb, "placeholder"), NULL);
    if (ph && *ph)
        copy(d->bar_placeholder, sizeof d->bar_placeholder, ph);

    json *m = json_get(j, "start_menu");
    d->menu = (enum menu_style)lookup(json_str(json_get(m, "style"), NULL), menu_names, 4, d->menu);
    const char *mt = json_str(json_get(m, "title"), NULL);
    if (mt)
        copy(d->menu_title, sizeof d->menu_title, mt);
    json *secs = json_get(m, "sections");
    if (json_len(secs) > 0) {
        d->nsections = 0;
        bool seen[APP_COUNT] = {0};
        for (json *s = secs->child; s && d->nsections < MAX_SECTIONS; s = s->next) {
            struct menu_section *ms = &d->sections[d->nsections];
            memset(ms, 0, sizeof *ms);
            copy(ms->title, sizeof ms->title, json_str(json_get(s, "title"), ""));
            json *items = json_get(s, "items");
            for (json *it = items ? items->child : NULL; it && ms->count < MAX_ITEMS; it = it->next) {
                int a = lookup(json_str(it, NULL), app_ids, APP_COUNT, -1);
                if (a >= 0 && !seen[a]) {
                    ms->items[ms->count++] = a;
                    seen[a] = true;
                }
            }
            if (ms->count)
                d->nsections++;
        }
        /* The menu contract: every app must stay reachable. */
        for (int a = 0; a < APP_COUNT; a++) {
            if (seen[a])
                continue;
            if (!d->nsections) {
                d->nsections = 1;
                memset(&d->sections[0], 0, sizeof d->sections[0]);
            }
            struct menu_section *last = &d->sections[d->nsections - 1];
            if (last->count < MAX_ITEMS)
                last->items[last->count++] = a;
            note(problems, plen, "an app was missing from the start menu");
        }
    }

    d->clock = (enum clock_style)lookup(json_str(json_get(j, "clock"), NULL), clock_names, 5, d->clock);
    d->explorer = (enum explorer_style)lookup(json_str(json_get(j, "file_explorer"), NULL), explorer_names, 3, d->explorer);
    d->companion = (enum companion_pos)lookup(json_str(json_get(j, "companion"), NULL), companion_names, 3, d->companion);

    json *apps = json_get(j, "apps");
    for (int a = 0; a < APP_COUNT; a++) {
        json *ap = json_get(apps, app_ids[a]);
        const char *label = json_str(json_get(ap, "label"), NULL);
        const char *glyph = json_str(json_get(ap, "glyph"), NULL);
        if (label && *label)
            copy(d->apps[a].label, sizeof d->apps[a].label, label);
        if (glyph && *glyph)
            snprintf(d->apps[a].glyph, sizeof d->apps[a].glyph, "%.4s", glyph);
    }
    json_free(j);

    /* ---- validation: keep whatever the AI designed usable ---- */
    if (d->radius < 0 || d->radius > 40)
        d->radius = d->radius < 0 ? 0 : 40;
    if (d->blur < 0 || d->blur > 24)
        d->blur = d->blur < 0 ? 0 : 24;
    if (d->surface_opacity < 0.35f || d->surface_opacity > 1)
        d->surface_opacity = d->surface_opacity < 0.35f ? 0.35f : 1;
    if (d->heading_px < 22 || d->heading_px > 64)
        d->heading_px = d->heading_px < 22 ? 22 : 64;
    if (d->body_px < 14 || d->body_px > 22)
        d->body_px = d->body_px < 14 ? 14 : 22;
    if (d->wp_intensity < 0.2f || d->wp_intensity > 1)
        d->wp_intensity = 0.8f;
    if (d->glow < 0 || d->glow > 1)
        d->glow = d->glow < 0 ? 0 : 1;
    if (d->motion < 0 || d->motion > 1)
        d->motion = d->motion < 0 ? 0 : 1;
    color t = readable(d->text, d->bg, 4.5f);
    if (t != d->text)
        note(problems, plen, "desktop text contrast raised");
    d->text = t;
    color mixed_surface = color_mix(d->bg, d->surface, d->surface_opacity);
    t = readable(d->surface_text, mixed_surface, 4.5f);
    if (t != d->surface_text)
        note(problems, plen, "panel text contrast raised");
    d->surface_text = t;
    d->muted = readable(d->muted, mixed_surface, 3.0f);
    /* Wallpaper colour roles: [base, light, light, depth]. The depth tone
     * must stay in the base's family or living wallpapers wash out to grey;
     * the lights must stand out from the base. */
    if (fabsf(color_luma(d->wp[3]) - color_luma(d->wp[0])) > 0.3f) {
        d->wp[3] = color_mix(d->wp[0], d->wp[1], 0.3f);
        note(problems, plen, "wallpaper depth colour brought into the background's family");
    }
    for (int i = 1; i <= 2; i++)
        if (color_contrast(d->wp[i], d->wp[0]) < 1.8f)
            d->wp[i] = color_luma(d->wp[0]) < 0.35f ? color_mix(d->wp[i], RGBA(255, 255, 255, 255), 0.35f)
                                                    : color_mix(d->wp[i], RGBA(0, 0, 0, 255), 0.35f);
    /* Accents carry icons and highlights on panels: they must stand out. */
    t = readable(d->accent, mixed_surface, 2.4f);
    if (t != d->accent)
        note(problems, plen, "accent made visible on panels");
    d->accent = t;
    if (!d->ai_name[0])
        copy(d->ai_name, sizeof d->ai_name, "Companion");
    /* The companion's label should be its name, not the system's. */
    if (!strcasecmp(d->apps[APP_COMPANION].label, "Lumen") || !strcasecmp(d->apps[APP_COMPANION].label, "Companion"))
        copy(d->apps[APP_COMPANION].label, sizeof d->apps[APP_COMPANION].label, d->ai_name);

    /* The reachability contract: however the AI arranged things, every app
     * must be reachable with the mouse alone. */
    if (d->use_menu && !(d->use_dock && d->dock_menu_button) && d->hot_corner < 0) {
        d->hot_corner = 0;
        note(problems, plen, "the start menu had no way to open it, so a hot corner was added");
    }
    bool reach[APP_COUNT] = {0};
    bool everything = d->use_command_bar || d->use_orb;
    if (d->use_dock)
        for (int i = 0; i < d->ndock; i++)
            reach[d->dock_items[i]] = true;
    if (d->use_desktop_icons)
        for (int i = 0; i < d->ndesk; i++)
            reach[d->desk_items[i]] = true;
    if (d->use_menu)
        for (int s = 0; s < d->nsections; s++)
            for (int i = 0; i < d->sections[s].count; i++)
                reach[d->sections[s].items[i]] = true;
    bool all = true;
    for (int a = 0; a < APP_COUNT; a++)
        all = all && reach[a];
    if (!everything && !all) {
        d->use_orb = true;
        note(problems, plen, "some apps were unreachable, so the companion orb was added (it can open anything)");
    }
    return true;
}

const char *design_prompt(void) {
    return "You are designing the entire visual world of a personal computer for the person described below, the "
           "way the OS in the film Her was made for one person. There is no default theme and nothing has to look "
           "like other computers: every choice is yours, made for them.\n\n"
           "Make it breathtaking. Aim for cinematic, luminous, futuristic and warm: deep rich backgrounds with "
           "light that seems to glow from within, vivid but harmonious accents, generous space, elegant type, "
           "frosted glass that catches colour. Avoid flat greys, muddy beige and low-contrast washes. Even a calm, "
           "light design should have depth and glow. Let what you learned about them drive every decision.\n\n"
           "Answer with ONE JSON object and nothing else, with exactly these keys:\n"
           "{\n"
           "  \"ai_name\": the name you chose together in the conversation (look for it there; never "
           "\"Operating System\" or \"Lumen\", which name the system itself), \"user_name\",\n"
           "  \"greeting\": a short line for their desktop (max 70 characters), personal, not generic,\n"
           "  \"tagline\": a few words that sit under it,\n"
           "  \"colors\": {\"background\", \"background2\", \"surface\", \"surface_text\", \"text\", \"muted\", "
           "\"accent\", \"accent2\"} as \"#rrggbb\" (text readable on background; surface_text readable on surface),\n"
           "  \"material\": glass|frost|solid|neon (how panels are made), \"surface_opacity\": 0.35-1,\n"
           "  \"glow\": 0-1 (how much light things give off), \"motion\": 0-1 (how much things move),\n"
           "  \"corner_radius\": 0-32, \"glass_blur\": 0-24, \"shadows\": true/false,\n"
           "  \"fonts\": {\"heading\": one of FONTS, \"body\": one of FONTS, \"heading_size\": 24-56, "
           "\"body_size\": 15-20},\n"
           "  \"wallpaper\": {\"kind\": one of [aurora, nebula, liquid, flow, prism (these five move slowly and "
           "are alive), gradient, waves, bokeh, dunes, sun, constellation, paper], \"colors\": [4 \"#rrggbb\": "
           "base, light 1, light 2, depth], \"seed\": any integer, \"intensity\": 0.2-1},\n"
           "  \"clock\": ring|large|poetic|corner|hidden,\n"
           "  \"launchers\": how they reach their things. Nothing is required: a dock or a start menu exist only "
           "if you decide this person wants one. {\"dock\": true/false, \"start_menu\": true/false, "
           "\"desktop_icons\": true/false, \"command_bar\": true/false (type to open), \"companion_orb\": "
           "true/false (a floating light that opens your conversation; you can open any app for them), "
           "\"hot_corner\": none|top-left|top-right|bottom-left|bottom-right (opens the start menu), "
           "\"orb_corner\": top-left|top-right|bottom-left|bottom-right}. Every app must stay reachable somehow,\n"
           "  \"dock\": {\"position\": bottom|left|top|right, \"style\": pill|bar|orbs, \"icons\": "
           "circle|squircle|square|glyph, \"icon_style\": filled|line|glow|duotone, \"labels\": true/false, "
           "\"items\": [app ids], \"menu_button\": true/false},\n"
           "  \"desktop_icons\": {\"items\": [app ids], \"area\": left|right|bottom|scattered},\n"
           "  \"command_bar\": {\"position\": top|center|bottom, \"placeholder\": its hint text},\n"
           "  \"start_menu\": {\"style\": grid|list|sheet|radial, \"title\", \"sections\": [{\"title\", "
           "\"items\": [app ids]}]} (if used, include every app id once),\n"
           "  \"file_explorer\": list|icons|columns, \"companion\": right|left|floating,\n"
           "  \"apps\": {app id: {\"label\": what to call it for this person}} for all seven,\n"
           "  \"rationale\": one or two sentences to them about why it looks like this\n"
           "}\n"
           "App ids: files, companion, notes, settings, about, redesign, power. Icons are drawn for you; "
           "do not use emoji anywhere.\n";
}

static void put_color(char *buf, size_t n, color c) {
    snprintf(buf, n, "#%02x%02x%02x", C_R(c), C_G(c), C_B(c));
}

bool design_save(const design *d, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    char c[8][8];
    color cs[8] = {d->bg, d->bg2, d->surface, d->surface_text, d->text, d->muted, d->accent, d->accent2};
    for (int i = 0; i < 8; i++)
        put_color(c[i], sizeof c[i], cs[i]);
    char w[4][8];
    for (int i = 0; i < 4; i++)
        put_color(w[i], sizeof w[i], d->wp[i]);
    sbuf b;
    sb_init(&b);
    sb_add(&b, "{\"ai_name\":");
    sb_json_str(&b, d->ai_name);
    sb_add(&b, ",\"user_name\":");
    sb_json_str(&b, d->user_name);
    sb_add(&b, ",\"greeting\":");
    sb_json_str(&b, d->greeting);
    sb_add(&b, ",\"tagline\":");
    sb_json_str(&b, d->tagline);
    sb_printf(&b, ",\"colors\":{\"background\":\"%s\",\"background2\":\"%s\",\"surface\":\"%s\",\"surface_text\":\"%s\","
                  "\"text\":\"%s\",\"muted\":\"%s\",\"accent\":\"%s\",\"accent2\":\"%s\"}",
              c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    sb_printf(&b, ",\"surface_opacity\":%.2f,\"corner_radius\":%.0f,\"glass_blur\":%.0f,\"shadows\":%s",
              (double)d->surface_opacity, (double)d->radius, (double)d->blur, d->shadows ? "true" : "false");
    sb_printf(&b, ",\"material\":\"%s\",\"glow\":%.2f,\"motion\":%.2f", material_names[d->material], (double)d->glow,
              (double)d->motion);
    sb_printf(&b, ",\"fonts\":{\"heading\":\"%s\",\"body\":\"%s\",\"heading_size\":%.0f,\"body_size\":%.0f}",
              d->font_heading, d->font_body, (double)d->heading_px, (double)d->body_px);
    sb_printf(&b, ",\"wallpaper\":{\"kind\":\"%s\",\"colors\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"seed\":%d,\"intensity\":%.2f}",
              wp_names[d->wallpaper], w[0], w[1], w[2], w[3], d->wp_seed, (double)d->wp_intensity);
    static const char *const corners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
    static const char *const areas[] = {"left", "right", "bottom", "scattered"};
    static const char *const bars[] = {"top", "center", "bottom"};
    sb_printf(&b, ",\"launchers\":{\"dock\":%s,\"start_menu\":%s,\"desktop_icons\":%s,\"command_bar\":%s,"
                  "\"companion_orb\":%s,\"hot_corner\":\"%s\",\"orb_corner\":\"%s\"}",
              d->use_dock ? "true" : "false", d->use_menu ? "true" : "false", d->use_desktop_icons ? "true" : "false",
              d->use_command_bar ? "true" : "false", d->use_orb ? "true" : "false",
              d->hot_corner >= 0 ? corners[d->hot_corner] : "none", corners[d->orb_corner & 3]);
    sb_printf(&b, ",\"dock\":{\"position\":\"%s\",\"style\":\"%s\",\"icons\":\"%s\",\"icon_style\":\"%s\",\"labels\":%s,"
                  "\"menu_button\":%s,\"items\":[",
              dock_names[d->dock], dock_style_names[d->dock_style], icon_names[d->icons], icon_style_names[d->icon_style],
              d->dock_labels ? "true" : "false", d->dock_menu_button ? "true" : "false");
    for (int i = 0; i < d->ndock; i++)
        sb_printf(&b, "%s\"%s\"", i ? "," : "", app_ids[d->dock_items[i]]);
    sb_add(&b, "]},\"desktop_icons\":{\"items\":[");
    for (int i = 0; i < d->ndesk; i++)
        sb_printf(&b, "%s\"%s\"", i ? "," : "", app_ids[d->desk_items[i]]);
    sb_printf(&b, "],\"area\":\"%s\"},\"command_bar\":{\"position\":\"%s\",\"placeholder\":", areas[d->desk_area & 3],
              bars[d->bar_position % 3]);
    sb_json_str(&b, d->bar_placeholder);
    sb_add(&b, "}");
    sb_printf(&b, ",\"start_menu\":{\"style\":\"%s\",\"title\":", menu_names[d->menu]);
    sb_json_str(&b, d->menu_title);
    sb_add(&b, ",\"sections\":[");
    for (int s = 0; s < d->nsections; s++) {
        sb_add(&b, s ? ",{\"title\":" : "{\"title\":");
        sb_json_str(&b, d->sections[s].title);
        sb_add(&b, ",\"items\":[");
        for (int i = 0; i < d->sections[s].count; i++)
            sb_printf(&b, "%s\"%s\"", i ? "," : "", app_ids[d->sections[s].items[i]]);
        sb_add(&b, "]}");
    }
    sb_printf(&b, "]},\"clock\":\"%s\",\"file_explorer\":\"%s\",\"companion\":\"%s\",\"apps\":{", clock_names[d->clock],
              explorer_names[d->explorer], companion_names[d->companion]);
    for (int a = 0; a < APP_COUNT; a++) {
        sb_printf(&b, "%s\"%s\":{\"label\":", a ? "," : "", app_ids[a]);
        sb_json_str(&b, d->apps[a].label);
        sb_add(&b, ",\"glyph\":");
        sb_json_str(&b, d->apps[a].glyph);
        sb_add(&b, "}");
    }
    sb_add(&b, "},\"rationale\":");
    sb_json_str(&b, d->rationale);
    sb_add(&b, "}\n");
    bool ok = fwrite(b.p, 1, b.len, f) == b.len;
    fclose(f);
    sb_free(&b);
    return ok;
}

bool design_load(design *d, const char *path, const profile *p) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return design_from_json(d, buf, p, NULL, 0);
}

#include "design_facet.inc"

/* ---- the portrait ---- */

int wallpaper_kind_from_name(const char *name) { return lookup(name, wp_names, WP_COUNT, -1); }
const char *wallpaper_name(int kind) { return kind >= 0 && kind < WP_COUNT ? wp_names[kind] : "gradient"; }

void portrait_learn(profile *p, const char *key, const char *val) {
    while (*key == ' ')
        key++;
    while (*val == ' ')
        val++;
    if (!*key || !*val)
        return;
    for (int i = 0; i < p->nfacts; i++)
        if (!strcasecmp(p->facts[i].key, key)) {
            if (strcasestr(p->facts[i].val, val))
                return;
            size_t l = strlen(p->facts[i].val);
            snprintf(p->facts[i].val + l, sizeof p->facts[i].val - l, "; %s", val);
            return;
        }
    if (p->nfacts >= (int)(sizeof p->facts / sizeof p->facts[0]))
        return;
    struct fact *f = &p->facts[p->nfacts++];
    snprintf(f->key, sizeof f->key, "%s", key);
    snprintf(f->val, sizeof f->val, "%s", val);
    for (char *c = f->key; *c; c++)
        *c = (char)tolower((unsigned char)*c);
}

void portrait_forget(profile *p, int i) {
    if (i < 0 || i >= p->nfacts)
        return;
    memmove(&p->facts[i], &p->facts[i + 1], sizeof p->facts[0] * (size_t)(p->nfacts - i - 1));
    p->nfacts--;
}

void portrait_text(const profile *p, char *out, size_t len) {
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < p->nfacts && o < len; i++)
        o += (size_t)snprintf(out + o, len - o, "%s: %s\n", p->facts[i].key, p->facts[i].val);
}

/* Plain text, one "key<TAB>value" per line, so it can be read and edited. */
bool portrait_save(const profile *p, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "#comfort\t%.2f %d %d %.2f\n", (double)p->text_scale, p->high_contrast, p->reduce_motion, (double)p->tempo);
    if (p->type_font[0])
        fprintf(f, "#type\t%s\n", p->type_font);
    for (int i = 0; i < p->nfacts; i++)
        fprintf(f, "%s\t%s\n", p->facts[i].key, p->facts[i].val);
    fclose(f);
    return true;
}

bool portrait_load(profile *p, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[512];
    p->nfacts = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        if (!strcmp(line, "#comfort")) {
            int hc = 0, rm = 0;
            double ts = 0, tp = -1;
            sscanf(tab + 1, "%lf %d %d %lf", &ts, &hc, &rm, &tp);
            p->text_scale = (float)ts, p->high_contrast = hc, p->reduce_motion = rm, p->tempo = (float)tp;
        } else if (!strcmp(line, "#type")) {
            snprintf(p->type_font, sizeof p->type_font, "%s", tab + 1);
        } else {
            portrait_learn(p, line, tab + 1);
        }
    }
    fclose(f);
    return true;
}
