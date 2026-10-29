/* Included at the end of design.c (shares its tables and helpers).
 * Bridges Facet markup documents and the design record. */
#include "fm/fm.h"
#include "stylist.h"
#include <dirent.h>

static void hexc(char *buf, size_t n, color c) { snprintf(buf, n, "#%02x%02x%02x", C_R(c), C_G(c), C_B(c)); }

static bool var_color(const fm_doc *doc, const char *name, color *out) {
    const char *v = fm_var(doc, name);
    return v && fm_parse_color(v, out);
}

static void var_font(const fm_doc *doc, const char *name, char *out, size_t n) {
    const char *v = fm_var(doc, name);
    if (!v)
        return;
    char fam[48];
    while (*v == ' ' || *v == '"' || *v == '\'')
        v++;
    size_t k = 0;
    while (v[k] && v[k] != '"' && v[k] != '\'' && v[k] != ',' && k < sizeof fam - 1)
        k++;
    snprintf(fam, sizeof fam, "%.*s", (int)k, v);
    while (k && fam[k - 1] == ' ')
        fam[--k] = 0;
    if (font_installed(fam))
        copy(out, n, fam);
}

static void reach_rec(const fm_node *n, bool *reach, bool *search) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        if (!strcasecmp(n->tag, "search"))
            *search = true;
        if (!strcasecmp(n->tag, "orb"))
            reach[APP_COMPANION] = true;
        if (!strcasecmp(n->tag, "view")) {
            const char *a = fm_attr_get(n, "app");
            int k = lookup(a ? a : "companion", app_ids, APP_COUNT, -1);
            if (k >= 0)
                reach[k] = true;
        }
        if (!strcasecmp(n->tag, "applist")) {
            const char *list = fm_attr_get(n, "apps");
            if (!list || !*list || !strcasecmp(list, "all")) {
                for (int i = 0; i < APP_COUNT; i++)
                    reach[i] = true;
            } else {
                for (int i = 0; i < APP_COUNT; i++)
                    if (strcasestr(list, app_ids[i]))
                        reach[i] = true;
            }
        }
        static const char *const keys[] = {"what", "action", "do", "on-click", "onclick"};
        for (int k = 0; k < 5; k++) {
            const char *a = fm_attr_get(n, keys[k]);
            if (!a)
                continue;
            for (int i = 0; i < APP_COUNT; i++) {
                char want[32];
                snprintf(want, sizeof want, "open:%s", app_ids[i]);
                if (strcasestr(a, want) || !strcasecmp(a, app_ids[i]))
                    reach[i] = true;
            }
            if (strcasestr(a, "redesign"))
                reach[APP_REDESIGN] = true;
            if (strcasestr(a, "power:"))
                reach[APP_POWER] = true;
        }
        reach_rec(n->first, reach, search);
    }
}

