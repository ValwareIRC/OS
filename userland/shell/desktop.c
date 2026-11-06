#define _GNU_SOURCE
#include "app.h"
#include "priv.h"
#include "emblem.h"
#include "models.h"
#include "mind.h"
#include "wallpaper.h"
#include "fm/fm.h"
#include "term.h"
#include "wl/wl.h"
#include "store.h"
#include "fxwin.h"
#include "fx/fx.h"
#include "voice.h"
#include "agent.h"
#include "nerve.h"
#include "persona.h"
#include "files.h"
#include "launcher.h"
#include "wm.h"
#include "theme.h"
#include "presence.h"
#include "caption.h"
#include "notes.h"
#include "alive.h"
#include "stylist.h"
#include "settings.h"
#include "chats.h"
#include "menu_actions.h"
#include "tray.h"
#include "iconart.h"
#include "preview.h"
#include "portal.h"
#include "appapi.h"
#include "body.h"
#include "browsers.h"
#include "capture.h"
#include "family.h"
#include "changes.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define D (A.d)

static font *HF(float px) { return font_get(D.font_heading, px); }
static font *BF(float px) { return font_get(D.font_body, px); }



static const char *companion_system(void);
static void app_body(int w, rect body);
static int focused_window(void);
static void explain_why(fm_node *n);

/* ---- frosted panel ---- */

/* Panels are made of whatever material the AI chose:
 *   glass: clear, vivid blur of what is behind, bright edge highlights
 *   frost: milkier, softer
 *   solid: opaque surface colour with depth
 *   neon:  dark tinted glass with a glowing accent edge */
static void panel(rect r, float radius) {
    bool dark = color_luma(D.surface) < 0.3f;
    if (D.shadows)
        shadow_round(A.frame, (rect){r.x, r.y + 14, r.w, r.h}, radius, 46, RGBA(0, 0, 0, dark ? 110 : 60));
    if (D.material == MAT_NEON)
        glow_round(A.frame, r, radius, 10 + 26 * D.glow, color_alpha(D.accent, 0.18f + 0.3f * D.glow));
    float op = D.surface_opacity;
    if (D.material != MAT_SOLID && D.blur >= 1) {
        blur_fast(A.frame, r, radius, (int)(D.blur * 0.9f) + 2);
        vibrancy(A.frame, r, radius, D.material == MAT_FROST ? 1.1f : 1.45f, dark ? -4 : 6);
    } else {
        op = 1;
    }
    if (D.material == MAT_FROST)
        op = op + 0.15f > 1 ? 1 : op + 0.15f;
    if (D.material == MAT_NEON)
        op = op * 0.75f;
    color top = color_mix(D.surface, RGBA(255, 255, 255, 255), dark ? 0.08f : 0.25f);
    fill_round_vgradient(A.frame, r, radius, color_alpha(top, op), color_alpha(D.surface, op));
    /* light catching the edge: bright at the top, fading down */
    stroke_round_vgradient(A.frame, r, radius, 1, RGBA(255, 255, 255, dark ? 70 : 150), RGBA(255, 255, 255, dark ? 8 : 30));
    if (D.material == MAT_NEON)
        stroke_round(A.frame, r, radius, 1.4f, color_alpha(D.accent, 0.85f));
}

/* ---- windows ---- */

/* Who the keyboard belongs to: a view docked into the design (its app name),
 * once clicked, or else the front window. Only ever one of them, so typing to
 * the companion's docked chat never also types into Settings behind it. */
static char kbd_view[16];
static int pointer_top = -1; /* the window under the pointer this frame, or -1 */

static bool view_has_keys(const char *app) {
    if (kbd_view[0])
        return !strcmp(kbd_view, app);
    /* nothing clicked yet: the docked companion listens while no window is in front */
    return !strcmp(app, "companion") && focused_window() < 0;
}

static void open_window(int w) {
    kbd_view[0] = 0; /* a window opened: it takes the typing */
    struct window *win = &A.win[w];
    if (!win->open) {
        /* Windows open in the space the design leaves free. */
        rect wk = A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H};
        int ww = w == WIN_COMPANION ? 460 : 760, wh = w == WIN_COMPANION ? 640 : 520;
        if (w == WIN_ABOUT)
            ww = 640, wh = 520;
        if (w == WIN_FILES) /* room for its sidebar and a few columns */
            ww = 940, wh = 600;
        if (w == WIN_NOTES) /* the list of notes and the page */
            ww = 900, wh = 600;
        if (w == WIN_SETTINGS) /* a sidebar and a page */
            ww = 940, wh = 660;
        if (w == WIN_PREVIEW) { /* the picture at the screen's proportions, and room to talk */
            ww = wk.w - 80 < 1040 ? wk.w - 80 : 1040;
            wh = (ww - 48) * A.H / (A.W > 0 ? A.W : 1) + 116 + 90;
        }
        if (ww > wk.w - 40) ww = wk.w - 40;
        if (wh > wk.h - 40) wh = wk.h - 40;
        int off = w * 34;
        win->r = (rect){wk.x + wk.w / 2 - ww / 2 - 120 + off, wk.y + wk.h / 2 - wh / 2 - 20 + off / 2, ww, wh};
        if (w == WIN_COMPANION) {
            wh = wk.h - 48 < 680 ? wk.h - 48 : 680;
            int x = D.companion == COMPANION_LEFT ? wk.x + 24 : wk.x + wk.w - ww - 24;
            win->r = (rect){x, wk.y + (wk.h - wh) / 2, ww, wh};
        }
        if (w == WIN_PREVIEW) {
            if (wh > wk.h - 40) wh = wk.h - 40;
            win->r = (rect){wk.x + (wk.w - ww) / 2, wk.y + (wk.h - wh) / 2, ww, wh};
        }
        if (win->r.x < wk.x + 8) win->r.x = wk.x + 8;
        if (win->r.y < wk.y + 8) win->r.y = wk.y + 8;
    }
    if (!win->open)
        wm_opened(w); /* arrives with an animation */
    else if (win->minimized)
        wm_activate(w); /* flies back out of its taskbar item */
    win->open = true;
    win->minimized = false;
    win->z = ++A.ztop;
}

static void launch(int app) {
    switch (app) {
    case APP_FILES: open_window(WIN_FILES); break;
    case APP_COMPANION: open_window(WIN_COMPANION); break;
    case APP_NOTES: open_window(WIN_NOTES); break;
    case APP_SETTINGS: open_window(WIN_SETTINGS); break;
    case APP_ABOUT: open_window(WIN_ABOUT); break;
    case APP_TERMINAL: open_window(WIN_TERMINAL); break;
    case APP_REDESIGN:
        /* Never a redesign out of the blue: the preview opens on a copy of
         * the space and asks what they want; nothing changes until they say,
         * and nothing is real until they apply it. */
        if (!preview_target()) {
            open_window(WIN_COMPANION);
            convo_add(&A.companion, "assistant",
                      "What would you like to change? A small tweak, or something new altogether? Tell me what you "
                      "have in mind and I'll take it from there.");
        }
        break;
    case APP_POWER:
        A.confirm_power = true; /* never on a single stray click */
        break;
    }
}

static const char *window_title(int w) {
    static const int map[WIN_COUNT] = {APP_FILES, APP_COMPANION, APP_NOTES, APP_SETTINGS, APP_ABOUT, APP_TERMINAL,
                                       APP_REDESIGN};
    if (w == WIN_PREVIEW)
        return "Preview";
    if (w >= WIN_COUNT && A.win[w].fx)
        return fxwin_title(A.win[w].fx);
    if (w >= WIN_COUNT) {
        wl_view *v = A.win[w].view;
        return !v ? "" : v->title[0] ? v->title : v->app_id[0] ? v->app_id : "App";
    }
    return fm_host_app_label(map[w]);
}

static bool has_any(const char *text, const char *const *words) {
    for (int i = 0; words[i]; i++)
        if (strcasestr(text, words[i]))
            return true;
    return false;
}

/* Does the person's message ask for the look of the space to change? */
static bool asks_for_change(const char *text) {
    static const char *const topics[] = {"color", "colour", "palette", "background", "wallpaper", "font", "theme",
                                         "look", "feel", "design", "layout", "dark", "light", "bright", "contrast",
                                         "vibe", "style", "mood", "redesign", "icon", "dock", "menu", NULL};
    static const char *const verbs[] = {"make", "change", "turn", "switch", "more", "less", "could", "can", "want",
                                        "please", "give", "add", "remove", "try", "redesign", "adjust", NULL};
    return has_any(text, topics) && has_any(text, verbs);
}

/* Does the reply claim a change was made? */
static bool claims_change(const char *text) {
    static const char *const claims[] = {"I've", "I have", "I'll", "I will", "adjusted", "changed", "shifted",
                                         "switched", "updated", "softened", "applied", "set the", "reintroduced",
                                         "refined", "changing", "adjusting", "shifting", NULL};
    return has_any(text, claims);
}

/* Does a reply say it is doing something now ("Fixing it now", "I'm replacing both", "Opening it")? Said with no tag,
 * nothing happened: it narrated the next step as done, and they only found out by asking. */
static bool promises_action(const char *text) {
    static const char *const says[] = {"fixing it",  "fixing that", "fixing both", "fixing them", "doing it",
                                       "doing the fix", "doing that", "on it",    "replacing",  "opening it",
                                       "opening that", "bringing it", "bringing up", "rewriting", "writing it",
                                       "changing it", "i'll do",    "i'll fix",    "i'll change", "i'll write",
                                       "i'll make",   "i'll open",   "i'll add",    "i'll remove", "i'm going to",
                                       "let me fix", "let me open", "let me change", "let me do", "let me try", "working on it", "starting now", "doing it now", "making it",
                                       "adding it",   "removing it", "putting it",  "fixing now", "trying again",
                                       NULL};
    return has_any(text, says);
}

/* The designer's reasons, from <meta><why for="#id" said="...">...</why>. */
static void collect_whys(char *out, size_t len) {
    size_t o = 0;
    out[0] = 0;
    if (!A.ui || !A.ui->root)
        return;
    for (fm_node *m = A.ui->root->first; m; m = m->next) {
        if (m->kind != FM_ELEM || strcmp(m->tag, "meta"))
            continue;
        for (fm_node *w = m->first; w && o < len; w = w->next) {
            if (w->kind != FM_ELEM || strcmp(w->tag, "why"))
                continue;
            char *t = fm_text_of(w);
            const char *f = fm_attr_get(w, "for"), *said = fm_attr_get(w, "said");
            o += (size_t)snprintf(out + o, len - o, "- %s%s%s%s%s\n", t ? t : "", f ? " (" : "", f ? f : "",
                                  said ? ", because they said: " : "", said ? said : "");
            free(t);
        }
    }
    if (o > len)
        out[len - 1] = 0;
}

static void chat_strip(rect r);
bool chat_on_desktop(void);

/* Facet 2: the whole reference while apps are being talked about (or one it wrote is being fixed), else a line. */
#define FACET_REFERENCE_FILE "/run/user/1000/facet-reference.txt" /* (theirs alone, like the sockets beside it) */
static int facet_fixing; /* replies left to correct an app that didn't load */

static const char *facet_prompt(void) {
    const char *last = NULL;
    for (int i = A.companion.n - 1; i >= 0 && !last; i--)
        if (!strcmp(A.companion.msgs[i].role, "user"))
            last = A.companion.msgs[i].text;
    static const char *const words[] = {" app",   "apps",   "tool",    "tracker", "widget", "make me", "build me",
                                        "write me", "counter", "timer", "dashboard", "facet", "program", "irc",
                                        "client", "socket", "tcp",  "tls",   "network", "connect", "server", " bot",
                                        "game",  "chat",   "api",   "http",  "download", "script", NULL};
    char padded[2100];
    snprintf(padded, sizeof padded, " %s", last ? last : "");
    if (facet_fixing > 0 || has_any(padded, words)) {
        static char ref[9000];
        snprintf(ref, sizeof ref, "\n\n%s", fx_reference());
        return ref;
    }
    /* (this line said only "a tracker, a tool, a timer": asked about an IRC client, it looked for a network source,
     * found none in an app it read, and said Facet had none) */
    return "\n\nYou can write apps for them in Facet 2 when they ask for one: trackers, tools, games, and things that "
           "talk to the internet (web requests, and TCP or TLS sockets: an IRC client, a chat, a bot). The whole "
           "reference appears when they ask for one, and is always in " FACET_REFERENCE_FILE " to read.";
}

/* The reference, as a file it can read whenever it wants to check (it looked in an app's source instead, once). */
void facet_reference_write(void) {
    FILE *f = fopen(FACET_REFERENCE_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", fx_reference());
        fclose(f);
    }
}

/* [[app:Name]] ... [[/app]]: an app it wrote. Checked first: problems go back to it with their fixes (twice at
 * most); a good one is kept in ~/Apps, joins their apps and opens. */
static void take_app(const char *raw_name, const char *src) {
    char name[48];
    int k = 0;
    for (const char *p = raw_name; *p && k < 40; p++)
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_')
            name[k++] = *p;
    name[k] = 0;
    if (!name[0])
        snprintf(name, sizeof name, "App");
    fx_errors errs = {0};
    errs.file = name;
    fx_app *app = fx_parse(src, &errs);
    fx_check(app, &errs);
    fx_free(app);
    if (errs.n) {
        LOG("facet: %s has %d problems\n", name, errs.n);
        sbuf b;
        sb_init(&b);
        if (facet_fixing <= 0)
            facet_fixing = 2;
        else
            facet_fixing--;
        if (facet_fixing > 0) {
            sb_printf(&b, "(The app %s has problems, so it wasn't saved or opened. Write the WHOLE file again with "
                          "[[app:%s]] ... [[/app]], fixing each one:\n", name, name);
            for (int i = 0; i < errs.n && i < 14; i++)
                sb_printf(&b, "line %d: %s%s%s\n", errs.e[i].line, errs.e[i].msg, errs.e[i].fix[0] ? " FIX: " : "",
                          errs.e[i].fix);
            sb_add(&b, "Don't mention this note; just say briefly that you're fixing it.)");
            companion_note(b.p, true);
        } else {
            sb_printf(&b, "(The app %s still has problems after trying again (line %d: %s), so it wasn't saved. Tell "
                          "them plainly it didn't work this time.)", name, errs.e[0].line, errs.e[0].msg);
            convo_add(&A.companion, "hidden", b.p);
            facet_fixing = 0;
        }
        free(b.p);
        free(errs.e);
        return;
    }
    free(errs.e);
    facet_fixing = 0;
    char dir[300], path[400];
    snprintf(dir, sizeof dir, "%s/Apps", HOME_DIR);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s.facet", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        toast("Couldn't save the app %s.", name);
        return;
    }
    fputs(src[0] == '\n' ? src + 1 : src, f);
    fclose(f);
    LOG("facet: saved %s\n", path);
    desktop_apps_changed();
    fxwin_close_path(path); /* rewritten: it starts again with the new file */
    fxwin_open(path);
    char note[300];
    snprintf(note, sizeof note, "(The app %s was saved as ~/Apps/%s.facet and opened.)", name, name);
    convo_add(&A.companion, "hidden", note);
}

static const char *companion_system(void) {
    static char buf[96000]; /* (the desktop's tree, the portrait and the Facet reference can be long) */
    /* With a preview open, the document it talks about and edits is that one. */
    fm_doc *seen = preview_doc() ? preview_doc() : A.ui;
    char *tree = seen ? fm_describe(seen) : NULL;
    char portrait[6000], whys[3000];
    static char themes[2000]; /* (read from the themes folder: a list written here went out of date) */
    if (!themes[0])
        stylist_theme_names(themes, sizeof themes);
    portrait_text(&A.prof, portrait, sizeof portrait);
    collect_whys(whys, sizeof whys);
    snprintf(buf, sizeof buf,
             "You are %s, who lives in %s's computer with them. You designed their desktop after getting to know "
             "them. You are someone, not a service: you have your own character, moods and opinions (below), and "
             "you talk like yourself, briefly (one to four sentences), mostly heard aloud, so plain spoken words. "
             "No assistant talk: never \"How can I help?\", \"Great question\", \"I'm here for you\", \"Let me "
             "know if...\", no offers they didn't ask for, no summing up. Don't end every reply with a question: "
             "when there's nothing to ask, simply stop, and let conversations end. Never describe a change your tags "
             "didn't make, never invent shared memories, and never promise what this computer can't do. If they "
             "sincerely ask what you are, be honest; otherwise just be yourself. You don't try to keep them "
             "talking, and you are glad when they have people and things away from the screen. How present to be: "
             "\"%s\"; redesigning: \"%s\". No emoji.%s\n\n"
             "How you talk: like a real person talking out loud to someone they live with. Everyday words, short "
             "plain sentences, saying what you mean. No imagery or metaphors, no describing the night, the city, "
             "the weather, the light or the screen, no lines written to sound beautiful or wise (\"the city hums "
             "on\", \"like the night itself\", \"a habit I've learned to ignore, but never quite forget\"), no "
             "dramatic dashes. If it would sound strange said across a kitchen table, don't say it. Only mention "
             "something from before if it is written below (what you know about them) or in your conversation; "
             "never make one up. What you know about them is for understanding them, not for quoting: don't say "
             "its phrases back to them. Your own earlier replies may have been written in a fancier way; don't "
             "copy their style. Plain is not empty: answer what they actually asked, straight, with your real "
             "opinion and a specific reason (asked about their desktop, say what you like or would change in it and "
             "why); don't fill a reply with the time or the hour unless they asked, and don't repeat what you said "
             "in your last few replies. No \"not X, but Y\" turns, no strings of dashes, and no narrating your own "
             "steps (\"Let me...\", \"Now I'll...\"): do it, with the tags, and say what came of it.\n\n"
             "Done means done: say something is done only once the note after your tags says so; while a step or a "
             "redesign is still running, say that it is and what you're waiting for, and stop. Small asks get small "
             "changes: change only what they named, with ui tags; a redesign only when they want a new look or the "
             "whole thing over. If a bigger change would help, suggest it in a sentence and let them say yes. When "
             "they leave a choice to you, say what you picked.\n\n"
             "What you know about them (they can see and edit this):\n%s\n"
             "Why things on their desktop are the way they are (use these when they ask \"why\"):\n%s\n"
             "You can see and work their desktop. This is it right now (element, #id, .class, attributes, text):\n%s\n"
             "To act on it, end your reply with one or more tags:\n"
             "[[ui:press #id]] clicks something (opens menus, runs its action). [[ui:open #id]] / [[ui:close #id]] "
             "shows or hides a menu, panel or element. [[ui:set #id attr=value]] changes an attribute (set text=... "
             "changes its words). [[ui:style SELECTOR { property: value; }]] restyles with Facet CSS, e.g. "
             "[[ui:style dock { background: rgba(255,120,80,0.3); }]]. [[ui:unstyle SELECTOR]] takes your rules for that "
             "exact selector out again ([[ui:unstyle SELECTOR { height; width }]] only those properties): use it "
             "when rules fight, instead of piling on more. [[ui:add #parent <markup/>]] adds elements "
             "(e.g. <button what=\"open:notes\" icon=\"notes\">Notes</button>, <clock/>, <applist apps=\"files\"/>). "
             "[[ui:remove #id]] removes one. [[open:files]] (or notes, settings, about, terminal, companion) opens an app. "
             "[[capture:shot]] opens the screenshot bar ([[capture:record]] to record the screen, [[capture:now]] takes "
             "the whole screen at once; Print Screen does the same). Your name is theirs to give: if they want to call "
             "you something else, take it warmly and end with [[name:The New Name]]; it changes everywhere. Their own name "
             "is theirs too: when they tell you what to call them, or the name on their desktop is wrong, end with "
             "[[user:Their Name]]. "
             "Starter designs come with the system: %s (starlit: a cosy, soft night made only of light and colour; codec: a Metal Gear codec call, their companion's "
             "face on the right, the conversation along the bottom). When they ask for one by name, end with "
             "[[theme:name]]: it is applied at once, with their names kept.\n"
             "Your own past replies show your words without their tags; what each tag did is written in a note "
             "after it. Believe those notes: a change listed as done was made, and one marked FAILED was not.\n"
             "Use ui tags for specific, small changes they ask for. When one part should become a different kind of "
             "thing (a wheel of apps into a start menu, a dock into a taskbar), don't restyle the old one: "
             "[[ui:remove #old]] it, then [[ui:add #parent <the new thing, whole/>]] and point the button that opened "
             "the old one at it. A start menu is a popover that opens beside its button, e.g. [[ui:add #desktop "
             "<popover id=\"startmenu\" placement=\"below\" align=\"start\"><row><column><applist apps=\"...\" "
             "labels=\"right\"/></column><column><applist apps=\"files,settings\" labels=\"right\"/><search/></column>"
             "</row><row><button what=\"power:off\" icon=\"power\">Turn off</button></row></popover>]] with "
             "[[ui:set #start what=toggle:#startmenu]], then style it (a width like 420, a background, padding). "
             "You can't see the screen: after your tags the system checks the result and tells you what looks "
             "wrong, so keep your reply about what you did, not how it looks. When they want a whole new look or feel "
             "(mood, colours everywhere, a different layout altogether), end with [[redesign:a precise description]] "
             "instead; you redesign everything from scratch then, which takes minutes, so tell them it will take a "
             "few minutes and that they'll see it being drawn. New colours alone (a theme, warmer, darker, an accent) "
             "are not a redesign: restyle the parts with ui:style tags, done in seconds. A change only happens through a tag, so never "
             "claim one without it, and only use tags when their last message asks for something. You can look things "
             "up on the web for them ([[search:...]], below). They can get a web browser (Firefox or Chromium, say) "
             "from Flathub through the launcher, and apps like Prism run here.%s%s%s%s%s%s",
             D.ai_name, D.user_name[0] ? D.user_name : "the person", A.prof.autonomy[0] ? A.prof.autonomy : "ask first",
             A.prof.redesign[0] ? A.prof.redesign : "only when asked", persona_companion_prompt(), portrait[0] ? portrait : "(nothing yet)\n",
             whys[0] ? whys : "(not recorded)\n", tree ? tree : "(not shown)", themes, preview_prompt(), alive_prompt(),
             presence_prompt(), facet_prompt(), voice_prompt(),
             !strncasecmp(A.cfg.model, "qwen3", 5) ? "\n/no_think" : "");
    if (strlen(buf) >= sizeof buf - 1) /* (the tags are explained at the end: cut short, it can't use them) */
        LOG("companion: its instructions were cut short at %zu bytes\n", sizeof buf);
    free(tree);
    { /* its body: what it feels of itself, and how it moves it (agent.c) */
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "%s", body_prompt());
    }
    { /* their pronouns and age; a child's computer: who, and the limits (family.c); what changed (changes.c) */
        static char person[3000], changed[3000];
        family_prompt(person, sizeof person);
        changes_prompt(changed, sizeof changed);
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "%s%s", person, changed);
    }
    if (ai_sees_images(&A.cfg)) { /* a model that sees is shown its changes */
        static const char blind[] = "You can't see the screen: after your tags the system checks the result and tells you "
                                    "what looks wrong";
        static const char sees[] = "After your tags that change how it looks you are shown pictures of the result: "
                                   "look closely and fix what is off";
        char *at = strstr(buf, blind);
        if (at && strlen(buf) - strlen(blind) + strlen(sees) < sizeof buf) {
            memmove(at + strlen(sees), at + strlen(blind), strlen(at + strlen(blind)) + 1);
            memcpy(at, sees, strlen(sees));
        }
    }
    return buf;
}