bool design_from_facet(design *d, fm_doc *doc, const profile *p, char *problems, int plen) {
    design_from_profile(d, p);
    if (problems && plen)
        problems[0] = 0;
    for (int i = 0; i < doc->nproblems; i++)
        if (doc->problems[i].fatal && problems)
            note(problems, plen, doc->problems[i].msg);
    if (fm_has_fatal(doc) || !doc->root)
        return false;
    for (const fm_node *n = doc->root->first; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        if (!strcasecmp(n->tag, "meta")) {
            const char *v;
            if ((v = fm_attr_get(n, "ai-name")) && *v) copy(d->ai_name, sizeof d->ai_name, v);
            if ((v = fm_attr_get(n, "user-name")) && *v) copy(d->user_name, sizeof d->user_name, v);
            if ((v = fm_attr_get(n, "greeting"))) copy(d->greeting, sizeof d->greeting, v);
            if ((v = fm_attr_get(n, "tagline"))) copy(d->tagline, sizeof d->tagline, v);
            if ((v = fm_attr_get(n, "rationale"))) copy(d->rationale, sizeof d->rationale, v);
        } else if (!strcasecmp(n->tag, "apps")) {
            for (const fm_node *a = n->first; a; a = a->next) {
                if (a->kind != FM_ELEM)
                    continue;
                int k = lookup(fm_attr_get(a, "id"), app_ids, APP_COUNT, -1);
                const char *l = fm_attr_get(a, "label");
                if (k >= 0 && l && *l)
                    copy(d->apps[k].label, sizeof d->apps[k].label, l);
            }
        } else if (!strcasecmp(n->tag, "wallpaper")) {
            d->wallpaper = (enum wallpaper_kind)lookup(fm_attr_get(n, "kind"), wp_names, WP_COUNT, d->wallpaper);
            const char *cs = fm_attr_get(n, "colors");
            if (cs) {
                const char *s = cs;
                for (int k = 0; k < 4 && *s; k++) {
                    while (*s == ' ' || *s == ',')
                        s++;
                    color c;
                    if (fm_parse_color(s, &c))
                        d->wp[k] = c | 0xff000000u;
                    while (*s && *s != ' ' && *s != ',')
                        s++;
                }
            }
            const char *v;
            if ((v = fm_attr_get(n, "seed"))) d->wp_seed = atoi(v);
            if ((v = fm_attr_get(n, "intensity"))) {
                float f = strtof(v, NULL);
                d->wp_intensity = f < 0.1f ? 0.1f : f > 1 ? 1 : f;
            }
        }
    }
    color c;
    if (var_color(doc, "--bg", &c)) d->bg = c | 0xff000000u;
    if (var_color(doc, "--accent", &c)) d->accent = c | 0xff000000u;
    d->bg2 = color_mix(d->bg, d->accent, 0.2f);
    if (var_color(doc, "--bg2", &c)) d->bg2 = c | 0xff000000u;
    if (var_color(doc, "--surface", &c)) {
        float a = C_A(c) / 255.0f;
        d->surface_opacity = a < 0.35f ? 0.35f : a;
        d->surface = c | 0xff000000u;
    }
    if (var_color(doc, "--text", &c)) d->text = c | 0xff000000u;
    if (var_color(doc, "--muted", &c)) d->muted = c | 0xff000000u;
    if (var_color(doc, "--accent2", &c)) d->accent2 = c | 0xff000000u;
    d->surface_text = d->text;
    if (var_color(doc, "--surface-text", &c)) d->surface_text = c | 0xff000000u;
    d->surface_text = readable(d->surface_text, d->surface, 4.5f);
    d->text = readable(d->text, d->bg, 4.5f);
    d->muted = readable(d->muted, d->surface, 3.0f);
    /* An accent the same as the surface makes bubbles, icons and buttons vanish. */
    if (color_contrast(d->accent, d->surface) < 1.6f)
        d->accent = readable(d->accent, d->surface, 2.2f);
    var_font(doc, "--font-heading", d->font_heading, sizeof d->font_heading);
    var_font(doc, "--font-body", d->font_body, sizeof d->font_body);
    const char *r = fm_var(doc, "--radius");
    if (r) {
        float v = strtof(r, NULL);
        d->radius = v < 0 ? 0 : v > 40 ? 40 : v;
    }
    /* Every app must be reachable somehow. */
    bool reach[APP_COUNT] = {0}, search = false;
    reach_rec(doc->desktop, reach, &search);
    if (!search) {
        char miss[200] = "";
        for (int i = 0; i < APP_COUNT; i++)
            if (!reach[i])
                snprintf(miss + strlen(miss), sizeof miss - strlen(miss), "%s%s", miss[0] ? ", " : "", app_ids[i]);
        if (miss[0] && problems) {
            char msg[260];
            snprintf(msg, sizeof msg,
                     "nothing opens these apps: %s (add them to an applist, a button with what=\"open:id\", or add "
                     "a search)",
                     miss);
            note(problems, plen, msg);
        }
    }
    if (!fm_var(doc, "--accent") && problems)
        note(problems, plen, ":root is missing --bg/--surface/--text/--accent variables");
    return true;
}

/* Markup reproducing a design record made with the older JSON vocabulary,
 * or built from the person's choices when no AI is reachable. */