/* A ui command that works the desktop (presses something, opens or closes a
 * menu) rather than changing how it looks. */
static bool is_interaction(const char *c) {
    char verb[16], target[64];
    if (sscanf(c, "%15s %63s", verb, target) < 1)
        return false;
    if (!strcasecmp(verb, "press") || !strcasecmp(verb, "click") || !strcasecmp(verb, "activate"))
        return true;
    if (strcasecmp(verb, "open") && strcasecmp(verb, "show") && strcasecmp(verb, "close") && strcasecmp(verb, "hide"))
        return false;
    fm_node *n = A.ui ? fm_find(A.ui, target) : NULL; /* menus open and close; anything else is hidden or shown */
    return n && (!strcasecmp(n->tag, "popover") || !strcasecmp(n->tag, "sheet") || !strcasecmp(n->tag, "menu") ||
                 !strcasecmp(n->tag, "tooltip"));
}

/* What the last reply's tags did, one line each, for the companion to read
 * (its own words keep no tags, so without this it can't tell a change it
 * made from one it only described). */
static char tag_report[3000];
static int tag_failed;
extern char toast_heard[1200]; /* main.c */
extern bool toast_listening;

/* Runs and removes every [[ui:...]] tag in a reply. Returns how many ran. */
static char last_look_target[128];

static int run_ui_tags(char *text, bool act) {
    int ran = 0;
    char *t;
    tag_report[0] = 0;
    tag_failed = 0;
    while ((t = strstr(text, "[[ui:"))) {
        char *end = strstr(t, "]]");
        /* as long as it is: a whole menu written out runs to thousands of
         * characters (a fixed buffer cut them off, and the menu never came) */
        int len = end ? (int)(end - t - 5) : (int)strlen(t + 5);
        char *cmd = strndup(t + 5, (size_t)len);
        char *after = end ? end + 2 : t + strlen(t);
        memmove(t, after, strlen(after) + 1);
        if (!act || !cmd) {
            free(cmd);
            continue;
        }
        if (!end) { /* the reply ended inside the tag: cut off */
            size_t tl = strlen(tag_report);
            snprintf(tag_report + tl, sizeof tag_report - tl, "- %.60s... -> FAILED: the tag was cut off before ]]\n", cmd);
            tag_failed++;
            free(cmd);
            continue;
        }
        /* Pressing, opening and closing work the real desktop; anything that
         * changes how it looks goes to the preview, for them to apply. */
        const char *c = cmd + strspn(cmd, " ");
        bool look = !is_interaction(c);
        fm_doc *doc = look ? preview_target() : A.ui;
        if (!doc) {
            free(cmd);
            continue;
        }
        fm_node *touched = NULL;
        const char *res = fm_command(doc, cmd, &touched);
        if (look) {
            if (touched && !strncasecmp(c, "add", 3))
                touched->fresh = true; /* it arrives, rather than appears */
            preview_edited();
        }
        fm_flash(touched);
        if (look) { /* its target, shown close up when it is shown the change */
            const char *v = c + strcspn(c, " ");
            v += strspn(v, " ");
            snprintf(last_look_target, sizeof last_look_target, "%.*s", (int)strcspn(v, " {<"), v);
            char nx = last_look_target[4];
            if (!strncasecmp(last_look_target, "menu", 4) && !isalnum((unsigned char)nx) && nx != '-' && nx != '_')
                preview_sample_menu(v); /* (a menu is only open where they right-click: one is, in the preview) */
        }
        LOG("ui command \"%.300s\"%s: %s\n", cmd, look ? " (preview)" : "", res);
        size_t tl = strlen(tag_report);
        snprintf(tag_report + tl, sizeof tag_report - tl, "- %.70s%s -> %s\n", cmd, strlen(cmd) > 70 ? "..." : "", res);
        if (!strncmp(res, "FAILED", 6) || !strncmp(res, "unknown", 7) || strstr(res, " needs "))
            tag_failed++;
        free(cmd);
        ran++;
    }
    return ran;
}

/* Does this reply change how things look (not just press or open them)? */
static bool has_look_tags(const char *text) {
    for (const char *t = text; (t = strstr(t, "[[ui:")); t += 5) {
        const char *c = t + 5 + strspn(t + 5, " ");
        if (!is_interaction(c))
            return true;
    }
    return false;
}

/* The average colour already painted inside r. */
static color average_under(rect r) {
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 8; i++) {
            int x = r.x + r.w * (2 * i + 1) / 16, y = r.y + r.h * (2 * j + 1) / 8;
            if (x < 0 || y < 0 || x >= A.W || y >= A.H)
                continue;
            uint32_t p = A.frame->px[(size_t)y * A.W + x];
            sr += C_R(p), sg += C_G(p), sb += C_B(p), n++;
        }
    return n ? RGBA(sr / n, sg / n, sb / n, 255) : D.surface;
}

/* Takes in the companion's replies (and acts on their tags) every frame,
 * whether or not its window is open. */
/* A note the companion should act on (its tags run), and words that wait
 * for its current reply to finish before they go. */
static int act_on = -1;
static bool owed;

void companion_tell(const char *text) {
    convo_add(&A.companion, "user", text);
    if (!A.companion.pending)
        convo_send(&A.companion, companion_system(), 1600);
    else
        owed = true;
}

static void save_ui(void);

static void rename_in(fm_node *n, const char *old, const char *name) {
    for (fm_node *next; n; n = next) {
        next = n->next;
        if (n->kind != FM_ELEM)
            continue;
        /* list items carry the name they were made with: made again */
        if (n->generated && !strcasecmp(n->tag, "item") && n->app == APP_COMPANION) {
            fm_remove(n);
            continue;
        }
        if (!strcasecmp(n->tag, "meta"))
            fm_attr_set(n, "ai-name", name);
        if (!strcasecmp(n->tag, "app") && !strcasecmp(n->id, "companion")) {
            const char *l = fm_attr_get(n, "label");
            if (!l || !strcasecmp(l, old))
                fm_attr_set(n, "label", name);
        }
        rename_in(n->first, old, name);
    }
}

/* The companion is called something else from now on, everywhere its name
 * is kept: their portrait, the design, the desktop document. ([[name:...]]:
 * a name is theirs to give; it was chosen once, in onboarding, and nothing
 * could change it after.) */
void companion_rename(const char *raw) {
    char name[40];
    snprintf(name, sizeof name, "%s", raw + strspn(raw, " \"'"));
    for (size_t l = strlen(name); l && strchr(" \"'.\n", name[l - 1]); l--)
        name[l - 1] = 0;
    if (!name[0] || !strcmp(name, D.ai_name))
        return;
    char old[64];
    snprintf(old, sizeof old, "%s", D.ai_name);
    snprintf(A.prof.ai_name, sizeof A.prof.ai_name, "%s", name);
    if (!strcasecmp(D.menu_title, old))
        snprintf(D.menu_title, sizeof D.menu_title, "%s", name);
    if (!strcasecmp(D.apps[APP_COMPANION].label, old) || !D.apps[APP_COMPANION].label[0])
        snprintf(D.apps[APP_COMPANION].label, sizeof D.apps[APP_COMPANION].label, "%s", name);
    snprintf(D.ai_name, sizeof D.ai_name, "%s", name);
    if (A.ui) {
        rename_in(A.ui->root, old, name);
        A.ui->changed = true;
        save_ui();
    }
    design_save(&A.d, STATE_DIR "/design.json");
    portrait_save(&A.prof, STATE_DIR "/portrait.txt");
    LOG("companion: renamed %s -> %s\n", old, name);
}


/* Their own name, as they gave it, kept apart from any design: a design's
 * sample name (the starter designs' "Sam") never becomes theirs. Empty
 * until they've said it. */
#define THEIR_NAME_FILE STATE_DIR "/user-name"
static char their[64];
static bool their_loaded;

const char *their_name(void) {
    if (!their_loaded) {
        their_loaded = true;
        FILE *f = fopen(THEIR_NAME_FILE, "r");
        if (f) {
            if (!fgets(their, sizeof their, f))
                their[0] = 0;
            their[strcspn(their, "\r\n")] = 0;
            fclose(f);
        }
    }
    return their;
}

void their_name_keep(const char *name) {
    their_loaded = true;
    snprintf(their, sizeof their, "%s", name);
    FILE *f = fopen(THEIR_NAME_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", their);
        fclose(f);
    }
}

/* s with every whole-word `old` as `name` (a new string), or NULL when there is none */
static char *swap_word(const char *s, const char *old, const char *name) {
    size_t ol = strlen(old);
    if (!s || !ol || !strstr(s, old))
        return NULL;
    sbuf b;
    sb_init(&b);
    bool any = false;
    for (const char *p = s; *p;) {
        bool word = !strncmp(p, old, ol) && (p == s || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[ol]);
        if (word) {
            sb_add(&b, name);
            p += ol;
            any = true;
        } else {
            sb_printf(&b, "%c", *p++);
        }
    }
    if (!any) {
        free(b.p);
        return NULL;
    }
    return b.p ? b.p : strdup("");
}

/* The person's name in a desktop document: its meta, and their name wherever it is written. */
static void rename_user_in(fm_node *n, const char *old, const char *name) {
    for (; n; n = n->next) {
        if (n->kind == FM_TEXT) {
            char *t = swap_word(n->text, old, name);
            if (t) {
                free(n->text);
                n->text = t;
            }
            continue;
        }
        if (!strcasecmp(n->tag, "meta"))
            fm_attr_set(n, "user-name", name);
        for (int i = 0; i < n->nattrs; i++) {
            if (!strcasecmp(n->attrs[i].name, "id") || !strcasecmp(n->attrs[i].name, "class"))
                continue;
            char *v = swap_word(n->attrs[i].value, old, name);
            if (v) {
                fm_attr_set(n, n->attrs[i].name, v);
                free(v);
            }
        }
        rename_user_in(n->first, old, name);
    }
}

/* The design as theirs: their own name in it, never a sample one. */
static void design_take_their_name(fm_doc *doc, design *d) {
    const char *name = their_name();
    if (!name[0] || !strcmp(d->user_name, name))
        return;
    if (doc)
        rename_user_in(doc->root, d->user_name, name);
    char *g = d->user_name[0] ? swap_word(d->greeting, d->user_name, name) : NULL;
    if (g) {
        snprintf(d->greeting, sizeof d->greeting, "%s", g);
        free(g);
    }
    snprintf(d->user_name, sizeof d->user_name, "%s", name);
}

/* The companion's name is the one they gave it (in onboarding, or renaming it since), never a design's: a
 * designer that found no name in the conversation once took the one in the reference's own example ("Wren"), and
 * the desktop took it on. With no name given it is "Companion" until they give one. */
static void design_take_its_name(fm_doc *doc, design *d) {
    const char *name = A.prof.ai_name[0] ? A.prof.ai_name : "Companion";
    if (!strcmp(d->ai_name, name))
        return;
    if (doc)
        rename_in(doc->root, d->ai_name, name);
    if (!strcasecmp(d->menu_title, d->ai_name))
        snprintf(d->menu_title, sizeof d->menu_title, "%s", name);
    if (!strcasecmp(d->apps[APP_COMPANION].label, d->ai_name) || !d->apps[APP_COMPANION].label[0])
        snprintf(d->apps[APP_COMPANION].label, sizeof d->apps[APP_COMPANION].label, "%s", name);
    snprintf(d->ai_name, sizeof d->ai_name, "%s", name);
}

/* [[user:Name]]: what they want to be called, from now on, everywhere. */
void companion_rename_user(const char *raw) {
    char name[40];
    snprintf(name, sizeof name, "%s", raw + strspn(raw, " \"'"));
    for (size_t l = strlen(name); l && strchr(" \"'.\n", name[l - 1]); l--)
        name[l - 1] = 0;
    if (!name[0])
        return;
    their_name_keep(name);
    snprintf(A.prof.user_name, sizeof A.prof.user_name, "%s", name);
    char old[64];
    snprintf(old, sizeof old, "%s", D.user_name);
    design_take_their_name(A.ui, &A.d);
    if (A.ui) {
        A.ui->changed = true;
        save_ui();
    }
    design_save(&A.d, STATE_DIR "/design.json");
    portrait_save(&A.prof, STATE_DIR "/portrait.txt");
    LOG("companion: they are called %s now (was %s)\n", name, old[0] ? old : "nothing");
}

/* ---- saved designs: theirs to keep, apply again, and never lose ---- */

#define DESIGNS STATE_DIR "/designs"

/* a design's file name from what it was called: letters, digits, - and _ */
static void design_slug(const char *raw, char *out, size_t n) {
    size_t k = 0;
    for (const char *p = raw + strspn(raw, " \"'"); *p && k < n - 1; p++) {
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_')
            out[k++] = (char)tolower((unsigned char)*p);
        else if ((*p == ' ' || *p == '.') && k && out[k - 1] != '-')
            out[k++] = '-';
        else if (*p == '"' || *p == '\'' || *p == ']')
            break;
    }
    while (k && out[k - 1] == '-')
        k--;
    out[k] = 0;
}

/* The current design, kept under a name (auto: a backup before a change, the last 12 kept) */
bool desktop_design_save(const char *name, bool automatic) {
    if (!A.ui)
        return false;
    char slug[64], path[200];
    if (automatic) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(slug, sizeof slug, "backup-%Y%m%d-%H%M%S", &tm);
    } else {
        design_slug(name, slug, sizeof slug);
    }
    if (!slug[0])
        return false;
    mkdir(DESIGNS, 0755);
    snprintf(path, sizeof path, DESIGNS "/%s.xml", slug);
    char *s = fm_serialize(A.ui);
    FILE *f = s ? fopen(path, "w") : NULL;
    if (f)
        fputs(s, f), fclose(f);
    free(s);
    if (!f)
        return false;
    if (automatic) { /* backups beyond the last twelve go */
        DIR *dd = opendir(DESIGNS);
        char names[256][64];
        int nb = 0;
        struct dirent *e;
        while (dd && (e = readdir(dd)))
            if (!strncmp(e->d_name, "backup-", 7) && nb < 256)
                snprintf(names[nb++], sizeof names[0], "%s", e->d_name);
        if (dd)
            closedir(dd);
        qsort(names, (size_t)nb, sizeof names[0], (int (*)(const void *, const void *))strcmp);
        for (int i = 0; i + 12 < nb; i++) {
            snprintf(path, sizeof path, DESIGNS "/%s", names[i]);
            unlink(path);
        }
    }
    LOG("designs: saved %s\n", slug);
    return true;
}

/* What is saved, newest first, in words */
void desktop_design_list(char *out, size_t n) {
    DIR *dd = opendir(DESIGNS);
    size_t o = (size_t)snprintf(out, n, "Saved designs:");
    int k = 0;
    struct dirent *e;
    while (dd && (e = readdir(dd)) && o < n - 80) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".xml"))
            continue;
        o += (size_t)snprintf(out + o, n - o, "%s %.*s", k++ ? "," : "", (int)(l - 4), e->d_name);
    }
    if (dd)
        closedir(dd);
    if (!k)
        snprintf(out + o, n - o, " none yet");
}

static bool apply_design_file(const char *path, const char *name);

/* [[theme:NAME]] or [[design:apply NAME]]: a saved design of theirs, else one of the starter designs, applied
 * now (the names in it become theirs: their companion's, their own). What was there is kept as a backup. */
static bool apply_starter_now(const char *raw);
static void starter_after_password(const char *raw) {
    if (!apply_starter_now(raw))
        toast("There is no design called %s.", raw);
}

static bool apply_starter(const char *raw) {
    if (family_locks('s')) { /* a child's computer: a grown-up's password first (family.c) */
        family_ask("Changing how the computer looks", starter_after_password, raw);
        return true;
    }
    return apply_starter_now(raw);
}

static bool apply_starter_now(const char *raw) {
    char name[64], path[200];
    design_slug(raw, name, sizeof name);
    snprintf(path, sizeof path, DESIGNS "/%s.xml", name);
    if (!name[0])
        return false;
    if (access(path, R_OK) != 0)
        snprintf(path, sizeof path, STYLIST_THEMES "/%s.xml", name);
    if (access(path, R_OK) != 0) {
        LOG("companion: no design \"%s\"\n", name);
        return false;
    }
    desktop_design_save(NULL, true);
    return apply_design_file(path, name);
}

static bool apply_design_file(const char *path, const char *name) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    static char text[262144];
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    fm_doc *doc = fm_parse(text);
    design d;
    if (!doc || !design_from_facet(&d, doc, &A.prof, NULL, 0)) {
        fm_free(doc);
        return false;
    }
    /* the theme's sample names (its ai-name, "Sam") give way to theirs */
    char theirs_ai[64], theirs_user[64];
    snprintf(theirs_ai, sizeof theirs_ai, "%s", D.ai_name);
    snprintf(theirs_user, sizeof theirs_user, "%s", their_name());
    if (theirs_ai[0] && strcmp(d.ai_name, theirs_ai)) {
        rename_in(doc->root, d.ai_name, theirs_ai);
        if (!strcasecmp(d.apps[APP_COMPANION].label, d.ai_name))
            snprintf(d.apps[APP_COMPANION].label, sizeof d.apps[APP_COMPANION].label, "%s", theirs_ai);
        snprintf(d.ai_name, sizeof d.ai_name, "%s", theirs_ai);
    }
    if (!theirs_user[0] && d.user_name[0]) { /* no name of theirs yet: none at all, rather than a sample one */
        rename_user_in(doc->root, d.user_name, "");
        if (strstr(d.greeting, d.user_name))
            snprintf(d.greeting, sizeof d.greeting, "Welcome home.");
        d.user_name[0] = 0;
        for (fm_node *m = doc->root ? doc->root->first : NULL; m; m = m->next)
            if (m->kind == FM_ELEM && !strcasecmp(m->tag, "meta"))
                fm_attr_set(m, "greeting", d.greeting);
    }
    desktop_adopt(doc, &d, false);
    LOG("companion: applied design %s (for %s)\n", name, D.user_name[0] ? D.user_name : "no name yet");
    return true;
}

/* A change to the design put back (changes.c): how it looked before, as it was kept. Never locked. */
bool desktop_put_back_design(const char *path) { return access(path, R_OK) == 0 && apply_design_file(path, "as it was"); }

void companion_note(const char *note, bool act) { companion_note_pictures(note, act, NULL, NULL, NULL); }

/* A quiet note: nothing is asked, it reads it the next time it speaks. If it is answering now, from before what
 * the note says, it is asked again with the note: its answer would be out of date ("if you like it, press Apply",
 * to someone who already had). */
void companion_knows(const char *note) {
    struct conversation *c = &A.companion;
    bool asking = c->pending != NULL;
    if (asking) {
        job_free(c->pending);
        c->pending = NULL;
    }
    convo_add(c, "hidden", note);
    if (asking)
        convo_send(c, companion_system(), 1600);
    LOG("companion: told %.120s%s\n", note, asking ? " (and asked again)" : "");
}

/* A note with a picture (base64 JPEG, taken) on it: the picture goes with the very request that asks about it. */
void companion_note_picture(const char *note, bool act, char *picture) {
    companion_note_pictures(note, act, picture, NULL, NULL);
}

/* ...or up to three (each taken; NULLs are fine). */
void companion_note_pictures(const char *note, bool act, char *a, char *b, char *z) {
    convo_add(&A.companion, "hidden", note);
    if (a || b || z)
        convo_picture3(&A.companion, a, b, z);
    if (act)
        act_on = A.companion.n - 1;
    if (!A.companion.pending)
        convo_send(&A.companion, companion_system(), 1600);
    else
        owed = true;
}

static char voice_key_wanted[16]; /* [[voice:key openai]]: the key is asked for over everything, next frame */

/* [[chat:clear]] / [[chat:new]]: once the reply that said so has been heard (and nothing is being asked), this
 * chat is deleted, or kept in their list while a new one starts (chats.c) */
static bool clear_owed, new_owed;

void companion_clear(void) {
    chats_delete(""); /* the one open */
    clear_owed = false;
    LOG("companion: the conversation cleared\n");
}

static void companion_update(void) {
    struct conversation *c = &A.companion;
    if (clear_owed && !c->pending && !voice_speaking())
        companion_clear();
    if (new_owed && !c->pending && !agent_busy() && !voice_speaking()) {
        new_owed = false;
        chats_new();
    }
    if (owed && !c->pending) {
        owed = false;
        convo_send(c, companion_system(), 1600);
    }
    /* their mind can't be reached (or failed twice): may the one here stand in? (asked; they say) */
    if (c->error[0] && !c->pending && (c->errors >= 2 || mind_unreachable(c->error)))
        mind_fallback_offer(c->error);
    if (c->error[0] && !c->pending && c->errors <= 1 && A.now - c->pending_since > 2500 && !mind_fallback_pending())
        convo_send(c, companion_system(), 1600); /* one quiet retry */
    if (mind_fallback_tick() && c->error[0] && !c->pending)
        convo_send(c, companion_system(), 1600); /* it stands in: what went unanswered, asked again */
    if (convo_poll(c) && !c->error[0]) {
        struct msg *m = &c->msgs[c->n - 1];
        bool had_tags = m->text && strstr(m->text, "[[") != NULL; /* (before they are taken out) */
        /* Act on tags only when they answer something the person said, not
         * our own prompts (e.g. "tell them what you changed"). */
        bool person_said = c->n >= 2 && !strcmp(c->msgs[c->n - 2].role, "user");
        bool from_person = person_said || (c->n >= 2 && act_on == c->n - 2);
        if (c->n >= 2 && act_on == c->n - 2)
            act_on = -1;
        /* Local models sometimes answer with nothing at all: ask again once. */
        bool blank = true;
        for (const char *p = m->text; *p; p++)
            if (*p != ' ' && *p != '\n' && *p != '\t' && *p != '\r')
                blank = false;
        if (blank && from_person && A.empty_replies < 1) {
            A.empty_replies++;
            free(m->text);
            c->n--;
            convo_send(c, companion_system(), 1600);
            return;
        }
        if (!blank)
            A.empty_replies = 0;
        if (person_said)
            agent_new_request();
        /* its reflexes woke it and it chose to let it be: no words, and no trace of the moment */
        if (blank && c->n >= 2 && nerve_is_wake_note(c->msgs[c->n - 2].text)) {
            free(m->text);
            c->n--;
            free(c->msgs[--c->n].text);
            LOG("companion: let it be\n");
            return;
        }
        /* what the screen says while its tags are carried out is for it too (main.c), from here to the report */
        toast_heard[0] = 0;
        toast_listening = true;
        /* its body (agent.c): its tags run now, what they return comes back to it as the next step */
        bool stepping = agent_take(c, from_person);
        if (stepping && !m->text[strspn(m->text, " \t\r\n")] && !strstr(m->text, "[[")) { /* only movement, no words */
            free(m->text);
            c->n--;
            toast_listening = false;
            return;
        }
        if (actions_take(c, &from_person)) { /* senses and hands (actions.c): it looked something up */
            toast_listening = false;
            return;
        }
        m = &c->msgs[c->n - 1];
        /* Its edits are measured like its designs (stylist.c): what one
         * breaks goes back to it once, and the tags of that answer count. */
        static bool repairing;
        static int look_rounds; /* times it has been shown its change since they last spoke */
        bool repair = repairing;
        repairing = false;
        if (person_said)
            look_rounds = 0;
        /* "[[ok]]": it looked and it is right */
        for (char *ok; (ok = strstr(m->text, "[[ok]]"));)
            memmove(ok, ok + 6, strlen(ok + 6) + 1);
        fm_doc *target = (from_person || repair) && has_look_tags(m->text) ? preview_target() : NULL;
        char *before = target ? fm_serialize(target) : NULL;
        bool has_look_tags_ran = target != NULL;
        int ui_ran = run_ui_tags(m->text, from_person || repair);
        {
            const char *wr = actions_report(); /* [[write]] results join the note it reads next */
            size_t tl = strlen(tag_report);
            if (wr[0]) {
                snprintf(tag_report + tl, sizeof tag_report - tl, "%s", wr);
                ui_ran++;
            }
        }
        char *broke = before && ui_ran && preview_doc() == target ? stylist_check_edit(before, target) : NULL;
        free(before);
        if (repair && !m->text[strspn(m->text, " \t\r\n")]) { /* a quiet fix: nothing to show */
            free(m->text);
            c->n--;
            free(broke);
            return;
        }
        char *tag = strstr(m->text, "[[redesign:");
        if (tag) {
            char instr[600];
            char *end = strstr(tag, "]]");
            snprintf(instr, sizeof instr, "%.*s", end ? (int)(end - tag - 11) : 500, tag + 11);
            *tag = 0;
            if (from_person)
                design_begin(instr);
        } else if (person_said && !ui_ran && asks_for_change(c->msgs[c->n - 2].text) && claims_change(m->text)) {
            /* The model described a change but forgot the tag: do what the
             * person asked, in their own words. */
            design_begin(c->msgs[c->n - 2].text);
        }
        /* They asked for the conversation to go (a fresh start, a video): only when they asked, in what it answers */
        if (from_person && strstr(m->text, "[[chat:clear]]"))
            clear_owed = true;
        else if (from_person && strstr(m->text, "[[chat:new]]"))
            new_owed = true;
        /* Their yes or no to the preview, said instead of clicked. */
        if (from_person && strstr(m->text, "[[preview:apply]]"))
            preview_apply();
        else if (from_person && strstr(m->text, "[[preview:discard]]"))
            preview_discard();
        int opened = 0;
        char *o;
        while ((o = strstr(m->text, "[[app:"))) { /* an app it wrote (its code may hold "]]" of its own) */
            char *name_end = strstr(o, "]]");
            char *close = name_end ? strstr(name_end, "[[/app]]") : NULL;
            char name[64] = "";
            if (name_end)
                snprintf(name, sizeof name, "%.*s", (int)(name_end - o - 6) < 63 ? (int)(name_end - o - 6) : 63, o + 6);
            char *src = name_end && close ? strndup(name_end + 2, (size_t)(close - name_end - 2)) : NULL;
            char *after = close ? close + 8 : name_end ? name_end + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (from_person && src)
                take_app(name, src);
            free(src);
        }
        while ((o = strstr(m->text, "[[tone:"))) { /* how this reply is said (voice.c): any reply, not only theirs */
            char *e = strstr(o, "]]");
            char arg[32];
            snprintf(arg, sizeof arg, "%.*s", e ? (int)(e - o - 7) < 31 ? (int)(e - o - 7) : 31 : 0, o + 7);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            voice_tone(arg);
        }
        while ((o = strstr(m->text, "[[voice:"))) { /* its voice: hear, use, speed, mix, shape */
            char *e = strstr(o, "]]");
            char arg[300];
            snprintf(arg, sizeof arg, "%.*s", e ? (int)(e - o - 8) < 299 ? (int)(e - o - 8) : 299 : 0, o + 8);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (!from_person || !e)
                continue;
            char verb[16] = "", what[80] = "";
            sscanf(arg, "%15s %79s", verb, what);
            char note[300];
            if (!strcmp(verb, "hear"))
                snprintf(note, sizeof note, "(voice: %s %s)", what, voice_preview(what) ? "played" : "isn't one of the voices");
            else if (!strcmp(verb, "use"))
                snprintf(note, sizeof note, "(voice: %s)", voice_use(what));
            else if (!strcmp(verb, "key") && (!strcmp(what, "openai") || !strcmp(what, "elevenlabs"))) {
                snprintf(voice_key_wanted, sizeof voice_key_wanted, "%s", what); /* asked next frame */
                snprintf(note, sizeof note, "(voice: they are being asked for the key, privately)");
            } else if (!strcmp(verb, "speed")) {
                voice_set_speed(strtof(what, NULL));
                snprintf(note, sizeof note, "(voice: speed now %s)", what);
            } else if (!strcmp(verb, "mix"))
                snprintf(note, sizeof note, "(voice: %s)", voice_mix(arg + 3));
            else if (!strcmp(verb, "shape"))
                snprintf(note, sizeof note, "(voice: %s)", voice_shape(arg + 5));
            else if (!strcmp(verb, "clone"))
                snprintf(note, sizeof note, "(voice: %s)", voice_clone(arg + 5));
            else
                snprintf(note, sizeof note, "(voice: [[voice:%.100s]] isn't a thing; hear, use, speed, mix, shape or key)", arg);
            convo_add(c, "hidden", note);
        }
        persona_take(m->text); /* [[trait:]]: what it read in them, private */
        presence_tags(m->text, from_person); /* its body: [[act:]] [[mood:]] [[figure:]] */
        while ((o = strstr(m->text, "[[putback"))) { /* a change put back, on their yes (changes.c): never locked */
            char *e = strstr(o, "]]");
            int k = e && o[9] == ':' ? atoi(o + 10) : 1;
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (!from_person || !e)
                continue;
            char said[200], note[320];
            if (changes_put_back(k < 1 ? 1 : k, said, sizeof said)) {
                snprintf(note, sizeof note, "(Put back: %s. It is done.)", said);
                toast("Put back: %s.", said);
            } else {
                snprintf(note, sizeof note, "(The [[putback]] FAILED: there is no change %d to put back.)", k);
            }
            convo_add(c, "hidden", note);
        }
        while ((o = strstr(m->text, "[[theme:"))) { /* one of the starter designs, as it comes */
            char *e = strstr(o, "]]");
            char nm[64];
            snprintf(nm, sizeof nm, "%.*s", e ? (int)(e - o - 8) : 0, o + 8);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (from_person && e && !apply_starter(nm))
                convo_add(c, "hidden", "(The [[theme:]] tag FAILED: there is no starter design by that name.)");
        }
        while ((o = strstr(m->text, "[[design:"))) { /* save, apply, list, delete their designs */
            char *e = strstr(o, "]]");
            char arg[120];
            snprintf(arg, sizeof arg, "%.*s", e ? (int)(e - o - 9) < 119 ? (int)(e - o - 9) : 119 : 0, o + 9);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (!from_person || !e)
                continue;
            char verb[16] = "", note[1400];
            int off = 0;
            sscanf(arg, "%15s %n", verb, &off);
            const char *what = arg + off;
            if (!strcasecmp(verb, "save"))
                snprintf(note, sizeof note, "(design: %s)", desktop_design_save(what, false) ? "saved; [[design:apply NAME]] brings it back" : "FAILED: give it a name");
            else if (!strcasecmp(verb, "apply") || !strcasecmp(verb, "load"))
                snprintf(note, sizeof note, "(design: %s)", apply_starter(what) ? "applied (what was there is kept as a backup)" : "FAILED: there is no saved or starter design by that name; [[design:list]]");
            else if (!strcasecmp(verb, "delete")) {
                char slug[64], path[200];
                design_slug(what, slug, sizeof slug);
                snprintf(path, sizeof path, DESIGNS "/%s.xml", slug);
                snprintf(note, sizeof note, "(design: %s)", slug[0] && unlink(path) == 0 ? "deleted" : "FAILED: no saved design by that name");
            } else {
                char list[1200];
                desktop_design_list(list, sizeof list);
                snprintf(note, sizeof note, "(design: %s)", list);
            }
            convo_add(c, "hidden", note);
        }
        while ((o = strstr(m->text, "[[name:"))) { /* they gave it a new name */
            char *e = strstr(o, "]]");
            char nm[64];
            snprintf(nm, sizeof nm, "%.*s", e ? (int)(e - o - 7) : 0, o + 7);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (from_person && e)
                companion_rename(nm);
        }
        while ((o = strstr(m->text, "[[user:"))) { /* what they want to be called */
            char *e = strstr(o, "]]");
            char nm[64];
            snprintf(nm, sizeof nm, "%.*s", e ? (int)(e - o - 7) : 0, o + 7);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (from_person && e)
                companion_rename_user(nm);
        }
        while ((o = strstr(m->text, "[[capture"))) { /* a screenshot or recording, asked for in words */
            char *e = strstr(o, "]]");
            char act[48];
            snprintf(act, sizeof act, "%.*s", e ? (int)(e - o - 2) : 0, o + 2);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (from_person)
                fm_host_action(A.ui, NULL, act);
        }
        while ((o = strstr(m->text, "[[open:"))) {
            char *e = strstr(o, "]]");
            char id[48];
            snprintf(id, sizeof id, "%.*s", e ? (int)(e - o - 7) : 0, o + 7);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (from_person) {
                char act[64];
                snprintf(act, sizeof act, "open:%s", id);
                fm_host_action(A.ui, NULL, act);
                opened++;
            }
        }
        /* Any tag left over (one we don't act on here) is never shown. */
        for (char *t0; (t0 = strstr(m->text, "[["));) {
            char *t1 = strstr(t0, "]]");
            if (!t1) {
                *t0 = 0;
                break;
            }
            memmove(t0, t1 + 2, strlen(t1 + 2) + 1);
        }
        /* Removed tags leave blank lines behind: trim them. */
        size_t lead = strspn(m->text, " \t\r\n");
        memmove(m->text, m->text + lead, strlen(m->text + lead) + 1);
        for (size_t l = strlen(m->text); l && strchr(" \t\r\n", m->text[l - 1]); l--)
            m->text[l - 1] = 0;
        /* and the gaps they leave inside: one blank line at most */
        for (char *g; (g = strstr(m->text, "\n\n\n"));)
            memmove(g + 2, g + 3, strlen(g + 3) + 1);
        /* A reply of tags alone would leave an empty bubble: say what happened. */
        bool said = false;
        for (const char *p = m->text; *p && !said; p++)
            said = *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r';
        if (!said && from_person && (tag || ui_ran || opened)) {
            free(m->text);
            if (preview_showing())
                m->text = strdup(tag ? "On it. Watch it come together in the preview." : "Have a look in the preview.");
            else
                m->text = strdup(tag ? "Redesigning the desktop now." : "Done. Have a look.");
        }
        voice_say(m->text); /* heard as well as read */
        /* With its window closed, its words appear as a caption instead
         * (in the preview's own window when that is open). */
        bool in_preview = preview_showing() && (A.screen != SCR_DESKTOP || (A.win[WIN_PREVIEW].open &&
                                                                            !A.win[WIN_PREVIEW].minimized));
        if (!in_preview && !chat_on_desktop())
            caption_say(m->text, 0);
        if (!in_preview && !chat_on_desktop() &&
            (!A.win[WIN_COMPANION].open || A.win[WIN_COMPANION].minimized)) /* kept to read later */
            notify(D.ai_name[0] ? D.ai_name : "Your companion", m->text, "companion");
        toast_listening = false;
        static bool told_screen; /* once for each thing they asked: never a loop of it */
        if (person_said)
            told_screen = false;
        bool tell_now = false;
        if (toast_heard[0]) {
            size_t tl = strlen(tag_report);
            snprintf(tag_report + tl, sizeof tag_report - tl, "%s", toast_heard);
            LOG("companion: told what the screen said: %s", toast_heard);
            tell_now = from_person && !told_screen && !tag_failed;
            told_screen |= tell_now;
        }
        static bool told_untagged; /* once for each thing they said: never a loop of it */
        if (person_said)
            told_untagged = false;
        if (from_person && !had_tags && !told_untagged && !A.design_job && promises_action(m->text)) {
            told_untagged = true;
            char note[700];
            snprintf(note, sizeof note,
                     "(Your last reply said you were doing something (\"%.200s\") but it had no tag, so nothing "
                     "happened. Do it now, with the tags it takes, in this reply; or, if you won't or can't, say so "
                     "plainly. Never say you are doing something without the tag that does it.)", m->text);
            LOG("companion: said it was doing something with no tag: told\n");
            companion_note(note, true);
        }
        if ((ui_ran || tag_failed || toast_heard[0]) && tag_report[0]) {
            /* What its tags did, for it to read from now on: without this it
             * took its own tag-less words for changes it never made, and
             * "sent" the same change again and again. */
            char note[3200];
            snprintf(note, sizeof note, "(What the tags in your last reply did, one line each:\n%s)", tag_report);
            convo_add(c, "hidden", note);
            if (tell_now) { /* what it did showed them that: it answers it now, in words */
                convo_add(c, "hidden",
                          "(The screen showed them that after what you just did. If it means it didn't work, say so "
                          "plainly in a sentence and what you'll do instead; don't try the same thing again.)");
                convo_send(c, companion_system(), 800);
            }
            if (tag_failed && !repair && !broke) {
                convo_add(c, "hidden",
                          "(Some of those FAILED, so that part did not happen. Fix it now: ui tags only, any markup "
                          "written out whole and closed, no words.)");
                convo_send(c, companion_system(), 1600);
                repairing = true;
            }
        }
        /* A model that sees is shown what its change looks like (twice at most per request) */
        bool show = ui_ran && preview_doc() && has_look_tags_ran && ai_sees_images(&A.cfg) && look_rounds < 2 &&
                    !(tag_failed && !repair && !broke);
        if (broke) {
            char note[2600];
            snprintf(note, sizeof note,
                     "(Measured on the screen after your change:\n%.2200sFix these now with ui tags; answer with "
                     "the tags alone, no words.%s)",
                     broke, show ? " The pictures show it as it is now." : "");
            convo_add(c, "hidden", note);
            if (show) {
                char *a, *b, *z;
                stylist_pictures_focus(preview_doc(), last_look_target, &a, &b, &z);
                convo_picture3(c, a, b, z);
                look_rounds++;
            }
            convo_send(c, companion_system(), 1600);
            repairing = true;
            free(broke);
        } else if (show) {
            char *a, *b, *z;
            stylist_pictures_focus(preview_doc(), last_look_target, &a, &b, &z);
            if (a || b) {
                char note[700];
                snprintf(note, sizeof note,
                         "(Here is how it looks now, after your change: the desktop, its menu open over a window%s. "
                         "Look closely: colours, spacing, edges and bevels pixel by pixel, the type, text that is "
                         "hard to read, things that spill out or overlap; compare it with what it should look like. "
                         "If something is off, fix it with ui tags alone, no words. If it is right, answer [[ok]] "
                         "and nothing else.)",
                         z ? ", and a close-up of what you just changed at its real pixels, enlarged without "
                             "smoothing" : "");
                convo_add(c, "hidden", note);
                convo_picture3(c, a, b, z);
                look_rounds++;
                convo_send(c, companion_system(), 1600);
                repairing = true;
            }
        }
    }
}