char *design_to_facet(const design *d) {
    sbuf b;
    sb_init(&b);
    char bg[8], bg2[8], st[8], tx[8], mu[8], ac[8], ac2[8], w[4][8];
    hexc(bg, 8, d->bg), hexc(bg2, 8, d->bg2), hexc(st, 8, d->surface_text);
    hexc(tx, 8, d->text), hexc(mu, 8, d->muted), hexc(ac, 8, d->accent), hexc(ac2, 8, d->accent2);
    for (int i = 0; i < 4; i++)
        hexc(w[i], 8, d->wp[i]);
    sb_printf(&b, "<facet>\n<meta ai-name=\"%s\" user-name=\"%s\" greeting=\"%s\" tagline=\"%s\"/>\n<apps>", d->ai_name,
              d->user_name, d->greeting, d->tagline);
    for (int i = 0; i < APP_COUNT; i++)
        sb_printf(&b, "<app id=\"%s\" label=\"%s\"/>", app_ids[i], d->apps[i].label);
    sb_printf(&b, "</apps>\n<wallpaper kind=\"%s\" colors=\"%s %s %s %s\" seed=\"%d\" intensity=\"%.2f\"/>\n",
              wp_names[d->wallpaper], w[0], w[1], w[2], w[3], d->wp_seed, d->wp_intensity);
    bool glassy = d->material != MAT_SOLID;
    float blur = glassy ? d->blur + 6 : 0;
    sb_printf(&b,
              "<style>\n:root { --bg:%s; --bg2:%s; --surface:rgba(%d,%d,%d,%.2f); --surface-text:%s; --text:%s; "
              "--muted:%s; --accent:%s; --accent2:%s; --font-heading:%s; --font-body:%s; --radius:%.0f; }\n",
              bg, bg2, C_R(d->surface), C_G(d->surface), C_B(d->surface), glassy ? d->surface_opacity : 1.0f, st, tx,
              mu, ac, ac2, d->font_heading, d->font_body, d->radius);
    sb_printf(&b, "desktop { color:var(--text); font-family:var(--font-body); font-size:%.0f; }\n", d->body_px);
    sb_printf(&b,
              ".glass { background:var(--surface); blur:%.0f; saturate:%.2f; radius:%.0f; color:var(--surface-text);"
              " border:1 rgba(255,255,255,0.22);%s }\n",
              blur, glassy ? 1.4f : 1.0f, d->radius + 6, d->shadows ? " shadow:0 14 40 rgba(0,0,0,0.35);" : "");
    if (d->material == MAT_NEON)
        sb_printf(&b, ".glass { border:1.4 var(--accent); glow:%.0f %s; }\n", 10 + 20 * d->glow, ac);
    sb_printf(&b, "item { icon-shape:%s; icon-style:%s; transition:%d; }\n",
              d->icons == ICON_GLYPH ? "none" : icon_names[d->icons], icon_style_names[d->icon_style],
              (int)(120 + 160 * d->motion));
    sb_printf(&b, "item:hover { transform:scale(%.2f) translate(0,-%d); }\n", 1.08f + 0.2f * d->motion,
              (int)(2 + 8 * d->motion));
    sb_printf(&b, "#greet { font:\"%s %.0f\"; }\n#tag { color:var(--muted); }\n", d->font_heading, d->heading_px);
    sb_printf(&b, "#bigclock { font:\"%s %.0f\"; }\n", d->font_heading, d->heading_px * 2.2f);
    sb_add(&b, "#hero { direction:column; align:center; gap:6; margin:90 0 0 0; }\n");
    sb_printf(&b, "#corner { direction:column; align:end; margin:28 32 0 0; } #corner clock { font:\"%s 28\"; }\n",
              d->font_heading);
    bool vert = d->dock == DOCK_LEFT || d->dock == DOCK_RIGHT;
    int isz = d->dock_style == DOCK_BAR ? 40 : 50;
    sb_printf(&b, "dock { %s gap:10; padding:12; align:center; }\n", vert ? "direction:column;" : "");
    if (d->dock_style == DOCK_BAR)
        sb_printf(&b, "dock { radius:0; } dock item { icon-size:%d; }\n", isz);
    else if (d->dock_style == DOCK_ORBS)
        sb_printf(&b,
                  "dock { background:none; blur:0; border:none; shadow:none; } dock item { icon-size:%d; "
                  "icon-shape:circle; }\n",
                  isz + 6);
    else
        sb_printf(&b, "dock { margin:14; radius:%.0f; } dock item { icon-size:%d; }\n", d->radius + 14, isz);
    sb_add(&b, "dock item label { font-size:12; }\n");
    sb_printf(&b, "#menu { direction:column; padding:22; gap:14; min-width:420; } #menu title { font:\"%s 22\"; }\n",
              d->font_heading);
    sb_add(&b, "#menu .section { color:var(--muted); font-size:13; text-transform:uppercase; }\n");
    if (d->menu == MENU_LIST)
        sb_add(&b, "#menu applist { direction:column; align:stretch; gap:2; } #menu item { direction:row; gap:12; "
                   "padding:8 12; radius:10; icon-size:30; } #menu item:hover { background:rgba(255,255,255,0.12); "
                   "transform:none; }\n");
    else
        sb_add(&b, "#menu applist { display:grid; columns:4; gap:12; } #menu item { icon-size:54; padding:10; "
                   "radius:14; } #menu item:hover { background:rgba(255,255,255,0.12); }\n");
    if (d->menu == MENU_SHEET || d->menu == MENU_RADIAL)
        sb_printf(&b, "#menu { align:center; justify:center; background:rgba(%d,%d,%d,0.55); blur:30; radius:0; }\n",
                  C_R(d->bg), C_G(d->bg), C_B(d->bg));
    if (d->menu == MENU_RADIAL)
        sb_add(&b, "#menu applist { display:orbit; orbit:220; } #menu item { icon-size:62; }\n");
    sb_add(&b, "#desk applist { display:grid; columns:1; gap:18; } #desk item { icon-size:56; }\n");
    sb_add(&b, "#bar { width:560; height:52; padding:0 20; radius:26; gap:10; }\n#orb { size:64; margin:30; }\n");
    sb_printf(&b,
              "frame { direction:column; background:var(--surface); blur:%.0f; radius:%.0f; color:var(--surface-text);"
              " border:1 rgba(255,255,255,0.18); overflow:hidden;%s }\n",
              blur, d->radius, d->shadows ? " shadow:0 20 50 rgba(0,0,0,0.4);" : "");
    sb_printf(&b, "titlebar { height:52; padding:0 14 0 22; gap:8; font:\"%s %.0f\"; }\n", d->font_heading,
              d->heading_px * 0.62f);
    sb_add(&b, "titlebar button { size:30; radius:15; icon-size:12; background:rgba(127,127,127,0.15); }\n"
               "titlebar button:hover { background:var(--accent); color:#fff; }\n"
               "slot { padding:4 24 24 24; }\n</style>\n<desktop>\n");

    if (d->clock == CLOCK_LARGE || d->clock == CLOCK_POETIC || d->clock == CLOCK_RING) {
        sb_add(&b, "  <column id=\"hero\" place=\"top\">");
        if (d->clock == CLOCK_RING)
            sb_add(&b, "<clock id=\"bigclock\" face=\"ring\" style=\"size:240; justify:center; align:center\"/>");
        else
            sb_printf(&b, "<clock id=\"bigclock\" format=\"%s\"/>", d->clock == CLOCK_POETIC ? "words" : "time");
        sb_add(&b, "<text id=\"greet\" bind=\"greeting\"/><text id=\"tag\" bind=\"tagline\"/></column>\n");
    } else if (d->clock == CLOCK_CORNER) {
        sb_add(&b, "  <column id=\"corner\" place=\"top-right\"><clock/><clock format=\"date\"/></column>\n");
    }
    if (d->use_dock) {
        sb_printf(&b, "  <dock class=\"glass\" dock=\"%s\"%s><applist apps=\"", dock_names[d->dock],
                  d->dock_style == DOCK_BAR ? (vert ? " height=\"full\"" : " width=\"full\"") : "");
        for (int i = 0; i < d->ndock; i++)
            sb_printf(&b, "%s%s", i ? "," : "", app_ids[d->dock_items[i]]);
        sb_printf(&b, "\" labels=\"%s\"/>", d->dock_labels ? "below" : "hover");
        if (d->dock_menu_button && d->use_menu)
            sb_add(&b, "<button what=\"toggle:#menu\" icon=\"grid\" style=\"size:50; radius:25\"/>");
        sb_add(&b, "</dock>\n");
    }
    if (d->use_menu) {
        const char *tag = d->menu == MENU_SHEET || d->menu == MENU_RADIAL ? "sheet" : "popover";
        sb_printf(&b, "  <%s id=\"menu\" class=\"glass\" hotkey=\"meta\">", tag);
        if (d->menu_title[0])
            sb_printf(&b, "<title>%s</title>", d->menu_title);
        for (int s = 0; s < d->nsections; s++) {
            if (d->sections[s].title[0])
                sb_printf(&b, "<text class=\"section\">%s</text>", d->sections[s].title);
            sb_add(&b, "<applist apps=\"");
            for (int i = 0; i < d->sections[s].count; i++)
                sb_printf(&b, "%s%s", i ? "," : "", app_ids[d->sections[s].items[i]]);
            sb_add(&b, "\" labels=\"below\"/>");
        }
        sb_printf(&b, "</%s>\n", tag);
        if (!d->use_dock || !d->dock_menu_button)
            sb_add(&b, "  <button class=\"glass\" place=\"top-left\" what=\"toggle:#menu\" icon=\"grid\" "
                       "style=\"size:52; margin:24; radius:26\"/>\n");
    }
    if (d->use_desktop_icons && d->ndesk) {
        const char *pl = d->desk_area == 1 ? "right" : d->desk_area == 2 ? "bottom" : "left";
        sb_printf(&b, "  <column id=\"desk\" place=\"%s\" style=\"margin:40\"><applist apps=\"", pl);
        for (int i = 0; i < d->ndesk; i++)
            sb_printf(&b, "%s%s", i ? "," : "", app_ids[d->desk_items[i]]);
        sb_printf(&b, "\" labels=\"below\"%s/></column>\n",
                  d->desk_area == 2 ? " style=\"display:flex; direction:row\"" : "");
    }
    if (d->use_command_bar) {
        const char *pl = d->bar_position == 0 ? "top" : d->bar_position == 2 ? "bottom" : "center";
        sb_printf(&b, "  <search id=\"bar\" class=\"glass\" place=\"%s\" placeholder=\"%s\" style=\"margin:%s\"/>\n",
                  pl, d->bar_placeholder[0] ? d->bar_placeholder : "Type to open anything",
                  d->bar_position == 1 ? "260 0 0 0" : "36");
    }
    if (d->use_orb || (!d->use_dock && !d->use_menu && !d->use_desktop_icons && !d->use_command_bar)) {
        static const char *const corners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
        sb_printf(&b, "  <orb id=\"orb\" place=\"%s\"/>\n", corners[d->orb_corner & 3]);
    }
    sb_add(&b, "</desktop>\n<template id=\"window\"><frame><titlebar drag=\"window\"><text bind=\"window.title\" "
               "style=\"grow:1\"/><button what=\"window:minimize\" icon=\"minimize\"/><button what=\"window:close\" "
               "icon=\"close\"/></titlebar><slot/></frame></template>\n</facet>\n");
    return b.p;
}