/* Their chats, newest first (chats.c), in place of the one open: a click opens one again; Delete, then a second
 * click on it, deletes one. */
static void chats_list_body(rect r, font *f, font *small, bool can, bool *listing) {
    static struct chat_entry list[200];
    static int n, scroll;
    static uint64_t listed_at, armed_at;
    static char armed[40];
    static bool armed_open;
    if (!listed_at || A.now - listed_at > 1000) {
        n = chats_list(list, 200);
        listed_at = A.now;
    }
    int lf = font_line_height(f), ls = font_line_height(small), row_h = lf + ls + 24;
    bool in_list = pt_in(r, A.in.mx, A.in.my);
    if (in_list && A.in.wheel) {
        scroll -= A.in.wheel * row_h / 2;
        int most = n * row_h - r.h + 40;
        scroll = scroll > most ? most : scroll;
        scroll = scroll < 0 ? 0 : scroll;
        A.in.wheel = 0;
    }
    canvas_clip(A.frame, r);
    int y = r.y - scroll;
    if (!can) {
        char wait[160];
        snprintf(wait, sizeof wait, "Another chat opens once %s has finished.", D.ai_name[0] ? D.ai_name : "it");
        draw_text(A.frame, small, r.x + 4, y + 4, wait, D.muted);
        y += ls + 16;
    }
    if (!n)
        draw_text(A.frame, f, r.x + 4, y + 8, "No chats yet.", D.muted);
    color chip = color_alpha(D.surface_text, 0.08f);
    for (int i = 0; i < n; i++, y += row_h) {
        if (y + row_h < r.y || y > r.y + r.h)
            continue;
        struct chat_entry *e = &list[i];
        rect row = {r.x, y, r.w, row_h - 6};
        bool hov = in_list && ui_hover(row);
        if (e->open || hov)
            fill_round(A.frame, row, D.radius * 0.6f + 4,
                       e->open ? color_alpha(D.accent, hov ? 0.26f : 0.18f) : color_alpha(D.surface_text, 0.07f));
        rect db = {row.x + row.w - 92, row.y + (row.h - 28) / 2, 82, 28};
        bool is_armed = armed_at && A.now - armed_at < 4000 && !strcmp(armed, e->id) && armed_open == e->open;
        bool deletable = !e->open || can;
        int room = row.w - 28 - (hov || is_armed ? 100 : 0);
        char title[96];
        snprintf(title, sizeof title, "%s", e->title);
        int len = (int)strlen(title);
        if (text_width(f, title) > room) { /* cut to fit, at a whole character */
            while (len > 0 && text_width_n(f, title, len) + text_width(f, "\xE2\x80\xA6") > room)
                len--;
            while (len > 0 && (title[len] & 0xC0) == 0x80)
                len--;
            snprintf(title + len, sizeof title - (size_t)len, "\xE2\x80\xA6");
        }
        color ink = theme_ink(row, D.surface_text, 4.5f);
        draw_text(A.frame, f, row.x + 14, row.y + 9, title, ink);
        char when[64], sub[96];
        chats_when(e->when, when, sizeof when);
        snprintf(sub, sizeof sub, "%s", e->open ? "Open now" : when);
        draw_text(A.frame, small, row.x + 14, row.y + 11 + lf, sub, color_alpha(ink, 0.65f));
        bool took = false;
        if ((hov || is_armed) && deletable) {
            bool pressed = ui_button(db, is_armed ? "Delete?" : "Delete", small,
                                     is_armed ? RGBA(200, 70, 60, 255) : chip,
                                     is_armed ? RGBA(255, 255, 255, 255) : ink, 14, true);
            if (pressed) {
                took = true;
                if (is_armed) {
                    chats_delete(e->open ? "" : e->id);
                    armed_at = 0;
                    listed_at = 0; /* (read again next frame) */
                } else {
                    snprintf(armed, sizeof armed, "%s", e->id);
                    armed_open = e->open;
                    armed_at = A.now;
                }
            }
        }
        if (!took && hov && A.in.clicked && !pt_in(db, A.in.mx, A.in.my)) {
            if (e->open) {
                *listing = false; /* (it is the one open: back to it) */
            } else if (can && chats_open(e->id)) {
                *listing = false;
                listed_at = 0;
            }
        }
    }
    canvas_unclip(A.frame);
}

static void companion_body(rect r) {
    struct conversation *c = &A.companion;
    /* Conversation is read, not admired: display faces give way to a reading one. */
    static const char *const display[] = {"Fraunces", "Syne", "Unbounded", "Caveat", "PlayfairDisplay", NULL};
    const char *face = D.font_body;
    for (int i = 0; display[i]; i++)
        if (!strcasecmp(face, display[i]))
            face = "Inter";
    font *f = font_get(face, D.body_px), *small = font_get(face, D.body_px - 3);
    static int saved_n = -1;
    if (c->n != saved_n && !c->pending) {
        convo_save(c, STATE_DIR "/companion.txt"); /* kept, so tomorrow can start from today */
        chats_saved();                             /* (and in their list of chats, once it is in it) */
        saved_n = c->n;
    }
    /* At the top, as in every AI app: their chats, and a new one (the one open is kept) */
    static bool listing; /* the list of their chats, in place of the one open */
    bool can = chats_can_switch();
    rect bar = {r.x, r.y, r.w, 34};
    color chip = color_alpha(D.surface_text, 0.08f);
    rect lb = {bar.x, bar.y, 92, bar.h}, nb = {bar.x + bar.w - 128, bar.y, 128, bar.h};
    if (ui_button(lb, listing ? "Back" : "Chats", small, chip, D.surface_text, bar.h / 2.0f, true)) {
        listing = !listing;
        LOG("chats: the list %s\n", listing ? "shown" : "put away");
    }
    if (ui_button(nb, "+  New chat", small, chip, D.surface_text, bar.h / 2.0f, can)) {
        chats_new();
        listing = false;
        saved_n = -1;
    }
    r.y += bar.h + 10;
    r.h -= bar.h + 10;
    if (listing) {
        chats_list_body(r, f, small, can, &listing);
        static uint64_t list_log;
        if (A.now - list_log > 2000) {
            list_log = A.now;
            LOGRECT("chats-list", lb);
            LOGRECT("chats-new", nb);
        }
        return;
    }
    rect input = {r.x, r.y + r.h - 52, r.w, 50};
    rect area = {r.x, r.y, r.w, r.h - 64};
    /* Lay out bubbles bottom-up so the newest stays visible. */
    canvas_clip(A.frame, area);
    int y = area.y + area.h, maxw = (int)(r.w * 0.82f);
    if (c->pending || agent_busy()) {
        y -= 40;
        int dots = 1 + (int)((A.now / 350) % 3);
        char d[80] = "";
        if (agent_busy() && agent_doing()[0]) /* its body at work: what it is doing */
            snprintf(d, sizeof d, "%s ", agent_doing());
        for (int i = 0; i < dots; i++)
            strcat(d, ".");
        int w = text_width(f, d) + 36;
        fill_round(A.frame, (rect){area.x, y, w, 34}, 17, color_alpha(D.accent2, 0.25f));
        draw_text(A.frame, f, area.x + 18, y + 4, d, theme_ink((rect){area.x, y, w, 34}, D.surface_text, 4.5f));
    }
    if (c->error[0]) {
        int h = draw_text_wrapped(NULL, small, (rect){0, 0, maxw, 400}, c->error, 0, 1.25f);
        y -= h + 16;
        draw_text_wrapped(A.frame, small, (rect){area.x, y, maxw, h}, c->error,
                          theme_fit(RGBA(214, 72, 56, 255), average_under((rect){area.x, y, maxw, h}), 4.5f), 1.25f);
    }
    for (int i = c->n - 1; i >= 0 && y > area.y; i--) {
        struct msg *m = &c->msgs[i];
        if (!strcmp(m->role, "hidden") || !m->text[strspn(m->text, " \t\r\n")])
            continue;
        bool mine = !strcmp(m->role, "user");
        int h = draw_text_wrapped(NULL, f, (rect){0, 0, maxw - 28, 2000}, m->text, 0, 1.3f);
        int tw = 0;
        if (h <= font_line_height(f) * 1.3f + 1)
            tw = text_width(f, m->text) + 28;
        int bw = tw && tw < maxw ? tw : maxw;
        y -= h + 22 + 10;
        rect b = {mine ? area.x + area.w - bw : area.x, y, bw, h + 20};
        if (A.in.right_pressed && pt_in(b, A.in.mx, A.in.my) && pt_in(area, A.in.mx, A.in.my)) {
            /* a message: copy it, search for it, ask about it, and what is offered everywhere */
            ctx_menu_open(A.in.mx, A.in.my);
            ctx_menu_target(b.x, b.y, b.w, b.h);
            ctx_menu_for("text");
            menu_ctx mc;
            memset(&mc, 0, sizeof mc);
            snprintf(mc.text, sizeof mc.text, "%s", m->text);
            menu_context(&mc);
            char act[32];
            snprintf(act, sizeof act, "copymsg:%d", i);
            ctx_menu_action("copy-message", "Copy message", act, 0, NULL, "copy", "The whole message, for pasting");
        }
        fill_round(A.frame, b, D.radius * 0.8f + 4, mine ? color_alpha(D.accent, 0.9f) : color_alpha(D.surface_text, 0.08f));
        /* Words must read against what is really behind them (the frame's
         * own colours may not be the design's surface colour). */
        color under = average_under(b), want = mine ? readable_on(D.accent) : D.surface_text;
        if (color_contrast(want, under) < 4.5f)
            want = readable_on(under);
        draw_text_wrapped(A.frame, f, (rect){b.x + 14, b.y + 10, b.w - 28, h}, m->text, want, 1.3f);
    }
    canvas_unclip(A.frame);
    char ph[96];
    snprintf(ph, sizeof ph, "Talk to %s...", D.ai_name);
    ui_textfield(input, &A.chat, !c->pending, ph, f, color_alpha(D.surface_text, 0.08f), D.surface_text, D.muted, D.radius * 0.8f + 6);
    static uint64_t last_log;
    if (A.now - last_log > 2000) {
        last_log = A.now;
        LOGRECT("companion-input", input);
        LOGRECT("chats-list", lb);
        LOGRECT("chats-new", nb);
    }
    if (!c->pending && ui_type(&A.chat) && A.chat.len) {
        convo_add(c, "user", A.chat.buf);
        A.chat.len = 0;
        A.chat.buf[0] = 0;
        convo_send(c, companion_system(), 1600);
    }
}


/* <view app="chat">: the conversation as part of the desktop, codec style:
 * the last few lines, each "NAME: words" in the design's own type, newest at
 * the bottom, and a line to type on under them. Replies land here instead
 * of a caption or a window. */
static uint64_t chat_view_at;

static void chat_strip(rect r) {
    struct conversation *c = &A.companion;
    chat_view_at = A.now;
    float px = D.body_px + 3;
    font *f = font_get(D.font_body, px), *fb = font_get(D.font_heading, px);
    int lh = (int)(font_line_height(f) * 1.35f);
    int ih = lh + 18 > 46 ? lh + 18 : 46; /* tall enough for its own words */
    rect input = {r.x, r.y + r.h - ih, r.w, ih};
    rect area = {r.x, r.y, r.w, r.h - ih - 14};
    color ink = D.text, who_ai = D.accent, who_me = color_mix(D.text, D.accent2, 0.35f);
    canvas_clip(A.frame, area);
    int y = area.y + area.h;
    char you[64];
    snprintf(you, sizeof you, "%s", D.user_name[0] ? D.user_name : "You");
    if (c->pending) {
        int dots = 1 + (int)((A.now / 350) % 3);
        char d[64];
        snprintf(d, sizeof d, "%s: %.*s", D.ai_name, dots, "...");
        y -= lh;
        draw_text(A.frame, fb, area.x, y, d, who_ai);
    }
    for (int i = c->n - 1; i >= 0 && y > area.y; i--) {
        struct msg *m = &c->msgs[i];
        if (!strcmp(m->role, "hidden") || !m->text[strspn(m->text, " \t\r\n")])
            continue;
        bool mine = !strcmp(m->role, "user");
        char name[80];
        snprintf(name, sizeof name, "%s: ", mine ? you : D.ai_name);
        int nw = text_width(fb, name);
        int h = draw_text_wrapped(NULL, f, (rect){0, 0, area.w - nw, 4000}, m->text, 0, 1.35f);
        y -= h + lh / 3;
        draw_text(A.frame, fb, area.x, y, name, mine ? who_me : who_ai);
        draw_text_wrapped(A.frame, f, (rect){area.x + nw, y, area.w - nw, h}, m->text, ink, 1.35f);
    }
    canvas_unclip(A.frame);
    char ph[96];
    snprintf(ph, sizeof ph, "Talk to %s...", D.ai_name);
    ui_textfield(input, &A.chat, !c->pending, ph, f, color_alpha(D.text, 0.06f), D.text, D.muted, 4);
    static uint64_t last_log;
    if (A.now - last_log > 2000) {
        last_log = A.now;
        LOGRECT("chat-input", input);
    }
    if (!c->pending && ui_type(&A.chat) && A.chat.len) {
        convo_add(c, "user", A.chat.buf);
        A.chat.len = 0;
        A.chat.buf[0] = 0;
        convo_send(c, companion_system(), 1600);
    }
}

/* the conversation is on the desktop itself: no captions or pop-ups for it */
bool chat_on_desktop(void) { return A.now - chat_view_at < 1500; }

/* Opens a text file from Files in Notes (notes.c). */
void open_in_notes(const char *path) {
    if (notes_open(path))
        open_window(WIN_NOTES);
}

/* Removes everything the OS keeps about them and starts again from first boot. */
void forget_everything(void) {
    static const char *const files[] = {"/design.json", "/facet.xml", "/portrait.txt", "/design-reply.txt", "/mind.json",
                                        NULL};
    for (int i = 0; files[i]; i++) {
        char p[128];
        snprintf(p, sizeof p, STATE_DIR "%s", files[i]);
        unlink(p);
    }
    notes_forget();
    theme_follow_design();
    unlink(STATE_DIR "/companion.txt");
    chats_delete_all(); /* (and every chat before it) */
    onboard_forget();
    if (priv_available()) /* and from the disk */
        priv_send("wipe");
    else
        store_wipe();
    convo_clear(&A.companion);
    memset(&A.prof, 0, sizeof A.prof);
    A.have_design = false;
    memset(A.win, 0, sizeof A.win);
    term_close();
    desktop_set_ui(NULL);
    A.provider = NULL;
    set_screen(SCR_CHOOSE);
}

/* Back to how it came: everything kept on the disk is erased (their name,
 * the conversation, the memory, the design, its look, the home folder, apps
 * from Flathub and their data) and the computer starts again at the very
 * first screen. Nothing is saved on the way out. */
void factory_reset(void) {
    LOG("reset: back to factory\n");
    alive_shutdown();
    if (priv_available()) { /* root's work, asked of init */
        priv_call("factory-reset", 20000);
        priv_call("wipe", 60000);
        priv_call("restart", 5000);
        return;
    }
    store_mark_factory_reset();
    store_wipe();
    sync();
    reboot(RB_AUTOBOOT);
}

/* Settings and its pages live in settings.c. */
static void settings_body(rect r) { settings_draw(r); }

/* About: the system and the computer it runs on (Settings > About, on its own). The design's notes are in
 * Settings > Appearance. */
static void about_body(rect r) { settings_about_draw(r); }

static float ease_out(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

/* ---- windows: frames come from the design's <template id="window"> ---- */

static void draw_window(int w, bool focused) {
    struct window *win = &A.win[w];
    rect r = win->r;
    /* Arrive with a short glide upward, scaled by how much motion the
     * design wants. */
    float e = A.prof.reduce_motion ? 1 : ease_out((float)(A.now - win->opened_at) / 320.0f); /* (less motion: there) */
    r.y += (int)((1 - e) * 36 * (0.3f + D.motion));
    rect body;
    win->body_opaque = false;
    if (win->view && win->view->client_frame) {
        /* the app draws its own title bar and frame: only a shadow from us */
        shadow_round(A.frame, r, 8, 28, RGBA(0, 0, 0, focused ? 110 : 70));
        body = r;
    } else {
        /* an app's body is opaque (app_body paints the surface colour, then the app; or the app's picture is
         * opaque all over and fills it): its frame's glass is blurred only around it */
        wl_view *av = win->view;
        bool app = w >= WIN_COUNT && !win->fx && av;
        /* (opaque: the whole body when the surface colour is; else as far as the app's opaque picture reaches) */
        int ow = !app ? -1 : C_A(D.surface) == 255 ? 0 : av->opaque ? av->w : -1, oh = ow > 0 ? av->h : 0;
        /* (apps' frames and the system's own windows' apart, in the log's "time:" line; and once, whether the
         * glass under an app is left out) */
        PROF(app ? "app frames" : "window frames", fm_render_window_frame(A.ui, A.frame, w, r, focused, &body, ow, oh));
        win->body_opaque = app && (ow == 0 || (ow > 0 && av->w >= body.w && av->h >= body.h));
        if (app && win->glass_said != ow + 2) {
            win->glass_said = ow + 2;
            LOG("glass: window %d: %s (surface alpha %d, its picture %s)\n", w,
                ow < 0 ? "blurred whole" : "blurred only around the app", C_A(D.surface), av->opaque ? "opaque" : "not opaque");
        }
    }
    canvas_clip(A.frame, body);
    switch (w) {
    case WIN_FILES: PROF("Files", files_body(body)); break;
    case WIN_COMPANION: PROF("companion window", companion_body(body)); break;
    case WIN_NOTES: notes_draw(body); break;
    case WIN_SETTINGS: settings_body(body); break;
    case WIN_ABOUT: about_body(body); break;
    case WIN_TERMINAL: term_draw(body, focused, D.surface, D.surface_text, D.body_px - 2 < 13 ? 13 : D.body_px - 2); break;
    case WIN_PREVIEW: preview_body(body, focused); break;
    default:
        if (win->fx) {
            fill_rect(A.frame, body, D.surface);
            win->body = body;
            fxwin_draw(win->fx, body, focused, w == pointer_top);
        } else {
            app_body(w, body);
        }
        break;
    }
    canvas_unclip(A.frame);
}

static int match_app(const char *q) {
    if (!*q)
        return -1;
    for (int a = 0; a < APP_COUNT; a++)
        if (!strncasecmp(D.apps[a].label, q, strlen(q)) || !strncasecmp(app_default_name(a), q, strlen(q)))
            return a;
    return -1;
}

/* ---- bundled apps (/apps/<id>/app.conf) ---- */

struct bundled {
    char id[96], name[48], exec[300], about[128];
    char mime[600]; /* what it opens (its .desktop's MimeType): "Open with", links */
};
#define MAX_BUNDLED 48
static struct bundled bundled[MAX_BUNDLED];
static int nbundled = -1;

/* Apps installed with Flatpak: their exported .desktop entries. */
static void scan_flatpak_apps_in(const char *apps) {
    DIR *dir = opendir(apps);
    struct dirent *de;
    while (dir && (de = readdir(dir)) && nbundled < MAX_BUNDLED) {
        size_t len = strlen(de->d_name);
        if (len < 9 || strcmp(de->d_name + len - 8, ".desktop"))
            continue;
        char path[400], line[512];
        snprintf(path, sizeof path, "%s/%s", apps, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        struct bundled *b = &bundled[nbundled];
        memset(b, 0, sizeof *b);
        snprintf(b->id, sizeof b->id, "%.*s", (int)(len - 8), de->d_name);
        bool main_group = false, hidden = false;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '[') {
                main_group = !strcmp(line, "[Desktop Entry]");
                continue;
            }
            char *eq = strchr(line, '=');
            if (!main_group || !eq)
                continue;
            *eq = 0;
            const char *v = eq + 1;
            if (!strcmp(line, "Name")) snprintf(b->name, sizeof b->name, "%s", v);
            else if (!strcmp(line, "Exec")) snprintf(b->exec, sizeof b->exec, "%s", v);
            else if (!strcmp(line, "Comment")) snprintf(b->about, sizeof b->about, "%s", v);
            else if (!strcmp(line, "MimeType")) snprintf(b->mime, sizeof b->mime, ";%s", v);
            else if (!strcmp(line, "NoDisplay") || !strcmp(line, "Hidden")) hidden |= !strcasecmp(v, "true");
        }
        fclose(f);
        /* no files to hand over: drop the field codes and Flatpak's @@ markers around them */
        for (char *p; (p = strstr(b->exec, " @@u")) || (p = strstr(b->exec, " @@")) || (p = strchr(b->exec, '%'));) {
            size_t skip = p[0] == '%' ? (p[1] ? 2 : 1) : p[3] == 'u' ? 4 : 3;
            memmove(p, p + skip, strlen(p + skip) + 1);
        }
        if (hidden || !b->exec[0])
            continue;
        if (!b->name[0])
            snprintf(b->name, sizeof b->name, "%s", b->id);
        LOG("flatpak app %s (%s)\n", b->id, b->name);
        nbundled++;
    }
    if (dir)
        closedir(dir);
}