/* What the designer model is asked for: one Facet markup document. */
const char *design_prompt_markup(void) {
    static char *p;
    if (p)
        return p;
    sbuf b;
    sb_init(&b);
    sb_add(&b,
           "You are designing the entire desktop of a personal computer for the person described below, the way the "
           "OS in the film Her was made for one person. There is no default theme, no default layout and nothing has "
           "to look like other computers. Whether there is a taskbar, a dock, a menu bar, a start menu, a launcher "
           "sheet, icons on the desktop, a search field, apps orbiting a light, or something nobody has made before "
           "is entirely your decision, made for them. Invent the structure, not just the colours.\n\n"
           "Make it beautiful in their idiom, never a house style: that may be paper-white and quiet, sunlit and "
           "airy, warm and wooden, or dark and glowing; the portrait decides, not you. Always crafted: a clear type "
           "scale (a display size, a title size, a body size and a caption size, line height about 1.4), a "
           "spacing rhythm in multiples of 4, few surfaces with generous space (three or fewer is often right), "
           "colours derived from one or two hues in tonal steps rather than saturated primaries, and glass, shadow "
           "and motion used with restraint. Don't imitate macOS or Windows window chrome (no traffic-light "
           "buttons) unless they asked for it. Keep everything at least 24 px from the screen edges.\n\n"
           "Answer with ONE <facet> document and nothing else: no prose, no code fences.\n"
           "Keep it compact, as a good designer's stylesheet is: a complete desktop is 150 to 350 lines. Style shared "
           "things once (a selector for all items, CSS variables for colours) instead of repeating rules per element, "
           "keep canvas scripts short, and write no comments beyond a few section headers. They are watching it being "
           "written: a long document is a long wait.\n"
           "Rules: every app must stay reachable (an applist, buttons with what=\"open:id\", or a search). Name the "
           "apps for them in <apps>. Use only fonts from FONTS. The <meta> ai-name is the name chosen together in "
           "the conversation (never \"Operating System\" or \"Lumen\", which name the system itself). No emoji anywhere.\n"
           "App ids: files, companion (your conversation with them), notes, settings, about, terminal (a real command line), redesign, power");
    DIR *dir = opendir("/apps");
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)))
            if (de->d_name[0] != '.')
                sb_printf(&b, ", %s (a bundled app)", de->d_name);
        closedir(dir);
    }
    sb_add(&b, ".\n\n");
    sb_add(&b, fm_reference());
    sb_add(&b, "\n");
    sb_add(&b, fm_examples());
    sb_add(&b, "\nThose examples show the language; your design must be your own, made for this person.\n");
    stylist_theme_menu(&b); /* finished themes it may start from (stylist_look.c) */
    p = b.p;
    return p;
}