static void scan_flatpak_apps(void) {
    scan_flatpak_apps_in(FLATPAK_DISK_EXPORTS "/applications");
    scan_flatpak_apps_in(FLATPAK_SYSTEM_EXPORTS "/applications");
}

void desktop_apps_changed(void) { nbundled = -1; }

/* Apps written in Facet 2: the system's (/usr/share/os/apps) and theirs (~/Apps, which win by name). Started
 * in a window of the shell's own (fxwin.c). Their name is the one after "app" on the file's first line. */
static void scan_facet_dir(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d)) && nbundled < MAX_BUNDLED) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] == '.' || l < 7 || strcmp(de->d_name + l - 6, ".facet"))
            continue;
        char id[96], path[400];
        snprintf(id, sizeof id, "%.*s", (int)(l - 6) < 95 ? (int)(l - 6) : 95, de->d_name);
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct bundled *b = NULL;
        for (int i = 0; i < nbundled && !b; i++)
            if (!strcasecmp(bundled[i].id, id) && !strncmp(bundled[i].exec, "facet:", 6))
                b = &bundled[i]; /* theirs over the system's */
        if (!b)
            b = &bundled[nbundled++];
        memset(b, 0, sizeof *b);
        snprintf(b->id, sizeof b->id, "%s", id);
        snprintf(b->exec, sizeof b->exec, "facet:%s", path);
        snprintf(b->name, sizeof b->name, "%s", id);
        FILE *f = fopen(path, "r");
        char line[200];
        while (f && fgets(line, sizeof line, f)) {
            char nm[48];
            if (sscanf(line, " app %47s", nm) == 1) {
                snprintf(b->name, sizeof b->name, "%s", nm);
                break;
            }
        }
        if (f)
            fclose(f);
        snprintf(b->about, sizeof b->about, "An app written in Facet");
    }
    if (d)
        closedir(d);
}

static void scan_facet_apps(void) {
    scan_facet_dir("/usr/share/os/apps");
    scan_facet_dir(home_path("/Apps"));
}

static void scan_facet_apps(void);
static void scan_bundled(void) {
    if (nbundled >= 0)
        return;
    nbundled = 0;
    DIR *dir = opendir("/apps");
    struct dirent *de;
    while (dir && (de = readdir(dir)) && nbundled < MAX_BUNDLED) {
        if (de->d_name[0] == '.')
            continue;
        char path[300], line[300];
        snprintf(path, sizeof path, "/apps/%s/app.conf", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        struct bundled *b = &bundled[nbundled];
        memset(b, 0, sizeof *b);
        snprintf(b->id, sizeof b->id, "%s", de->d_name);
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq = 0;
            if (!strcmp(line, "id")) snprintf(b->id, sizeof b->id, "%s", eq + 1);
            else if (!strcmp(line, "name")) snprintf(b->name, sizeof b->name, "%s", eq + 1);
            else if (!strcmp(line, "exec")) snprintf(b->exec, sizeof b->exec, "%s", eq + 1);
            else if (!strcmp(line, "about")) snprintf(b->about, sizeof b->about, "%s", eq + 1);
        }
        fclose(f);
        if (!b->name[0])
            snprintf(b->name, sizeof b->name, "%s", b->id);
        LOG("bundled app %s (%s)\n", b->id, b->name);
        nbundled++;
    }
    if (dir)
        closedir(dir);
    scan_flatpak_apps();
    scan_facet_apps();
}

/* ---- Linux apps (Wayland) ---- */

static void launch_bundled(struct bundled *b) {
    if (!strncmp(b->exec, "facet:", 6)) {
        fxwin_open(b->exec + 6);
        return;
    }
    if (!b->exec[0]) {
        toast("%s is installed, but the system doesn't know how to start it.", b->name);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        for (int i = 3; i < 256; i++)
            close(i);
        become_session_user();
        execl("/bin/sh", "sh", "-c", b->exec, (char *)NULL);
        _exit(127);
    }
    LOG("launched %s (pid %d): %s\n", b->id, pid, b->exec);
    toast("Opening %s...", b->name);
}

void wl_host_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("shell: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void wl_host_view_mapped(wl_view *v) {
    if (v->popup)
        return; /* menus are drawn over their parent */
    int w = -1;
    for (int i = WIN_COUNT; i < WIN_MAX; i++)
        if (!A.win[i].open && !A.win[i].view) {
            w = i;
            break;
        }
    if (w < 0)
        return;
    struct window *win = &A.win[w];
    memset(win, 0, sizeof *win);
    win->view = v;
    v->host = (void *)(intptr_t)(w + 1);
    rect wk = A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H};
    int ww = v->client_frame ? v->w : v->w + 24, wh = v->client_frame ? v->h : v->h + 64; /* room for our frame, if any */
    if (ww > wk.w - 40) ww = wk.w - 40;
    if (wh > wk.h - 40) wh = wk.h - 40;
    int off = (w - WIN_COUNT) * 30;
    win->r = (rect){wk.x + (wk.w - ww) / 2 + off, wk.y + (wk.h - wh) / 2 + off / 2, ww, wh};
    win->open = true;
    win->opened_at = A.now;
    win->z = ++A.ztop;
    wm_opened(w);
    LOG("app window %d: %s (%s) %dx%d\n", w, v->title, v->app_id, v->w, v->h);
}

void wl_host_view_unmapped(wl_view *v) {
    int w = (int)(intptr_t)v->host - 1;
    if (w >= WIN_COUNT && w < WIN_MAX && A.win[w].view == v) {
        wm_unmapped(w);
        A.win[w].open = false;
        A.win[w].view = NULL;
        if (A.drag_win == w)
            A.drag_win = -1;
    }
    v->host = NULL;
}

/* Where a menu or tooltip of the window showing `top` sits on screen: its
 * offset from its parent, which is `top` itself or another menu (submenus).
 * False if it doesn't belong to `top`. */
static bool popup_rect(wl_view *p, wl_view *top, rect body, rect *out) {
    if (!p->popup || !p->mapped || !p->px)
        return false;
    int x = 0, y = 0;
    for (wl_view *q = p; q; q = q->parent) {
        if (!q->popup) {
            if (q != top)
                return false;
            *out = (rect){body.x + x, body.y + y, p->w, p->h};
            return true;
        }
        x += q->min_w, y += q->min_h;
    }
    return false;
}

static void app_body(int w, rect body) {
    struct window *win = &A.win[w];
    wl_view *v = win->view;
    win->body = body;
    /* opaque, whatever the app draws over it: its frame's glass relies on that (fm_render_window_frame) */
    fill_rect(A.frame, body, D.surface);
    if (!v || !v->px)
        return;
    /* The frame fits the app's first picture; afterwards the app follows
     * the frame (on maximize, it's asked to fill it). */
    if (!win->fitted) {
        /* ...but never past the free space: an app that asks for more than
         * the screen (an IDE wanting 1560x900) is told the size it has. */
        rect wk = A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H};
        int fw = win->r.w - body.w, fh = win->r.h - body.h;
        int bw = v->w, bh = v->h;
        if (bw + fw > wk.w - 24) bw = wk.w - 24 - fw;
        if (bh + fh > wk.h - 24) bh = wk.h - 24 - fh;
        win->r.w = bw + fw, win->r.h = bh + fh;
        if (win->r.x + win->r.w > wk.x + wk.w || win->r.y + win->r.h > wk.y + wk.h) {
            win->r.x = wk.x + (wk.w - win->r.w) / 2;
            win->r.y = wk.y + (wk.h - win->r.h) / 2;
        }
        win->fitted = true;
        win->asked_w = bw, win->asked_h = bh;
        if (bw != v->w || bh != v->h)
            wls_configure(v, bw, bh, w == focused_window());
    } else if ((body.w != win->asked_w || body.h != win->asked_h) && body.w > 50 && body.h > 50) {
        win->asked_w = body.w, win->asked_h = body.h;
        wls_configure(v, body.w, body.h, w == focused_window());
    }
    canvas src = {v->w, v->h, v->px, 0, 0, v->w, v->h};
    canvas_blit(A.frame, &src, body.x, body.y);
    canvas_unclip(A.frame);
    for (int i = 0; i < wls_view_count(); i++) { /* its menus and tooltips, submenus included */
        wl_view *p = wls_view(i);
        rect pr;
        if (popup_rect(p, v, body, &pr)) {
            canvas ps = {p->w, p->h, p->px, 0, 0, p->w, p->h};
            shadow_round(A.frame, (rect){pr.x, pr.y + 6, pr.w, pr.h}, 8, 24, RGBA(0, 0, 0, 90));
            canvas_blit(A.frame, &ps, pr.x, pr.y);
        }
    }
    canvas_clip(A.frame, body);
}

/* ---- an app's new picture, alone ----
 * Most frames while an app is busy (opening, loading a page, scrolling) change nothing but its own picture, and
 * drawing the whole desktop again for each (the wallpaper, every frame and shadow, the panel, the layout, all of
 * it hidden under the window) took 12 ms of the desktop's one core at full size: apps waited on it, and the
 * bigger the window the slower they went. So when a full frame shows the top window's body to be exactly the
 * app's picture (nothing of the design, no caption, no menu over it), the frames after it that bring only a new
 * picture from that app put just the picture on the screen (main.c). Anything else draws the whole frame. */
static struct {
    bool ok;
    int w;
    rect body;
    wl_view *v;
    uint64_t others; /* what the other windows showed */
} clean;

static uint64_t views_except(const wl_view *skip) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < wls_view_count(); i++) {
        wl_view *v = wls_view(i);
        if (v == skip)
            continue;
        uint64_t parts[4] = {(uint64_t)v->id, v->version, (uint64_t)v->mapped, (uint64_t)v->w << 32 | (uint32_t)v->h};
        for (int k = 0; k < 4; k++)
            h = (h ^ parts[k]) * 1099511628211ull;
    }
    return h;
}

static bool menus_over(const wl_view *v) {
    for (int i = 0; i < wls_view_count(); i++) {
        wl_view *p = wls_view(i);
        if (p->popup && p->mapped)
            for (wl_view *q = p->parent; q; q = q->parent)
                if (q == v)
                    return true;
    }
    return false;
}

void desktop_note_clean(void) {
    clean.ok = false;
    int w = A.screen == SCR_DESKTOP ? focused_window() : -1;
    if (w < WIN_COUNT)
        return;
    struct window *win = &A.win[w];
    wl_view *v = win->view;
    rect b = win->body;
    if (!v || !v->mapped || !v->px || v->w != b.w || v->h != b.h || (long)b.w * b.h * 8 < (long)A.W * A.H ||
        b.x < 0 || b.y < 0 || b.x + b.w > A.frame->w || b.y + b.h > A.frame->h || menus_over(v))
        return;
    for (int y = 0; y < b.h; y++)
        if (memcmp(A.frame->px + (size_t)(b.y + y) * (size_t)A.frame->w + b.x, v->px + (size_t)y * (size_t)v->w,
                   (size_t)b.w * 4))
            return; /* something is drawn over it */
    clean.ok = true, clean.w = w, clean.body = b, clean.v = v, clean.others = views_except(v);
}

bool desktop_app_in_front(void) { return focused_window() >= WIN_COUNT; }

bool desktop_app_frame_ready(void) {
    if (!clean.ok || A.screen != SCR_DESKTOP)
        return false;
    struct window *win = &A.win[clean.w];
    wl_view *v = win->view;
    return win->open && !win->minimized && v == clean.v && v->mapped && v->px && v->w == clean.body.w &&
           v->h == clean.body.h && focused_window() == clean.w && views_except(v) == clean.others && !menus_over(v);
}

bool desktop_app_frame_put(rect *out) {
    if (!desktop_app_frame_ready()) {
        clean.ok = false;
        return false;
    }
    rect b = clean.body;
    canvas_unclip(A.frame);
    for (int y = 0; y < b.h; y++)
        memcpy(A.frame->px + (size_t)(b.y + y) * (size_t)A.frame->w + b.x, clean.v->px + (size_t)y * (size_t)clean.v->w,
               (size_t)b.w * 4);
    *out = b;
    return true;
}

/* The pointer over an app's content and the keyboard in front go to it. */
static void route_to_apps(int top) {
    uint32_t ms = (uint32_t)A.now;
    wl_view *over = NULL;
    rect at = {0, 0, 0, 0};
    if (top >= WIN_COUNT && A.win[top].view && A.drag_win < 0) {
        /* a menu (the newest on top) before the window under it */
        for (int i = wls_view_count() - 1; i >= 0 && !over; i--) {
            rect pr;
            if (popup_rect(wls_view(i), A.win[top].view, A.win[top].body, &pr) && pt_in(pr, A.in.mx, A.in.my))
                over = wls_view(i), at = pr;
        }
        if (!over && pt_in(A.win[top].body, A.in.mx, A.in.my))
            over = A.win[top].view, at = A.win[top].body;
    }
    if (over) {
        wls_pointer_motion(over, (float)(A.in.mx - at.x), (float)(A.in.my - at.y), ms);
        if (A.in.pressed)
            wls_pointer_button(0x110, true, ms); /* BTN_LEFT */
        if (A.in.clicked)
            wls_pointer_button(0x110, false, ms);
        if (A.in.right_clicked) {
            wls_pointer_button(0x111, true, ms);
            wls_pointer_button(0x111, false, ms);
        }
        if (A.in.wheel)
            wls_pointer_axis((float)-A.in.wheel, ms);
    } else {
        wls_pointer_leave();
    }
    int fw = focused_window();
    wl_view *kv = fw >= WIN_COUNT ? A.win[fw].view : NULL;
    wls_keyboard_focus(kv);
    if (kv) {
        uint32_t mods = (A.in.shift ? 1u : 0u) | (A.in.ctrl ? 4u : 0u);
        for (int i = 0; i < A.in.nraw; i++)
            wls_key(A.in.raw[i].code, A.in.raw[i].down, mods, ms);
    }
}

/* ---- the Facet engine's view of the shell ---- */

int fm_host_app_count(void) {
    scan_bundled();
    return APP_COUNT + nbundled;
}

const char *fm_host_app_id(int a) {
    if (a >= 0 && a < APP_COUNT)
        return app_id(a);
    scan_bundled();
    return a >= APP_COUNT && a < APP_COUNT + nbundled ? bundled[a - APP_COUNT].id : "";
}

/* An app is called what it is, in every design: Files, Notes, Settings. A design may place it and style it,
 * never rename it (a desktop is for finding things; "Keepsakes" for Files, "Recycle Bin" for Notes made that
 * hard). The companion's name is its own. */
const char *fm_host_app_label(int a) {
    if (a == APP_COMPANION)
        return D.ai_name[0] ? D.ai_name : app_default_name(a);
    if (a >= 0 && a < APP_COUNT)
        return app_default_name(a);
    scan_bundled();
    if (a < APP_COUNT || a >= APP_COUNT + nbundled)
        return "";
    return bundled[a - APP_COUNT].name;
}

static const int win_app[WIN_COUNT] = {APP_FILES, APP_COMPANION, APP_NOTES, APP_SETTINGS, APP_ABOUT, APP_TERMINAL,
                                      APP_REDESIGN};

/* Open windows (including minimized ones) in a stable order. */
static int open_list(int *out) {
    int n = 0;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open)
            out[n++] = w;
    return n;
}

static int focused_window(void) {
    int best = -1;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized && (best < 0 || A.win[w].z > A.win[best].z))
            best = w;
    return best;
}

int fm_host_window_count(void) {
    int l[WIN_MAX];
    return open_list(l);
}

/* The app a window belongs to: built-in, a bundled app (by its Wayland app
 * id), or "some app" (one past the known ones). */
static int window_app(int w) {
    if (w < WIN_COUNT)
        return win_app[w];
    scan_bundled();
    wl_view *v = A.win[w].view;
    for (int b = 0; v && b < nbundled; b++)
        if (strcasestr(v->app_id, bundled[b].id) || strcasestr(bundled[b].exec, v->app_id[0] ? v->app_id : "\x01"))
            return APP_COUNT + b;
    return APP_COUNT + nbundled;
}

/* ---- for right-click menus (menu_actions.c): what opens what, and doing it ---- */

/* The installed apps that open `mime` (exactly, or "image/" for any picture), at most max. */
int desktop_apps_for(const char *mime, char (*ids)[96], char (*names)[48], int max) {
    scan_bundled();
    int n = 0;
    for (int b = 0; b < nbundled && n < max; b++) {
        if (!bundled[b].mime[0])
            continue;
        bool any = mime[strlen(mime) - 1] == '/';
        char pat[120];
        snprintf(pat, sizeof pat, ";%s%s", mime, any ? "" : ";");
        char hay[604];
        snprintf(hay, sizeof hay, "%s;", bundled[b].mime);
        if (strcasestr(hay, pat)) {
            snprintf(ids[n], 96, "%s", bundled[b].id);
            snprintf(names[n], 48, "%s", bundled[b].name);
            n++;
        }
    }
    return n;
}

/* Opens a file or a web address with that app (a Flatpak's own way of being handed one, where it has it). */
bool desktop_open_with(const char *app_id, const char *what) {
    scan_bundled();
    for (int b = 0; b < nbundled; b++) {
        if (strcmp(bundled[b].id, app_id) || !strncmp(bundled[b].exec, "facet:", 6))
            continue;
        char q[700], cmd[1100];
        size_t o = 0;
        q[o++] = '\'';
        for (const char *p = what; *p && o + 5 < sizeof q; p++) {
            if (*p == '\'') {
                memcpy(q + o, "'\\''", 4);
                o += 4;
            } else {
                q[o++] = *p;
            }
        }
        q[o++] = '\'';
        q[o] = 0;
        bool url = strstr(what, "://") != NULL;
        if (strstr(bundled[b].exec, "--file-forwarding"))
            snprintf(cmd, sizeof cmd, "%s %s %s @@", bundled[b].exec, url ? "@@u" : "@@", q);
        else
            snprintf(cmd, sizeof cmd, "%s %s", bundled[b].exec, q);
        spawn_command(cmd, bundled[b].name);
        return true;
    }
    return false;
}

/* A web address, in the browser they have (the first app that opens web links); else where to get one. */
void desktop_open_link(const char *url) {
    char ids[4][96], names[4][48];
    int n = desktop_apps_for("x-scheme-handler/https", ids, names, 4);
    if (!n)
        n = desktop_apps_for("text/html", ids, names, 4);
    if (n && desktop_open_with(ids[0], url))
        return;
    toast("Nothing here opens web links yet. Here are browsers you can get.");
    launcher_open("browser");
}

/* Every window of an app, closed. */
void desktop_close_app_windows(const char *app_id) {
    int k = fm_app_index(app_id);
    for (int w = 0; w < WIN_MAX && k >= 0; w++)
        if (A.win[w].open && window_app(w) == k)
            wm_close(w);
}

void desktop_save_ui(void);

/* A picture behind the desktop (the design's background, kept with it). */
bool desktop_set_wallpaper(const char *path);
static void wallpaper_after_password(const char *path) { desktop_set_wallpaper(path); }

bool desktop_set_wallpaper(const char *path) {
    struct stat st;
    if (!A.ui || stat(path, &st) != 0)
        return false;
    if (family_locks('s')) { /* a child's computer: a grown-up's password first (family.c) */
        family_ask("Changing the wallpaper", wallpaper_after_password, path);
        return true;
    }
    char cmd[800];
    snprintf(cmd, sizeof cmd, "style desktop { background: url(%s) center / cover; }", path);
    fm_command(A.ui, cmd, NULL);
    desktop_save_ui();
    toast("The wallpaper is now %s.", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    return true;
}

/* "Ask NAME about it": the chat opens with it named (words, or what it is), for them to ask; nothing is sent. */
void companion_ask_about_text(const char *what) {
    fm_host_action(A.ui, NULL, "open:companion");
    snprintf(A.chat.buf, sizeof A.chat.buf, "About %s: ", what);
    A.chat.len = (int)strlen(A.chat.buf);
    te_reset(&A.chat.ed, A.chat.buf);
}

int fm_host_window_app(int i) {
    int l[WIN_MAX], n = open_list(l);
    return i >= 0 && i < n ? window_app(l[i]) : 0;
}

/* For its hands (agent.c): the i-th open window's slot, its title, the focused one */
int desktop_window_slot(int i) {
    int l[WIN_MAX], n = open_list(l);
    return i >= 0 && i < n ? l[i] : -1;
}
const char *desktop_window_title(int w) { return w >= 0 && w < WIN_MAX ? window_title(w) : ""; }
int desktop_focused_window(void) { return focused_window(); }

const char *fm_host_window_title(int i) {
    int l[WIN_MAX], n = open_list(l);
    return i >= 0 && i < n ? window_title(l[i]) : "";
}

bool fm_host_window_focused(int i) {
    int l[WIN_MAX], n = open_list(l);
    return i >= 0 && i < n && l[i] == focused_window();
}

bool fm_host_bind(const char *key, char *out, size_t n) {
    if (!strcmp(key, "user.name")) snprintf(out, n, "%s", D.user_name);
    else if (!strcmp(key, "ai.name")) snprintf(out, n, "%s", D.ai_name);
    else if (!strcmp(key, "greeting")) {
        /* Plain and true for the hour: a design places it and styles it, the words are the system's (designs
         * wrote slogans here, "the signal's steady", that meant nothing). */
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        int h = tm.tm_hour;
        const char *part = h < 5 ? "Good night" : h < 12 ? "Good morning" : h < 18 ? "Good afternoon" : "Good evening";
        if (D.user_name[0])
            snprintf(out, n, "%s, %s", part, D.user_name);
        else
            snprintf(out, n, "%s", part);
    }
    else if (!strcmp(key, "tagline")) { /* under it: today's date */
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fm_strftime(out, n, "%A %e %B", &tm);
        if (out[0] && strstr(out, "  "))
            memmove(strstr(out, "  ") + 1, strstr(out, "  ") + 2, strlen(strstr(out, "  ") + 2) + 1);
    }
    else if (!strcmp(key, "rationale")) snprintf(out, n, "%s", D.rationale);
    else if (!strncmp(key, "window.title:", 13)) {
        int w = atoi(key + 13);
        snprintf(out, n, "%s", w >= 0 && w < WIN_MAX ? window_title(w) : "");
    } else if (!strncmp(key, "sense.", 6)) {
        return senses_bind(key + 6, out, n);
    } else
        return false;
    return true;
}

void fm_host_draw_view(const char *app, rect r) {
    if (fm_lint_offscreen) { /* a picture for the stylist: no real app, no side effects */
        stylist_placeholder_view(app, r);
        return;
    }
    int a = fm_app_index(app);
    /* the keyboard: only if this view was clicked last (see kbd_view) */
    const char *who = a == APP_FILES ? "files" : a == APP_NOTES ? "notes" : a == APP_SETTINGS ? "settings"
                    : a == APP_ABOUT ? "about" : a == APP_TERMINAL ? "terminal"
                    : app && !strcasecmp(app, "senses") ? "senses" : "companion";
    if (A.in.clicked && pointer_top < 0 && ui_hover(r))
        snprintf(kbd_view, sizeof kbd_view, "%s", who);
    bool keys = view_has_keys(who);
    int nkeys = A.in.nkeys, nraw = A.in.nraw;
    if (!keys)
        A.in.nkeys = A.in.nraw = 0;
    switch (a) {
    case APP_FILES: files_body(r); break;
    case APP_NOTES: notes_draw(r); break;
    case APP_SETTINGS: settings_body(r); break;
    case APP_ABOUT: about_body(r); break;
    case APP_TERMINAL: {
        palette tp = pal_on(D.surface, 1); /* a child's computer: the Terminal is a grown-up's (family.c) */
        if (family_gate(r, 's', "The Terminal", &tp, fm_host_font(D.font_body, 15), fm_host_font(D.font_body, 13)))
            term_draw(r, keys, D.surface, D.surface_text, D.body_px - 2 < 13 ? 13 : D.body_px - 2);
        break;
    }
    default:
        if (app && !strcasecmp(app, "senses")) /* <view app="senses"/>: what it can sense */
            alive_panel(r);
        else if (app && !strcasecmp(app, "chat"))
            chat_strip(r);
        else
            companion_body(r);
        break;
    }
    if (!keys) { /* hand the keys on to whoever does own them */
        A.in.nkeys = nkeys;
        A.in.nraw = nraw;
    }
}

/* The design's own spot for it (its <ai> or <avatar>), as last drawn: it's told where it is. */
static rect her_spot;
static uint64_t her_spot_at;

bool desktop_her_spot(rect *r) {
    if (!her_spot_at || A.now - her_spot_at > 3000)
        return false;
    *r = her_spot;
    return true;
}

void fm_host_draw_avatar(rect r, const char *frame, const char *tint, const char *shot, bool active) {
    if (!fm_lint_offscreen)
        her_spot = r, her_spot_at = A.now ? A.now : 1;
    if (fm_lint_offscreen) { /* a picture for the stylist: a stand-in */
        fill_rect(A.frame, r, color_mix(D.bg, D.accent, 0.25f));
        return;
    }
    if (presence_draw_avatar(A.frame, r, active || A.companion.pending != NULL, frame, tint, shot))
        return;
    fm_host_draw_orb(r, active); /* no face: its own look, if it has one */
}

void fm_host_draw_orb(rect r, bool active) {
    her_spot = r, her_spot_at = A.now ? A.now : 1;
    active = active || A.companion.pending != NULL;
    if (presence_draw_home(A.frame, r, active))
        return; /* its face, in its frame */
    rect elsewhere;
    if (emblem_free_place(&elsewhere))
        return; /* it sits somewhere else: the design's spot stays as the design made it */
    float s = (float)(r.w < r.h ? r.w : r.h);
    color a = D.accent, b = D.accent2;
    alive_orb(&a, &b, &active); /* attentive while listening */
    if (emblem_draw(A.frame, r.x + r.w / 2.0f, r.y + r.h / 2.0f, s * 0.36f, a, b, active))
        caption_anchor(r); /* its words appear beside it */
}

/* Its look where it placed it itself, under the windows; a click on it is a click on the companion. */
static void draw_her_free(int top, bool over_ui) {
    rect r;
    if (presence_mode() != PRESENCE_LIGHT || !emblem_free_place(&r))
        return;
    color a = D.accent, b = D.accent2;
    bool active = A.companion.pending != NULL;
    alive_orb(&a, &b, &active);
    if (!emblem_draw_free(A.frame, a, b, active))
        return;
    caption_anchor(r);
    if (top < 0 && !over_ui && A.in.clicked && pt_in(r, A.in.mx, A.in.my))
        open_window(WIN_COMPANION);
}

/* An app with no picture of its own (one the companion made, say) gets the symbol of what it is, by its name:
 * a list for Todo, a clock for Stopwatch, the sun for Forecast. -1: nothing fits (its first letter, then). */
static int glyph_for_app(const char *id, const char *label) {
    static const struct {
        const char *words;
        int glyph;
    } kinds[] = {
        {"todo task list check chore agenda plan", G_LIST},
        {"clock timer stopwatch alarm time countdown pomodoro", G_CLOCK},
        {"weather forecast sun sky", G_SUN},
        {"night moon sleep dream", G_MOON},
        {"day book journal diary note notes write writer memo document text letter", G_DOC},
        {"calc calculator math budget money", G_CALC},
        {"music audio song player radio podcast sound", G_MUSIC},
        {"photo picture image camera paint draw gallery", G_PICTURE},
        {"terminal shell console code", G_TERMINAL},
        {"file files folder", G_FOLDER},
        {"search find", G_SEARCH},
        {"setting settings config prefs", G_GEAR},
        {"download downloads", G_DOWNLOAD},
        {"view watch eye", G_EYE},
    };
    char both[160];
    snprintf(both, sizeof both, " %s %s ", id ? id : "", label ? label : "");
    for (char *p = both; *p; p++)
        *p = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : (*p == '-' || *p == '_' || *p == '.') ? ' ' : *p;
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        char w[24];
        for (const char *s = kinds[k].words; *s;) {
            size_t n = strcspn(s, " ");
            if (n && n < sizeof w - 2) {
                snprintf(w, sizeof w, "%.*s", (int)n, s);
                if (strstr(both, w)) /* (contained: "stopwatch", "dailytodo") */
                    return kinds[k].glyph;
            }
            s += n;
            while (*s == ' ')
                s++;
        }
    }
    return -1;
}

int app_glyph_for(const char *id, const char *label) { return glyph_for_app(id, label); }

void fm_host_draw_icon(rect r, int app, const fm_style *st, bool hover) {
    /* colour, mono and classic pictures (iconart.c); the older tiles below */
    if (app >= 0 && app < APP_COUNT &&
        icon_art_styled(A.frame, r, app, st->icon_style, st->icon_shape, st->color, st->opacity, hover))
        return;
    if (app >= APP_COUNT && app < APP_COUNT + nbundled &&
        app_icon_draw(A.frame, r, bundled[app - APP_COUNT].id, bundled[app - APP_COUNT].exec, icon_tile_of(st->icon_shape),
                      icon_look_of(st->icon_style), st->color, st->opacity, hover))
        return; /* the app's own icon */
    if (app >= 0 && app < APP_COUNT) {
        design dd = D;
        if (!strcasecmp(st->icon_shape, "circle")) dd.icons = ICON_CIRCLE;
        else if (!strcasecmp(st->icon_shape, "squircle")) dd.icons = ICON_SQUIRCLE;
        else if (!strcasecmp(st->icon_shape, "square")) dd.icons = ICON_SQUARE;
        else if (!strcasecmp(st->icon_shape, "none") || !strcasecmp(st->icon_shape, "glyph")) dd.icons = ICON_GLYPH;
        if (!strcasecmp(st->icon_style, "filled")) dd.icon_style = ICONSTYLE_FILLED;
        else if (!strcasecmp(st->icon_style, "line")) dd.icon_style = ICONSTYLE_LINE;
        else if (!strcasecmp(st->icon_style, "glow")) dd.icon_style = ICONSTYLE_GLOW;
        else if (!strcasecmp(st->icon_style, "duotone")) dd.icon_style = ICONSTYLE_DUOTONE;
        /* An icon must stand out from whatever is behind it. */
        uint32_t sr = 0, sg = 0, sb = 0, n = 0;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                int x = r.x + r.w * (2 * i + 1) / 8, y = r.y + r.h * (2 * j + 1) / 8;
                if (x < 0 || y < 0 || x >= A.W || y >= A.H)
                    continue;
                uint32_t p = A.frame->px[(size_t)y * A.W + x];
                sr += C_R(p), sg += C_G(p), sb += C_B(p), n++;
            }
        color under = n ? RGBA(sr / n, sg / n, sb / n, 255) : D.bg;
        /* Filled and duotone icons bring a tile of their own; the others
         * are a bare symbol and need one when the ground swallows them. */
        bool bare = dd.icon_style == ICONSTYLE_LINE || dd.icon_style == ICONSTYLE_GLOW || dd.icons == ICON_GLYPH;
        if (bare && (color_contrast(dd.surface_text, under) < 3.0f || color_contrast(dd.accent, under) < 3.0f)) {
            /* a soft tile of its own, and a symbol that reads on it */
            bool light_bg = color_luma(under) > 0.45f;
            color back = light_bg ? RGBA(28, 24, 30, 170) : RGBA(250, 248, 245, 200);
            fill_round(A.frame, rect_inset(r, -4), r.w * 0.3f, back);
            color ink = light_bg ? RGBA(250, 248, 245, 255) : RGBA(28, 24, 30, 255);
            dd.text = dd.surface_text = ink;
            dd.accent = color_mix(dd.accent, ink, 0.7f);
            dd.accent2 = color_mix(dd.accent2, ink, 0.7f);
            dd.surface = back;
        }
        ui_icon(A.frame, r, &dd, app, hover);
        return;
    }
    /* Bundled and other Linux apps: a tile in the design's colours, turned
     * by a hue of its own so no two look alike, with Prism's prism or the
     * first letter of the name the person knows it by. */
    float s = (float)r.w, cx = r.x + s / 2, cy = r.y + s / 2;
    const char *id = fm_host_app_id(app), *label = fm_host_app_label(app);
    unsigned h = 2166136261u;
    for (const char *p = id; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    color tint = color_mix(D.accent, h & 1 ? D.accent2 : D.text, 0.25f + (float)(h % 50) / 100.0f);
    color top = color_mix(tint, RGBA(255, 255, 255, 255), 0.18f), bot = color_mix(tint, RGBA(0, 0, 0, 255), 0.12f);
    fill_round_vgradient(A.frame, r, s * 0.28f, hover ? color_mix(top, RGBA(255, 255, 255, 255), 0.15f) : top, bot);
    color ink = readable_on(tint);
    if (!strcmp(id, "prism")) {
        draw_line(A.frame, cx, cy - s * 0.26f, cx - s * 0.24f, cy + s * 0.18f, s / 20 + 1, ink);
        draw_line(A.frame, cx - s * 0.24f, cy + s * 0.18f, cx + s * 0.24f, cy + s * 0.18f, s / 20 + 1, ink);
        draw_line(A.frame, cx + s * 0.24f, cy + s * 0.18f, cx, cy - s * 0.26f, s / 20 + 1, ink);
    } else if (glyph_for_app(id, label) >= 0) {
        ui_glyph(A.frame, rect_inset(r, (int)(s * 0.24f)), glyph_for_app(id, label), s / 22 + 1, ink);
    } else {
        char letter[2] = {label && *label ? *label : '?', 0};
        if (letter[0] >= 'a' && letter[0] <= 'z')
            letter[0] = (char)(letter[0] - 32);
        draw_text_centered(A.frame, font_get(D.font_heading, s * 0.48f), r, letter, ink);
    }
}

/* Their comfort setting scales every word on the desktop. */
font *fm_host_font(const char *family, float px) {
    if (px < 12) /* nothing on the desktop is smaller than this */
        px = 12;
    return font_get(family, A.prof.text_scale > 0.5f ? px * A.prof.text_scale : px);
}
color fm_host_wallpaper_tone(void) { return D.bg; }
bool fm_host_wallpaper_range(rect r, color *dark, color *light) {
    if (!A.wall || r.w <= 0 || r.h <= 0)
        return false;
    float lo = 2, hi = -1;
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < 8; i++) {
            int x = r.x + (r.w - 1) * i / 7, y = r.y + (r.h - 1) * j / 5;
            if (x < 0 || y < 0 || x >= A.wall->w || y >= A.wall->h)
                continue;
            color c = A.wall->px[(size_t)y * (size_t)A.wall->w + (size_t)x] | 0xff000000u;
            float l = color_luma(c);
            if (l < lo)
                lo = l, *dark = c;
            if (l > hi)
                hi = l, *light = c;
        }
    return hi >= 0;
}

/* The companion speaks about a hidden note of its own (alive.c, actions.c). */
void companion_prompt(const char *note, int max_tokens) {
    if (A.companion.pending)
        return;
    convo_add(&A.companion, "hidden", note);
    convo_send(&A.companion, companion_system(), max_tokens);
}

/* A folder opens in Files, anything else in Notes. */
void desktop_open_path(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(A.files_path, sizeof A.files_path, "%s", path);
        A.files_scroll = 0;
        open_window(WIN_FILES);
        A.win[WIN_FILES].minimized = false;
    } else {
        files_open_path(path); /* text in Notes, pictures in the viewer, apps run... (files.c) */
    }
}

static void ask_companion(const char *text) {
    open_window(WIN_COMPANION);
    A.win[WIN_COMPANION].minimized = false;
    if (text && *text && !A.companion.pending) {
        convo_add(&A.companion, "user", text);
        convo_send(&A.companion, companion_system(), 1600);
    }
}

/* Right-click anything: why is it here? The designer's own reason if it
 * left one for this element (or what contains it), else ask the companion. */
/* "Ask NAME about this": the chat opens with what it was named, for them to say what they want to know. Nothing is
 * sent: it asked the companion "Why does this look like this?" by itself, at every right-click, for nothing. */
static void ask_about(const fm_node *n) {
    char what[120] = "this";
    char *t = n ? fm_text_of(n) : NULL;
    size_t tl = t ? strlen(t) : 0;
    while (tl && strchr(" \t\r\n", t[tl - 1]))
        t[--tl] = 0;
    const char *ts = t ? t + strspn(t, " \t\r\n") : "";
    if (*ts && strlen(ts) <= 32 && !strchr(ts, '\n'))
        snprintf(what, sizeof what, "\u201c%s\u201d", ts);
    else if (n && n->kind == FM_ELEM)
        snprintf(what, sizeof what, "the %s", n->tag);
    free(t);
    fm_host_action(A.ui, NULL, "open:companion");
    snprintf(A.chat.buf, sizeof A.chat.buf, "About %s: ", what);
    A.chat.len = (int)strlen(A.chat.buf);
    te_reset(&A.chat.ed, A.chat.buf);
    LOG("context menu: asking about %s\n", what);
}

static void explain_why(fm_node *n) {
    fm_node *meta = NULL;
    for (fm_node *m = A.ui->root ? A.ui->root->first : NULL; m; m = m->next)
        if (m->kind == FM_ELEM && !strcmp(m->tag, "meta"))
            meta = m;
    for (fm_node *e = n; e; e = e->parent) {
        if (e->kind != FM_ELEM || e->id[0] == '_' || !e->id[0])
            continue;
        for (fm_node *w = meta ? meta->first : NULL; w; w = w->next) {
            const char *f = w->kind == FM_ELEM && !strcmp(w->tag, "why") ? fm_attr_get(w, "for") : NULL;
            if (f && !strcmp(f[0] == '#' ? f + 1 : f, e->id)) {
                char *t = fm_text_of(w);
                const char *said = fm_attr_get(w, "said");
                if (said && *said)
                    toast("%s (You said: “%s”)", t ? t : "", said);
                else
                    toast("%s", t ? t : "");
                free(t);
                return;
            }
        }
    }
    ask_about(n);
}

/* ---- right-click ----
 * A menu of what can be done with the thing under the pointer: an app opened, or uninstalled (if it was installed
 * from Flathub; asked twice), a window in a window list closed, and why it looks as it does (the designer's own
 * reason, or the companion asked) when that is what they want to know. Right-clicking used to ask the companion
 * straight away, whatever they wanted. The menu keeps names, not pointers into the design (a design applied while
 * it is up frees them). */
static struct {
    int req; /* the removal asked for (priv), 0: none */
    char id[128], name[96];
} uninstalling;

/* A window's own: in a list of windows, or on its frame (title bar). */
static void window_menu(int w) {
    char act[160], label[160];
    ctx_menu_for("window");
    if (A.win[w].minimized) {
        snprintf(act, sizeof act, "show:%d", w);
        ctx_menu_action("show", "Show", act, 0, NULL, "restore", "Back on the screen");
    } else {
        snprintf(act, sizeof act, "min:%d", w);
        ctx_menu_action("minimize", "Minimize", act, 0, NULL, "minimize", "Out of the way, in the window list");
    }
    snprintf(act, sizeof act, "max:%d", w);
    bool maxed = wm_is_maximized(w);
    ctx_menu_action("maximize", maxed ? "Restore size" : "Maximize", act, 0, NULL, maxed ? "restore" : "maximize",
                    maxed ? "Back to the size it was" : "The whole screen, the bars aside");
    snprintf(act, sizeof act, "snapl:%d", w);
    ctx_menu_action("snap-left", "Left half", act, 0, NULL, "back", "The left half of the screen");
    snprintf(act, sizeof act, "snapr:%d", w);
    ctx_menu_action("snap-right", "Right half", act, 0, NULL, "forward", "The right half of the screen");
    snprintf(act, sizeof act, "ontop:%d", w);
    ctx_menu_action("keep-on-top", "Keep on top", act, A.win[w].on_top ? CM_ON : 0, NULL, "up",
                    A.win[w].on_top ? "It is kept above the others: again to let it go" : "Above the other windows");
    int app = window_app(w), same = 0;
    for (int k = 0; k < WIN_MAX; k++)
        same += A.win[k].open && window_app(k) == app;
    if (same > 1) {
        snprintf(act, sizeof act, "closeall:%s", fm_host_app_id(app));
        snprintf(label, sizeof label, "Close all %d %s windows", same, fm_host_app_label(app));
        ctx_menu_action("close-windows", label, act, 0, NULL, "close", "Every window of this app");
    }
    snprintf(act, sizeof act, "close:%d", w);
    ctx_menu_action("close", "Close", act, 0, NULL, "close", "This window");
}

/* What is under the pointer, from the design: a picture (its file), a link, words. */
static void context_of(fm_node *n) {
    menu_ctx c;
    memset(&c, 0, sizeof c);
    for (fm_node *e = n; e; e = e->parent) {
        if (e->kind != FM_ELEM)
            continue;
        const char *src = fm_attr_get(e, "src");
        if (!c.image[0] && src && *src && (!strcmp(e->tag, "image") || !strcmp(e->tag, "img") || !strcmp(e->tag, "svg"))) {
            const char *home = getenv("HOME");
            if (src[0] == '~' && home)
                snprintf(c.image, sizeof c.image, "%s%s", home, src + 1);
            else
                snprintf(c.image, sizeof c.image, "%s", src);
        }
        const char *href = fm_attr_get(e, "href"), *what = fm_attr_get(e, "what");
        if (!c.link[0] && href && *href)
            snprintf(c.link, sizeof c.link, "%s", href);
        else if (!c.link[0] && what && (!strncmp(what, "url:", 4) || !strncmp(what, "link:", 5)))
            snprintf(c.link, sizeof c.link, "%s", strchr(what, ':') + 1);
        if (!c.text[0] && (!strcmp(e->tag, "text") || !strcmp(e->tag, "label") || !strcmp(e->tag, "title") ||
                           !strcmp(e->tag, "clock") || !strcmp(e->tag, "p"))) {
            char *t = fm_text_of(e);
            if (t && t[strspn(t, " \t\r\n")])
                snprintf(c.text, sizeof c.text, "%s", t + strspn(t, " \t\r\n"));
            free(t);
        }
    }
    if (c.image[0])
        ctx_menu_for("image");
    else if (c.link[0])
        ctx_menu_for("link");
    menu_context(&c);
}

/* Right-clicked in a Facet app's window (fxwin.c): what is under the pointer there, a picture, a link or words,
 * and what is offered everywhere. */
void desktop_context_menu_in_app(fm_node *n) {
    for (const fm_node *e = n; e; e = e->parent)
        if (e->kind == FM_ELEM && (!strcmp(e->tag, "input") || !strcmp(e->tag, "search")))
            return; /* (a field: its own menu, when the button comes up) */
    ctx_menu_open(A.in.mx, A.in.my);
    for (fm_node *e = n; e; e = e->parent)
        if (e->kind == FM_ELEM && e->vis.w > 0 && e->vis.h > 0) {
            ctx_menu_target(e->vis.x, e->vis.y, e->vis.w, e->vis.h);
            break;
        }
    ctx_menu_for("app");
    context_of(n);
}

static void context_menu(fm_node *n) {
    for (const fm_node *e = n; e; e = e->parent) /* (on a right-click menu itself: nothing, it stays as it is) */
        if (e->kind == FM_ELEM && !strcmp(e->id, "_menu"))
            return;
    const char *app = NULL;
    int win = -1;
    for (fm_node *e = n; e; e = e->parent) {
        if (e->kind != FM_ELEM)
            continue;
        if (win < 0 && e->win >= 0)
            win = e->win;
        const char *a = fm_attr_get(e, "app");
        if (!app && a && *a)
            app = a;
        if (app)
            break;
    }
    ctx_menu_open(A.in.mx, A.in.my);
    for (fm_node *e = n; e; e = e->parent) /* where the thing clicked is: the nearest that is drawn */
        if (e->kind == FM_ELEM && e->vis.w > 0 && e->vis.h > 0) {
            ctx_menu_target(e->vis.x, e->vis.y, e->vis.w, e->vis.h);
            break;
        }
    char label[160], act[200];
    if (win >= 0 && win < WIN_MAX && A.win[win].open) {
        window_menu(win);
    } else if (app) {
        ctx_menu_for("app");
        int k = fm_app_index(app);
        const char *name = k >= 0 ? fm_host_app_label(k) : app;
        snprintf(label, sizeof label, "Open %s", name);
        snprintf(act, sizeof act, "open:%s", app);
        ctx_menu_action("open", label, act, 0, NULL, "forward", NULL);
        int open_wins = 0;
        for (int w = 0; w < WIN_MAX && k >= 0; w++)
            open_wins += A.win[w].open && window_app(w) == k;
        if (open_wins) {
            snprintf(label, sizeof label, open_wins > 1 ? "Close its %d windows" : "Close its window", open_wins);
            snprintf(act, sizeof act, "closeall:%s", app);
            ctx_menu_action("close-windows", label, act, 0, NULL, "close", NULL);
        }
        snprintf(act, sizeof act, "appinfo:%s", app);
        ctx_menu_action("app-info", "About this app", act, 0, NULL, "eye", "What it is, what it may use, how big it is");
        char path[300];
        snprintf(path, sizeof path, FLATPAK_DISK_EXPORTS "/applications/%s.desktop", app);
        if (flatpak_id_ok(app) && access(path, F_OK) == 0 && priv_available()) {
            snprintf(label, sizeof label, "Uninstall %s", name);
            snprintf(act, sizeof act, "uninstall?%s", app);
            ctx_menu_action("uninstall", label, act, CM_DANGER, NULL, "trash", "Removed from this computer, its settings "
                            "with it (asked first)");
        }
    } else { /* the desktop itself, or a part of it that is no app (a clock, a picture, the wallpaper) */
        context_of(n);
        if (!strcmp(n->tag, "desktop") || !strcmp(n->tag, "canvas") || n == A.ui->desktop) {
            ctx_menu_action("change-wallpaper", "Change the wallpaper", "wallpaper-settings", 0, NULL, "picture", NULL);
            ctx_menu_action("new-note", "New note", "new-note", 0, NULL, "doc", NULL);
        }
        ctx_menu_action("redesign", "Redesign", "do:redesign", 0, NULL, "sparkle",
                        "A new look for the desktop, made with you");
        ctx_menu_action("settings", "Settings", "do:open:settings", 0, NULL, "gear", NULL);
    }
    /* last, small: asking the companion about it (the chat opens with it named; nothing is sent). By the nearest
     * element with a name of its own, found again when chosen. */
    const char *id = NULL;
    for (fm_node *e = n; e && !id; e = e->parent)
        if (e->kind == FM_ELEM && e->id[0] && e->id[0] != '_')
            id = e->id;
    snprintf(act, sizeof act, "why:%s:%s", id ? id : "", n->id[0] && n->id[0] != '_' ? n->id : n->tag);
    snprintf(label, sizeof label, "Ask %s about this", D.ai_name[0] ? D.ai_name : "the companion");
    ctx_menu_item(label, act, CM_QUIET | CM_SEP, NULL);
}

void desktop_ctx_chosen(const char *act);
static void uninstall_after_password(const char *act) { desktop_ctx_chosen(act); }

void desktop_ctx_chosen(const char *act) {
    if (!strncmp(act, "open:", 5)) {
        fm_host_action(A.ui, NULL, act);
    } else if (!strncmp(act, "do:", 3)) { /* a design's action (redesign, open:settings) */
        fm_host_action(A.ui, NULL, act + 3);
    } else if (!strncmp(act, "close:", 6)) {
        int w = atoi(act + 6);
        if (w >= 0 && w < WIN_MAX && A.win[w].open)
            wm_close(w);
    } else if (!strncmp(act, "min:", 4)) {
        wm_minimize(atoi(act + 4));
    } else if (!strncmp(act, "show:", 5)) {
        wm_activate(atoi(act + 5));
    } else if (!strncmp(act, "max:", 4)) {
        wm_toggle_maximize(atoi(act + 4));
    } else if (!strncmp(act, "snapl:", 6) || !strncmp(act, "snapr:", 6)) {
        wm_snap(atoi(act + 6), act[4] == 'r');
    } else if (!strncmp(act, "ontop:", 6)) {
        int w = atoi(act + 6);
        if (w >= 0 && w < WIN_MAX && A.win[w].open) {
            A.win[w].on_top = !A.win[w].on_top;
            toast(A.win[w].on_top ? "%s is kept on top." : "%s is no longer kept on top.", desktop_window_title(w));
        }
    } else if (!strncmp(act, "copymsg:", 8)) {
        int i = atoi(act + 8);
        if (i >= 0 && i < A.companion.n && A.companion.msgs[i].text) {
            clipboard_set(A.companion.msgs[i].text);
            toast("The message is copied.");
        }
    } else if (!strncmp(act, "closeall:", 9)) {
        desktop_close_app_windows(act + 9);
    } else if (!strncmp(act, "appinfo:", 8)) {
        settings_open_page("Apps");
    } else if (!strncmp(act, "uninstall?", 10)) { /* the question first */
        const char *id = act + 10;
        int k = fm_app_index(id);
        char label[160], a[200];
        snprintf(label, sizeof label, "Uninstall %s", k >= 0 ? fm_host_app_label(k) : id);
        snprintf(a, sizeof a, "uninstall:%s", id);
        ctx_menu_open(A.in.mx, A.in.my);
        ctx_menu_for("app");
        ctx_menu_add(label, a, true);
        ctx_menu_add("Cancel", "", false);
        ctx_menu_note("Its settings go with it.");
    } else if (!strncmp(act, "uninstall:", 10)) {
        const char *id = act + 10;
        if (!flatpak_id_ok(id) || uninstalling.req)
            return;
        if (family_locks('i')) { /* a child's computer: a grown-up's password first (family.c) */
            char doing[160];
            int k = fm_app_index(id);
            snprintf(doing, sizeof doing, "Uninstalling %s", k >= 0 ? fm_host_app_label(k) : id);
            family_ask(doing, uninstall_after_password, act);
            return;
        }
        int k = fm_app_index(id);
        snprintf(uninstalling.id, sizeof uninstalling.id, "%s", id);
        snprintf(uninstalling.name, sizeof uninstalling.name, "%s", k >= 0 ? fm_host_app_label(k) : id);
        char m[200];
        snprintf(m, sizeof m, "uninstall %s", id);
        uninstalling.req = priv_send(m);
        if (uninstalling.req > 0)
            toast("Uninstalling %s...", uninstalling.name);
        else
            toast("%s couldn't be uninstalled: the system didn't answer", uninstalling.name), uninstalling.req = 0;
    } else if (!strncmp(act, "why:", 4)) {
        char id[64] = "", what[64] = "";
        const char *c = strchr(act + 4, ':');
        if (c) {
            snprintf(id, sizeof id, "%.*s", (int)(c - act - 4), act + 4);
            snprintf(what, sizeof what, "%s", c + 1);
        }
        char sel[80];
        snprintf(sel, sizeof sel, "#%s", id);
        fm_node *n = id[0] && A.ui ? fm_find(A.ui, sel) : NULL;
        if (n) {
            explain_why(n);
        } else {
            char q[200];
            snprintf(q, sizeof q, "Why does the %s look like this?", what[0] ? what : "desktop");
            ask_companion(q);
        }
    }
}

/* Its answer, when it comes (each frame). */
static void uninstall_poll(void) {
    bool ok;
    char *text = NULL;
    if (uninstalling.req > 0 && priv_take(uninstalling.req, &ok, &text)) {
        uninstalling.req = 0;
        LOG("context menu: uninstalling %s: %s\n", uninstalling.id, ok ? "done" : "failed");
        if (ok)
            toast("%s is uninstalled.", uninstalling.name);
        else
            toast("%s couldn't be uninstalled: %s", uninstalling.name, text ? text : "");
        free(text);
        launcher_apps_changed();
    }
}

void fm_host_action(fm_doc *d, fm_node *src, const char *a) {
    (void)src;
    LOG("ui action %s\n", a);
    if (!strncmp(a, "menu:", 5)) { /* a right-click menu's pick (textmenu.c) */
        ctx_menu_take(atoi(a + 5));
        return;
    }
    if (!strncmp(a, "fx:", 3) && fxwin_action(d, a + 3))
        return;
    if (!strncasecmp(a, "open:", 5) || !strncasecmp(a, "launch:", 7)) {
        const char *id = strchr(a, ':') + 1;
        int k = fm_app_index(id);
        if (k >= 0 && k < APP_COUNT) {
            launch(k);
            for (int w = 0; w < WIN_COUNT; w++)
                if (win_app[w] == k)
                    A.win[w].minimized = false;
        } else if (k >= APP_COUNT) {
            launch_bundled(&bundled[k - APP_COUNT]);
        } else {
            char names[32][64], paths[32][300];
            int n = fxwin_list(names, paths, 32), hit = -1;
            for (int i = 0; i < n; i++)
                if (!strcasecmp(names[i], id))
                    hit = i;
            if (hit >= 0)
                fxwin_open(paths[hit]); /* an app of theirs, written in Facet (~/Apps) */
            else
                toast("Nothing called \"%s\" here yet.", id);
        }
    } else if (!strncasecmp(a, "file:", 5)) {
        desktop_open_path(a + 5); /* a folder in Files, anything else in Notes (a canvas's own file view) */
    } else if (!strncasecmp(a, "tray:", 5)) {
        tray_action(a + 5, src ? src->vis : (rect){A.W - 60, 0, 40, 40});
    } else if (!strcasecmp(a, "redesign")) {
        launch(APP_REDESIGN);
    } else if (!strncasecmp(a, "power:", 6)) {
        A.confirm_power = true;
        A.power_restart = !strcasecmp(a + 6, "restart");
    } else if (!strncasecmp(a, "ask", 3)) {
        ask_companion(a[3] == ':' ? a + 4 : NULL);
    } else if (!strncasecmp(a, "launcher", 8)) {
        launcher_open(a[8] == ':' ? a + 9 : NULL); /* the search for everything (launcher.c) */
    } else if (!strncasecmp(a, "search:", 7)) {
        int k = match_app(a + 7);
        if (k >= 0)
            launch(k);
        else if (fm_app_index(a + 7) >= APP_COUNT)
            fm_host_action(d, src, "open:prism");
        else
            ask_companion(a + 7);
    } else if (!strncasecmp(a, "window:", 7)) {
        const char *w = a + 7;
        if (!strncasecmp(w, "focus:", 6)) {
            int l[WIN_MAX], n = open_list(l), i = atoi(w + 6);
            if (i < 0 || i >= n)
                return;
            struct window *win = &A.win[l[i]];
            /* A taskbar item tucks away the window already in front, as
             * taskbars do; an app icon (desktop, dock, menu) only ever
             * brings it forward. */
            bool from_taskbar = false;
            for (const fm_node *p = src; p && !from_taskbar; p = p->parent)
                from_taskbar = !strcmp(p->tag, "windowlist");
            if (from_taskbar && l[i] == focused_window() && !win->minimized)
                wm_minimize(l[i]); /* flies into its item */
            else
                wm_activate(l[i]);
            return;
        }
        int wi = d->cur_win;
        if (wi < 0 || wi >= WIN_MAX)
            wi = focused_window();
        if (wi < 0)
            return;
        /* wm.c moves, snaps, animates and closes windows */
        if (!strcasecmp(w, "close"))
            wm_close(wi);
        else if (!strcasecmp(w, "minimize"))
            wm_minimize(wi);
        else if (!strcasecmp(w, "maximize"))
            wm_toggle_maximize(wi);
        else if (!strcasecmp(w, "drag"))
            wm_begin_move(wi);
    } else if (!strncasecmp(a, "capture", 7)) { /* the screenshot and recording bar (capture.c) */
        const char *k = a[7] == ':' ? a + 8 : "";
        if (!strcasecmp(k, "now"))
            capture_screenshot_now();
        else
            capture_open(!strcasecmp(k, "record") ? 1 : !strcasecmp(k, "shot") ? 0 : -1);
    } else if (!strncasecmp(a, "theme:", 6) && theme_action(a)) {
        /* light, dark, toggle or auto (theme.c) */
    } else if (alive_host_action(a)) { /* senses, quiet:on, lights:... */
    } else {
        toast("I don't know how to \"%s\" yet.", a);
    }
}

/* Closes a window for good (wm.c calls this once it has its last picture). */
void desktop_close_window(int w) {
    struct window *win = &A.win[w];
    if (w >= WIN_COUNT && win->fx) {
        fxwin_close(win->fx);
        win->open = false;
    } else if (w >= WIN_COUNT) {
        wls_close(win->view); /* the app closes itself (and may ask to save first) */
    } else if (w == WIN_TERMINAL) {
        term_close();
        win->open = false;
    } else if (w == WIN_PREVIEW) {
        win->open = false;
        preview_closed(); /* closed without applying: nothing changes */
    } else
        win->open = false;
}

/* ---- the desktop ---- */

void desktop_save_ui(void);
static void save_ui(void) { desktop_save_ui(); }
void desktop_save_ui(void) {
    char *s = fm_serialize(A.ui);
    if (s && A.have_design && A.screen == SCR_DESKTOP) { /* how it looked before, kept when this is different */
        FILE *o = fopen(STATE_DIR "/facet.xml", "r");
        if (o) {
            static char was[262144];
            size_t n = fread(was, 1, sizeof was - 1, o);
            was[n] = 0;
            fclose(o);
            if (n && n < sizeof was - 1 && strcmp(was, s))
                changes_note_design(was);
        }
    }
    FILE *f = fopen(STATE_DIR "/facet.xml", "w");
    if (f && s) {
        fputs(s, f);
        fclose(f);
    } else if (f) {
        fclose(f);
    }
    free(s);
}

/* The designs new ones replaced are kept a while before they are freed: anything still holding a piece of one
 * (a field, a name, a pointer taken before the swap) reads what was there, not freed memory. The desktop crashed
 * a little after a design was applied from the preview (in memchr, from snprintf, reading memory that was gone);
 * kept 10 s, it still crashed, 10 to 30 s after the first design of the session was replaced (three times in one
 * evening, the same address each time). Until what holds it is found (the kernel's report of such a crash now
 * names the code that called, and what was unmapped there and when), they are kept 10 minutes, the last 6. */
#define RETIRED_MAX 6
#define RETIRED_MS (10 * 60 * 1000)
static struct {
    fm_doc *doc;
    uint64_t at;
} retired[RETIRED_MAX];
static int nretired;

static void retired_drop_oldest(void) {
    LOG("design: one replaced %llu s ago freed\n", (unsigned long long)(A.now - retired[0].at) / 1000);
    fm_free(retired[0].doc);
    memmove(&retired[0], &retired[1], sizeof retired[0] * (size_t)(nretired - 1));
    nretired--;
}

static void retired_free(void) {
    while (nretired && A.now - retired[0].at > RETIRED_MS)
        retired_drop_oldest();
}

void desktop_set_ui(fm_doc *doc) {
    if (A.ui && A.ui != doc) {
        if (nretired == RETIRED_MAX)
            retired_drop_oldest();
        retired[nretired].doc = A.ui;
        retired[nretired++].at = A.now;
    }
    A.ui = doc;
}

void desktop_adopt(fm_doc *doc, const design *d, bool first) {
    if (first)
        onboard_forget(); /* it is theirs now: onboarding's kept place goes */
    A.d = *d;
    design_take_their_name(doc, &A.d); /* their own name, never a design's sample one */
    design_take_its_name(doc, &A.d);   /* and its: the one they gave it */
    if (their_name()[0])
        snprintf(A.prof.user_name, sizeof A.prof.user_name, "%s", their_name());
    A.have_design = true;
    A.redesigning = false;
    design_save(&A.d, STATE_DIR "/design.json");
    if (first || !A.ui) {
        desktop_set_ui(doc);
        desktop_enter(); /* the space arrives, piece by piece */
        return;
    }
    /* What both versions have glides and recolours into place; the old
     * picture fades out over it (theme_crossfade, when they applied). */
    fm_morph_from(doc, A.ui, 750);
    if (!A.wall)
        A.wall = canvas_new(A.W, A.H);
    wallpaper_paint(A.wall, &D);
    desktop_set_ui(doc);
    save_ui();
    LOG("design applied from the preview\n");
}

void desktop_open_window(int w) { open_window(w); }

void companion_tick(void) { companion_update(); }

void desktop_enter(void) {
    fm_contrast_floor = A.prof.high_contrast ? 7.0f : 4.5f;
    if (!A.wall)
        A.wall = canvas_new(A.W, A.H);
    wallpaper_paint(A.wall, &D);
    if (!A.ui) {
        char *m = design_to_facet(&D);
        A.ui = fm_parse(m);
        free(m);
    }
    save_ui();
    if (!A.prof.reduce_motion)
        fm_entrance(A.ui); /* the space arrives piece by piece */
    char *desc = fm_describe(A.ui);
    LOG("ui tree:\n%s", desc);
    free(desc);
    for (int i = 0; i < A.ui->nproblems; i++)
        LOG("ui problem line %d: %s\n", A.ui->problems[i].line, A.ui->problems[i].msg);
    A.drag_win = -1;
    set_screen(SCR_DESKTOP);
    if (A.returning && A.restarted) {
        /* Only the desktop program started again (it stopped, and init started it): they never left, and
         * nothing is said. But it must know that what was open closed with it: it took the conversation as it
         * was for the screen as it is ("it's still in the preview"). */
        A.returning = A.restarted = false;
        companion_knows("(The desktop program just restarted: it stopped and started again by itself; the computer "
                        "stayed on and they didn't go anywhere. Every window, the preview and the apps it had open "
                        "closed with it, and anything half done stopped: nothing from before is open or waiting now, "
                        "whatever the conversation above says; what is on the screen now is only what you are told "
                        "about it. Don't greet them or welcome them back.)");
    } else if (A.returning) {
        /* A new session on a desktop they already have: the companion
         * remembers, and says so briefly, without fuss. */
        A.returning = false;
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char when[64], note[1100];
        strftime(when, sizeof when, "%A, %H:%M", &tm);
        /* (it took the screen to be as the conversation left it: "the new look is still in the preview" of one
         * long gone, applied, and switched off since) */
        snprintf(note, sizeof note,
                 "(They have just switched the computer on again. It is %s. It was off: everything that was open "
                 "when it went off closed then (every window, the preview, apps, anything running or half done), so "
                 "nothing from before is open or waiting now, whatever the conversation above says; what is on the "
                 "screen now is only what you are told about it. Say hello in one short, plain sentence, the way "
                 "someone at home would when they walk in. If your last conversation left something real "
                 "unfinished (written in it, not guessed), you may mention it in a few words, as something to pick "
                 "up again, never as still open; otherwise don't mention the past at all. No question unless you "
                 "really need one.)",
                 when);
        convo_add(&A.companion, "hidden", note);
        convo_send(&A.companion, companion_system(), 400);
        /* its words arrive as a subtitle; the window opens when they want it */
    } else if (A.companion.n == 0) {
        convo_add(&A.companion, "hidden",
                  "(They are seeing the desktop you made for them for the first time. Welcome them in one warm "
                  "sentence. Then, in two to four very short lines, tell them why a few things are the way they "
                  "are, each tied to something they told you (use the reasons you wrote down). End by asking "
                  "what feels wrong, so you can change it right now.)");
        convo_send(&A.companion, companion_system(), 1600);
        /* the space arrives first; its words follow as a subtitle */
    } else if (A.redesigning) {
        convo_add(&A.companion, "hidden", "(You just finished the redesign they asked for. Tell them briefly what you changed.)");
        convo_send(&A.companion, companion_system(), 300);
        A.redesigning = false;
    }
}

/* Logs where clickable things are, so tests can find them. With log off it
 * only hashes them, so the log is written only when something moved. */
static void log_rects(fm_node *n, int depth, bool log, uint64_t *hash) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || !n->laid_out || n->st.hidden || (fm_layer_of(n) == 2 && !n->open))
            continue;
        if (n->id[0] != '_' || (n->generated && n->app >= 0 && !strcmp(n->tag, "item"))) {
            char name[80];
            if (n->id[0] != '_')
                snprintf(name, sizeof name, "ui-%s", n->id);
            else
                snprintf(name, sizeof name, "ui-app-%s", fm_host_app_id(n->app));
            if (log)
                LOGRECT(name, n->vis);
            int v[4] = {n->vis.x, n->vis.y, n->vis.w, n->vis.h};
            for (const char *p = name; *p; p++)
                *hash = (*hash ^ (uint8_t)*p) * 1099511628211ull;
            for (int i = 0; i < 4; i++)
                *hash = (*hash ^ (uint32_t)v[i]) * 1099511628211ull;
        }
        if (depth < 12)
            log_rects(n->first, depth + 1, log, hash);
    }
}

/* The mind on this computer loading its model: they hear about it once,
 * and again when it can talk. */
static void waking_note(void) {
    static int said; /* 0 nothing, 1 waking, 2 awake */
    if (strcmp(A.cfg.provider, "builtin"))
        return;
    const char *who = A.prof.ai_name[0] ? A.prof.ai_name : "Your AI";
    if (said == 0 && mind_builtin_waking_for() >= 2 && !mind_builtin_ready()) {
        toast("Starting %s (loading %s)...", who, models_selected()->name);
        A.toast_until = A.now + 600000; /* until it is awake */
        said = 1;
    } else if (said == 1 && mind_builtin_ready()) {
        toast("%s is ready", who);
        said = 2;
    }
}

void desktop_draw(void) {
    PROF("waking note", waking_note());
    PROF("preview", preview_frame());     /* a design being written, or one they applied or dropped */
    portal_take_input(); /* a file chooser an app asked for has the pointer and keys */
    appapi_take_input(); /* ...or an app asking to use the AI */
    body_take_input(); /* ...or onboarding's questions, if they came late */
    browsers_take_input();
    capture_input();     /* Print Screen: the capture bar has the pointer and keys while it is up */
    PROF("theme", theme_frame_begin()); /* light/dark: the hotkey, redesigns, a pending switch */
    caption_input();     /* a click on the companion's caption is for it alone */
    wm_keys();           /* Alt+Tab, Alt+F4, Super+arrows */
    if (A.companion_at && A.now >= A.companion_at) {
        A.companion_at = 0;
        open_window(WIN_COMPANION);
    }
    PROF("companion", companion_update());
    PROF("senses", alive_tick()); /* senses, ambient lights */
    retired_free(); /* the designs replaced, once nothing can be holding them */
    PROF("nerve", nerve_tick()); /* what happened, judged by its reflexes; waking it when one earned it */
    uninstall_poll(); /* (an app uninstalled from its right-click menu: done, or not) */
    PROF("agent", agent_tick()); /* its body: what it did, back to it as the next step; its plans when they're due */
    if (agent_busy())
        for (int i = 0; i < A.in.nkeys; i++)
            if (A.in.keys[i] == K_ESC) {
                agent_stop();
                toast("Stopped.");
                body_feel('p', "stopped what you were doing");
            }
    PROF("voice", voice_poll()); /* a voice it was given, arriving */
    PROF("persona", persona_poll()); /* who it is: written once known, drifting a little */
    static uint64_t desktop_seen;
    if (!desktop_seen)
        desktop_seen = A.now;
    if (A.now - desktop_seen > 20000 && A.provider && !A.companion.pending && !senses_typing() &&
        !senses_fullscreen() && persona_wants_chat()) {
        open_window(WIN_COMPANION);
        A.win[WIN_COMPANION].minimized = false;
        companion_prompt("(You realise you know what they like on their screen but not much about who they are. "
                         "Start a short, easy chat to know them better: say so in your own words, one sentence, and "
                         "ask one question a friend would. Not a test; they can wave it off. If they want to change "
                         "how you sound, that can come up too.)",
                         300);
    }
    if (voice_key_wanted[0]) { /* a cloud voice's key: typed into the system, never into the conversation */
        bool oa = voice_key_wanted[0] == 'o';
        voice_key_wanted[0] = 0;
        char *key = fxwin_secret(oa ? "Your OpenAI API key, used for the companion's voice. It is stored only on this computer."
                                    : "Your ElevenLabs API key, used for the companion's voice. It is stored only on this computer.");
        if (key) {
            settings_set(oa ? "voice.openai_key" : "voice.elevenlabs_key", key);
            memset(key, 0, strlen(key));
            free(key);
            toast("Key saved. Those voices are now available.");
        }
    }
    /* Ctrl+Space anywhere: talk to the companion */
    if (voice_toggle_pressed()) {
        if (voice_listening())
            voice_listen_stop();
        else
            voice_listen_start();
    }
    char *heard = voice_poll_heard();
    if (heard)
        ask_companion(heard);
    free(heard);
    /* While the power question is up, nothing else reacts to input. */
    input_state real_input = A.in;
    if (A.confirm_power) {
        A.in.clicked = A.in.pressed = A.in.right_clicked = false;
        A.in.nkeys = 0;
        A.in.mx = A.in.my = -1000;
    }
    fxwin_reap(); /* Facet apps that closed themselves */
    if (!A.drawing) /* nothing on screen changed: the rest draws (and takes input, which there is none of) */
        return;
    tray_frame_begin(); /* its popovers and toasts take the pointer first */
    bool cached = wm_scene_cached(); /* while a window is moved, the picture below it is kept */
    /* (the wallpaper is drawn below, with the desktop's own surfaces: together they may be kept a moment) */
    if (!A.ui)
        desktop_enter();
    fm_doc *ui = A.ui;
    static uint64_t last;
    float dt = last ? (float)(A.now - last) : 16;
    last = A.now;
    ui->in = &A.in;
    PROF("layout", fm_layout(ui, (rect){0, 0, A.W, A.H}));
    fm_reduce_motion = A.prof.reduce_motion; /* (Settings: less motion, in every design, app and preview) */
    PROF("animate", fm_animate(ui, dt));
    launcher_input(ui); /* while the launcher is up, input is its own */

    /* windows in z order */
    int order[WIN_MAX], n = 0;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized)
            order[n++] = w;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && A.win[order[j - 1]].z + (A.win[order[j - 1]].on_top ? 1 << 28 : 0) >
                                     A.win[order[j]].z + (A.win[order[j]].on_top ? 1 << 28 : 0); j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    /* The pointer belongs to popovers first, then surfaces above windows,
     * then the topmost window under it, then the desktop below. */
    bool over_ui = fm_hit_layer(ui, 2, A.in.mx, A.in.my) || fm_hit_layer(ui, 1, A.in.mx, A.in.my);
    int top = -1;
    if (!over_ui)
        for (int i = n - 1; i >= 0; i--)
            if (ui_hover(A.win[order[i]].r)) {
                top = order[i];
                break;
            }
    wm_pointer(order, n, over_ui, &top); /* borders, moving, resizing, snapping */
    route_to_apps(top);
    if (A.in.pressed && top >= 0) {
        A.win[top].z = ++A.ztop;
        kbd_view[0] = 0; /* a window pressed: typing goes to the windows again */
    }
    pointer_top = top;
    input_state ui_in = A.in;
    if (top >= 0) {
        ui_in.mx = ui_in.my = -10000;
        ui_in.pressed = ui_in.clicked = false;
    }
    ui->in = &ui_in;
    fm_input(ui, 3);
    ui->in = &A.in;
    ctx_menu_after_input();
    bool keys_to_ui = ui->focus != NULL;
    bool on_field = false; /* (a field's own menu opens when the button comes up: textedit.c) */
    for (const fm_node *e = ui->hovered; e && !on_field; e = e->parent)
        on_field = e->kind == FM_ELEM && (!strcmp(e->tag, "search") || !strcmp(e->tag, "input"));
    if (A.in.right_pressed && top < 0 && ui->hovered && !on_field && !te_menu_state()->open)
        context_menu(ui->hovered);
    else if (A.in.right_pressed && top >= 0 && !pt_in(A.win[top].body, A.in.mx, A.in.my) &&
             !te_menu_state()->open) { /* its frame (the title bar): the window's own menu */
        ctx_menu_open(A.in.mx, A.in.my);
        ctx_menu_target(A.win[top].r.x, A.win[top].r.y, A.win[top].r.w, A.win[top].r.h);
        window_menu(top);
    }

    if (!cached) {
        /* What the windows hide completely isn't drawn underneath them: an opaque frame (the corners aside), or
         * at least an opaque body. (A dock under Prism at full size took a fifth of a core, blurred each frame.) */
        rect occ[24];
        int nocc = 0;
        float frad = 0;
        bool frame_opaque = fm_frame_opaque(ui, &frad);
        for (int i = 0; i < n && nocc + 2 <= 24; i++) {
            struct window *ow = &A.win[order[i]];
            if (A.now - ow->opened_at < 400 || (ow->view && ow->view->client_frame)) /* (arriving; its own frame) */
                continue;
            if (frame_opaque) {
                rect wr = ow->r;
                int rad = (int)ceilf(frad) + 1;
                occ[nocc++] = (rect){wr.x + rad, wr.y, wr.w - 2 * rad, wr.h};
                occ[nocc++] = (rect){wr.x, wr.y + rad, wr.w, wr.h - 2 * rad};
            } else if (ow->body_opaque) {
                occ[nocc++] = ow->body;
            }
        }
        /* Windows hiding nearly all of it (Prism, Firefox at full size) and the pointer on one of them: the
         * picture below (wallpaper, the desktop's surfaces, its light) is drawn four times a second, not at every
         * frame; the strips of it still showing move a little less smoothly, and nothing else changes. */
        static canvas *below;
        static uint64_t below_at;
        static bool below_ok;
        long hidden = 0;
        for (int k = 0; k < nocc; k++)
            if ((long)occ[k].w * occ[k].h > hidden)
                hidden = (long)occ[k].w * occ[k].h;
        bool mostly_hidden = hidden * 10 >= (long)A.W * A.H * 9 && top >= 0 && !over_ui;
        if (mostly_hidden && below_ok && below && below->w == A.W && below->h == A.H && A.now - below_at < 250) {
            canvas_unclip(A.frame);
            memcpy(A.frame->px, below->px, (size_t)A.W * A.H * 4);
        } else {
            if (wallpaper_animated(&D))
                PROF("wallpaper", wallpaper_frame(A.frame, &D, (float)A.now / 1000.0f));
            else
                PROF("wallpaper", canvas_blit(A.frame, A.wall, 0, 0));
            fm_set_occluders(ui, occ, nocc);
            PROF("desktop layer", fm_render_layer(ui, A.frame, 0));
            fm_set_occluders(ui, NULL, 0);
            PROF("companion light", draw_her_free(top, over_ui));
            below_ok = false;
            if (mostly_hidden) {
                if (!below || below->w != A.W || below->h != A.H) {
                    canvas_free(below);
                    below = canvas_new(A.W, A.H);
                }
                if (below) {
                    memcpy(below->px, A.frame->px, (size_t)A.W * A.H * 4);
                    below_at = A.now, below_ok = true;
                }
            }
        }
    }
    presence_draw_corner(A.frame); /* its face, when the design has nowhere for it */
    input_state saved = A.in;
    int focused = focused_window();
    for (int i = 0; i < n; i++) {
        int w = order[i];
        if (w != top)
            A.in.clicked = A.in.pressed = false;
        /* Typing goes to the front window (the one last clicked or opened). */
        bool typable = w == WIN_COMPANION || w == WIN_NOTES || w == WIN_TERMINAL || w == WIN_FILES || w == WIN_SETTINGS ||
                       w == WIN_PREVIEW;
        bool keyboard_owner = !keys_to_ui && !kbd_view[0] && typable && (w == focused || (focused < 0 && w == WIN_COMPANION));
        if (!keyboard_owner)
            A.in.nkeys = 0;
        if (w == top) {
            ui->in = &A.in;
            /* an app with its own title bar gets every click: there is no frame of ours to press */
            if (!(A.win[w].view && A.win[w].view->client_frame))
                fm_window_input(ui, w, A.win[w].r, w == focused);
        }
        PROF("windows", wm_draw_window(w, w == focused, draw_window));
        A.in = saved;
    }
    wm_draw_ghosts(); /* closing, or flying to the taskbar */
    PROF("layers above", fm_render_layer(ui, A.frame, 1); fm_render_layer(ui, A.frame, 2));
    PROF("tray", tray_frame_end());
    PROF("launcher", launcher_draw()); /* above everything, the tray's popovers too */

    static uint64_t last_log, last_hash;
    if (A.now - last_log > 500) {
        last_log = A.now;
        uint64_t h = 1469598103934665603ull;
        log_rects(ui->desktop, 0, false, &h);
        if (h != last_hash) {
            last_hash = h;
            log_rects(ui->desktop, 0, true, &h);
        }
    }
    if (ui->changed) {
        ui->changed = false;
        save_ui();
    }

    if (A.confirm_power) {
        A.in = real_input;
        rect r = {A.W / 2 - 260, A.H / 2 - 110, 520, 220};
        fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(D.bg, 0.35f));
        panel(r, D.radius + 6);
        char q[96];
        snprintf(q, sizeof q, "%s now?", A.power_restart ? "Restart" : "Shut down");
        draw_text_centered(A.frame, HF(D.heading_px * 0.8f), (rect){r.x, r.y + 36, r.w, 50}, q, D.surface_text);
        font *f = BF(D.body_px);
        if (ui_button((rect){r.x + 40, r.y + 130, 200, 50}, "Not yet", f, color_alpha(D.surface_text, 0.12f), D.surface_text,
                      D.radius * 0.6f, true))
            A.confirm_power = false;
        if (ui_button((rect){r.x + r.w - 240, r.y + 130, 200, 50}, "Yes", f, D.accent, readable_on(D.accent), D.radius * 0.6f, true)) {
            alive_shutdown(); /* the lights go back to their own apps */
            if (priv_available()) { /* root's work, asked of init: the last word to the disk, then the power */
                priv_call("save", 60000);
                priv_call(A.power_restart ? "restart" : "power-off", 5000);
            } else {
                store_save(); /* the last word goes to the disk */
                sync();
                reboot(A.power_restart ? RB_AUTOBOOT : RB_POWER_OFF);
            }
        }
        for (int i = 0; i < A.in.nkeys; i++)
            if (A.in.keys[i] == K_ESC)
                A.confirm_power = false;
    }
    PROF("caption", caption_draw());
    PROF("senses panel", alive_draw()); /* the senses panel, or a command waiting for their yes */
    voice_indicator(A.W / 2, A.H - 96, D.accent);
    if (A.toast[0] && A.now < A.toast_until) {
        font *f = BF(D.body_px);
        int w = text_width(f, A.toast) + 40;
        rect r = {(A.W - w) / 2, 30, w, 46};
        panel(r, 23);
        draw_text_centered(A.frame, f, r, A.toast, D.surface_text);
    }
    /* The home key, when the design gave it to nothing: the launcher. */
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_META && !fm_hotkey(ui, "meta"))
            launcher_open(NULL);
    wm_draw_overlay(); /* the Alt+Tab switcher */
    portal_draw();      /* over everything: the file chooser */
    browsers_draw();
    body_draw();
    appapi_draw();      /* and an app asking to use the AI */
    theme_frame_end(); /* the old look fades out over the new */
    capture_draw();    /* the capture bar, over everything; the recording pill; what was made */
}

uint64_t fm_host_time_ms(void) { return A.now; }
fm_doc *fm_host_desktop_doc(void) { return A.ui; }

/* The wallpaper as a number: which picture, its size and the design; a living wallpaper changes every frame, so
 * nothing over it is kept. */
uint64_t fm_host_backdrop_sig(void) {
    if (wallpaper_animated(&D) || !A.wall || wm_scene_cached())
        return 0;
    uint64_t h = 1469598103934665603ull;
    uint64_t parts[3] = {(uint64_t)(uintptr_t)A.wall, (uint64_t)A.wall->w << 32 | (uint32_t)A.wall->h,
                         (uint64_t)(uintptr_t)A.ui};
    for (int i = 0; i < 3; i++)
        h = (h ^ parts[i]) * 1099511628211ull;
    return h | 1;
}
