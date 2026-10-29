#define _GNU_SOURCE
#include "app.h"
#include "models.h"
#include "mind.h"
#include "wallpaper.h"
#include "fm/fm.h"
#include "term.h"
#include "wl/wl.h"
#include "store.h"
#include "voice.h"
#include "files.h"
#include "launcher.h"
#include "wm.h"
#include "theme.h"
#include "caption.h"
#include "notes.h"
#include "alive.h"
#include "stylist.h"
#include "settings.h"
#include "tray.h"
#include "iconart.h"
#include "preview.h"
#include "portal.h"
#include "capture.h"

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

static void open_window(int w) {
    struct window *win = &A.win[w];
    if (!win->open) {
        /* Windows open in the space the design leaves free. */
        rect wk = A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H};
        int ww = w == WIN_COMPANION ? 460 : 760, wh = w == WIN_COMPANION ? 640 : 520;
        if (w == WIN_ABOUT)
            ww = 640, wh = 520;
        if (w == WIN_FILES) /* room for its sidebar and a few columns */
            ww = 940, wh = 600;
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
    if (w >= WIN_COUNT) {
        wl_view *v = A.win[w].view;
        return !v ? "" : v->title[0] ? v->title : v->app_id[0] ? v->app_id : "App";
    }
    return D.apps[map[w]].label;
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

static const char *companion_system(void) {
    static char buf[28000];
    /* With a preview open, the document it talks about and edits is that one. */
    fm_doc *seen = preview_doc() ? preview_doc() : A.ui;
    char *tree = seen ? fm_describe(seen) : NULL;
    char portrait[6000], whys[3000];
    portrait_text(&A.prof, portrait, sizeof portrait);
    collect_whys(whys, sizeof whys);
    snprintf(buf, sizeof buf,
             "You are %s, %s's companion living in their personal operating system. You designed their "
             "desktop for them after getting to know them. Be warm, perceptive and brief (one to four sentences). "
             "Talk plainly, like a thoughtful friend: short, concrete sentences, no metaphors or poetry, and never "
             "describe a change your tags didn't make. Don't end every reply with a question: when there's nothing "
             "to ask, simply stop, and let conversations end. Never invent memories or experiences of your own, "
             "and never promise things this computer can't do yet (it has no sound). "
             "You are an AI and say so plainly if it matters; you don't try to keep them talking, and you are glad "
             "when they have people and things away from the screen. How present to be: \"%s\"; redesigning: "
             "\"%s\". No emoji.\n\n"
             "What you know about them (they can see and edit this):\n%s\n"
             "Why things on their desktop are the way they are (use these when they ask \"why\"):\n%s\n"
             "You can see and work their desktop. This is it right now (element, #id, .class, attributes, text):\n%s\n"
             "To act on it, end your reply with one or more tags:\n"
             "[[ui:press #id]] clicks something (opens menus, runs its action). [[ui:open #id]] / [[ui:close #id]] "
             "shows or hides a menu, panel or element. [[ui:set #id attr=value]] changes an attribute (set text=... "
             "changes its words). [[ui:style SELECTOR { property: value; }]] restyles with Facet CSS, e.g. "
             "[[ui:style dock { background: rgba(255,120,80,0.3); }]]. [[ui:add #parent <markup/>]] adds elements "
             "(e.g. <button what=\"open:notes\" icon=\"notes\">Notes</button>, <clock/>, <applist apps=\"files\"/>). "
             "[[ui:remove #id]] removes one. [[open:files]] (or notes, settings, about, terminal, companion) opens an app. "
             "[[capture:shot]] opens the screenshot bar ([[capture:record]] to record the screen, [[capture:now]] takes "
             "the whole screen at once; Print Screen does the same). Your name is theirs to give: if they want to call "
             "you something else, take it warmly and end with [[name:The New Name]]; it changes everywhere.\n"
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
             "claim one without it, and only use tags when their last message asks for something. You can't browse "
             "the web for them yet; say so honestly if asked.%s%s%s",
             D.ai_name, D.user_name[0] ? D.user_name : "the person", A.prof.autonomy[0] ? A.prof.autonomy : "ask first",
             A.prof.redesign[0] ? A.prof.redesign : "only when asked", portrait[0] ? portrait : "(nothing yet)\n",
             whys[0] ? whys : "(not recorded)\n", tree ? tree : "(not shown)", preview_prompt(), alive_prompt(),
             !strncasecmp(A.cfg.model, "qwen3", 5) ? "\n/no_think" : "");
    free(tree);
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

/* Runs and removes every [[ui:...]] tag in a reply. Returns how many ran. */
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
static void companion_rename(const char *raw) {
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

void companion_note(const char *note, bool act) {
    convo_add(&A.companion, "hidden", note);
    if (act)
        act_on = A.companion.n - 1;
    if (!A.companion.pending)
        convo_send(&A.companion, companion_system(), 1600);
    else
        owed = true;
}

static void companion_update(void) {
    struct conversation *c = &A.companion;
    if (owed && !c->pending) {
        owed = false;
        convo_send(c, companion_system(), 1600);
    }
    if (c->error[0] && !c->pending && c->errors <= 1 && A.now - c->pending_since > 2500)
        convo_send(c, companion_system(), 1600); /* one quiet retry */
    if (convo_poll(c) && !c->error[0]) {
        struct msg *m = &c->msgs[c->n - 1];
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
        if (actions_take(c, &from_person)) /* senses and hands (actions.c): it looked something up */
            return;
        m = &c->msgs[c->n - 1];
        /* Its edits are measured like its designs (stylist.c): what one
         * breaks goes back to it once, and the tags of that answer count. */
        static bool repairing;
        bool repair = repairing;
        repairing = false;
        fm_doc *target = (from_person || repair) && has_look_tags(m->text) ? preview_target() : NULL;
        char *before = target ? fm_serialize(target) : NULL;
        int ui_ran = run_ui_tags(m->text, from_person || repair);
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
        /* Their yes or no to the preview, said instead of clicked. */
        if (from_person && strstr(m->text, "[[preview:apply]]"))
            preview_apply();
        else if (from_person && strstr(m->text, "[[preview:discard]]"))
            preview_discard();
        int opened = 0;
        char *o;
        while ((o = strstr(m->text, "[[name:"))) { /* they gave it a new name */
            char *e = strstr(o, "]]");
            char nm[64];
            snprintf(nm, sizeof nm, "%.*s", e ? (int)(e - o - 7) : 0, o + 7);
            char *after = e ? e + 2 : o + strlen(o);
            memmove(o, after, strlen(after) + 1);
            if (from_person && e)
                companion_rename(nm);
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
                m->text = strdup(tag ? "On it. Reworking your space now." : "Done. Have a look.");
        }
        voice_say(m->text); /* heard as well as read */
        /* With its window closed, its words appear as a caption instead
         * (in the preview's own window when that is open). */
        bool in_preview = preview_showing() && (A.screen != SCR_DESKTOP || (A.win[WIN_PREVIEW].open &&
                                                                            !A.win[WIN_PREVIEW].minimized));
        if (!in_preview)
            caption_say(m->text, 0);
        if (!in_preview && (!A.win[WIN_COMPANION].open || A.win[WIN_COMPANION].minimized)) /* kept to read later */
            notify(D.ai_name[0] ? D.ai_name : "Your companion", m->text, "companion");
        if ((ui_ran || tag_failed) && tag_report[0]) {
            /* What its tags did, for it to read from now on: without this it
             * took its own tag-less words for changes it never made, and
             * "sent" the same change again and again. */
            char note[3200];
            snprintf(note, sizeof note, "(What the tags in your last reply did, one line each:\n%s)", tag_report);
            convo_add(c, "hidden", note);
            if (tag_failed && !repair && !broke) {
                convo_add(c, "hidden",
                          "(Some of those FAILED, so that part did not happen. Fix it now: ui tags only, any markup "
                          "written out whole and closed, no words.)");
                convo_send(c, companion_system(), 1600);
                repairing = true;
            }
        }
        if (broke) {
            char note[2600];
            snprintf(note, sizeof note,
                     "(Measured on the screen after your change:\n%.2200sFix these now with ui tags; answer with "
                     "the tags alone, no words.)",
                     broke);
            convo_add(c, "hidden", note);
            convo_send(c, companion_system(), 1600);
            repairing = true;
            free(broke);
        }
    }
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
        saved_n = c->n;
    }
    rect input = {r.x, r.y + r.h - 52, r.w, 50};
    rect area = {r.x, r.y, r.w, r.h - 64};
    /* Lay out bubbles bottom-up so the newest stays visible. */
    canvas_clip(A.frame, area);
    int y = area.y + area.h, maxw = (int)(r.w * 0.82f);
    if (c->pending) {
        y -= 40;
        int dots = 1 + (int)((A.now / 350) % 3);
        char d[8] = "";
        for (int i = 0; i < dots; i++)
            strcat(d, ".");
        fill_round(A.frame, (rect){area.x, y, 70, 34}, 17, color_alpha(D.accent2, 0.25f));
        draw_text(A.frame, f, area.x + 18, y + 4, d, theme_ink((rect){area.x, y, 70, 34}, D.surface_text, 4.5f));
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
    }
    if (!c->pending && ui_type(&A.chat) && A.chat.len) {
        convo_add(c, "user", A.chat.buf);
        A.chat.len = 0;
        A.chat.buf[0] = 0;
        convo_send(c, companion_system(), 1600);
    }
}

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
    store_wipe(); /* and from the disk */
    convo_clear(&A.companion);
    memset(&A.prof, 0, sizeof A.prof);
    A.have_design = false;
    memset(A.win, 0, sizeof A.win);
    term_close();
    desktop_set_ui(NULL);
    A.provider = NULL;
    set_screen(SCR_CHOOSE);
}

/* Settings and its pages live in settings.c. */
static void settings_body(rect r) { settings_draw(r); }

/* About: what this is, and why it looks the way it does, in the design's
 * own colours and type (the palette and faces are shown, not just named). */
static void about_body(rect r) {
    color under = theme_under((rect){r.x, r.y, r.w, 60});
    color ink = theme_fit(D.surface_text, under, 4.5f), muted = theme_fit(D.muted, under, 3.0f);
    color accent = theme_fit(D.accent, under, 3.0f);
    float hp = D.heading_px * 0.62f < 26 ? 26 : D.heading_px * 0.62f;
    font *f = BF(D.body_px), *h = HF(hp), *small = BF(D.body_px - 3), *label = HF(D.body_px - 3);
    r = rect_inset(r, 6);
    /* who: its light, the name, and for whom */
    int hh = font_line_height(h);
    ui_orb(A.frame, r.x + 26.0f, r.y + 28.0f, 17, D.accent, D.accent2, false);
    draw_text(A.frame, h, r.x + 66, r.y + 28 - hh / 2 - 8, "Operating System", ink);
    char who[160];
    if (D.user_name[0])
        snprintf(who, sizeof who, "Version " OS_VERSION "  \xc2\xb7  shaped for %s by %s", D.user_name, D.ai_name);
    else
        snprintf(who, sizeof who, "Version " OS_VERSION "  \xc2\xb7  shaped by %s", D.ai_name);
    draw_text(A.frame, small, r.x + 66, r.y + 28 + hh / 2 - 6, who, muted);
    int y = r.y + 76;
    fill_rect(A.frame, (rect){r.x, y, r.w, 1}, color_alpha(ink, 0.12f));
    y += 22;

    /* why: the designer's own words, at a comfortable measure */
    draw_text(A.frame, label, r.x, y, "WHY IT LOOKS LIKE THIS", accent);
    y += font_line_height(label) + 8;
    int measure = r.w < 560 ? r.w : 560;
    y += draw_text_wrapped(A.frame, f, (rect){r.x, y, measure, 200}, D.rationale[0] ? D.rationale : "No note was left.",
                           ink, 1.45f) + 24;

    /* the palette and the type, shown */
    draw_text(A.frame, label, r.x, y, "PALETTE", accent);
    draw_text(A.frame, label, r.x + r.w / 2, y, "TYPE", accent);
    y += font_line_height(label) + 10;
    const color sw[5] = {D.bg, D.surface, D.text, D.accent, D.accent2};
    for (int i = 0; i < 5; i++) {
        rect s = {r.x + i * 40, y, 30, 30};
        fill_round(A.frame, s, 15, sw[i] | 0xff000000u);
        stroke_round(A.frame, s, 15, 1, color_alpha(ink, 0.18f));
    }
    font *hs = HF(22), *bs = BF(D.body_px);
    draw_text(A.frame, hs, r.x + r.w / 2, y - 2, D.font_heading, ink);
    draw_text(A.frame, bs, r.x + r.w / 2, y + font_line_height(hs), D.font_body, muted);
    y += 30 + font_line_height(hs) + 10;

    /* credits, quietly, at the foot */
    const char *credits = "Kernel, drivers, network stack and desktop written for this operating system. Uses mbed TLS "
                          "(Apache-2.0), stb_truetype (public domain), fonts under the SIL Open Font License, and "
                          "Mozilla's CA list.";
    int ch = draw_text_wrapped(NULL, small, (rect){0, 0, r.w, 200}, credits, 0, 1.35f);
    int cy = r.y + r.h - ch;
    if (cy < y + 8)
        cy = y + 8;
    draw_text_wrapped(A.frame, small, (rect){r.x, cy, r.w, ch}, credits, muted, 1.35f);
}

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
    float e = ease_out((float)(A.now - win->opened_at) / 320.0f);
    r.y += (int)((1 - e) * 36 * (0.3f + D.motion));
    rect body;
    if (win->view && win->view->client_frame) {
        /* the app draws its own title bar and frame: only a shadow from us */
        shadow_round(A.frame, r, 8, 28, RGBA(0, 0, 0, focused ? 110 : 70));
        body = r;
    } else {
        fm_render_window_frame(A.ui, A.frame, w, r, focused, &body);
    }
    canvas_clip(A.frame, body);
    switch (w) {
    case WIN_FILES: files_body(body); break;
    case WIN_COMPANION: companion_body(body); break;
    case WIN_NOTES: notes_draw(body); break;
    case WIN_SETTINGS: settings_body(body); break;
    case WIN_ABOUT: about_body(body); break;
    case WIN_TERMINAL: term_draw(body, focused, D.surface, D.surface_text, D.body_px - 2 < 13 ? 13 : D.body_px - 2); break;
    case WIN_PREVIEW: preview_body(body, focused); break;
    default: app_body(w, body); break;
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
}

/* ---- Linux apps (Wayland) ---- */

static void launch_bundled(struct bundled *b) {
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

const char *fm_host_app_label(int a) {
    if (a >= 0 && a < APP_COUNT)
        return D.apps[a].label[0] ? D.apps[a].label : app_default_name(a);
    scan_bundled();
    if (a < APP_COUNT || a >= APP_COUNT + nbundled)
        return "";
    /* the design may have named it for them: <apps><app id="prism" label="..."/> */
    if (A.ui && A.ui->root)
        for (fm_node *s = A.ui->root->first; s; s = s->next)
            if (s->kind == FM_ELEM && !strcmp(s->tag, "apps"))
                for (fm_node *e = s->first; e; e = e->next) {
                    const char *id = fm_attr_get(e, "id"), *l = fm_attr_get(e, "label");
                    if (e->kind == FM_ELEM && id && l && *l && !strcasecmp(id, bundled[a - APP_COUNT].id))
                        return l;
                }
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

int fm_host_window_app(int i) {
    int l[WIN_MAX], n = open_list(l);
    return i >= 0 && i < n ? window_app(l[i]) : 0;
}

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
        /* The designer wrote it once; the hour keeps changing. A greeting
         * for another part of the day gives way to one for now. */
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        int h = tm.tm_hour;
        const char *g = D.greeting;
        bool wrong = (strcasestr(g, "morning") && h >= 12) || (strcasestr(g, "afternoon") && (h < 12 || h >= 18)) ||
                     (strcasestr(g, "evening") && (h < 17 && h >= 5)) || (strcasestr(g, "good night") && h >= 5 && h < 21);
        if (wrong) {
            const char *part = h < 5 ? "Still up" : h < 12 ? "Good morning" : h < 18 ? "Good afternoon" : "Good evening";
            if (D.user_name[0])
                snprintf(out, n, "%s, %s.", part, D.user_name);
            else
                snprintf(out, n, "%s.", part);
        } else {
            snprintf(out, n, "%s", g);
        }
    }
    else if (!strcmp(key, "tagline")) snprintf(out, n, "%s", D.tagline);
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
    switch (a) {
    case APP_FILES: files_body(r); break;
    case APP_NOTES: notes_draw(r); break;
    case APP_SETTINGS: settings_body(r); break;
    case APP_ABOUT: about_body(r); break;
    case APP_TERMINAL: term_draw(r, false, D.surface, D.surface_text, D.body_px - 2 < 13 ? 13 : D.body_px - 2); break;
    default:
        if (app && !strcasecmp(app, "senses")) /* <view app="senses"/>: what it can sense */
            alive_panel(r);
        else
            companion_body(r);
        break;
    }
}

void fm_host_draw_orb(rect r, bool active) {
    caption_anchor(r); /* its words appear beside it */
    float s = (float)(r.w < r.h ? r.w : r.h);
    color a = D.accent, b = D.accent2;
    active = active || A.companion.pending != NULL;
    alive_orb(&a, &b, &active); /* warmer as the PC heats up, attentive while listening */
    ui_orb(A.frame, r.x + r.w / 2.0f, r.y + r.h / 2.0f, s * 0.36f, a, b, active);
}

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
        open_in_notes(path);
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
    char q[200];
    const char *what = n->id[0] && n->id[0] != '_' ? n->id : n->tag;
    snprintf(q, sizeof q, "Why is the %s here, the way it is?", what);
    ask_companion(q);
}

void fm_host_action(fm_doc *d, fm_node *src, const char *a) {
    (void)src;
    LOG("ui action %s\n", a);
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
            toast("Nothing called \"%s\" here yet.", id);
        }
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
    if (w >= WIN_COUNT) {
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

static void save_ui(void) {
    char *s = fm_serialize(A.ui);
    FILE *f = fopen(STATE_DIR "/facet.xml", "w");
    if (f && s) {
        fputs(s, f);
        fclose(f);
    } else if (f) {
        fclose(f);
    }
    free(s);
}

void desktop_set_ui(fm_doc *doc) {
    if (A.ui && A.ui != doc)
        fm_free(A.ui);
    A.ui = doc;
}

void desktop_adopt(fm_doc *doc, const design *d, bool first) {
    A.d = *d;
    /* The designer read the whole conversation: trust it for names the
     * tags missed. */
    if (!A.prof.ai_name[0] && D.ai_name[0])
        snprintf(A.prof.ai_name, sizeof A.prof.ai_name, "%s", D.ai_name);
    if (!A.prof.user_name[0] && D.user_name[0])
        snprintf(A.prof.user_name, sizeof A.prof.user_name, "%s", D.user_name);
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
    if (A.returning) {
        /* A new session on a desktop they already have: the companion
         * remembers, and says so briefly, without fuss. */
        A.returning = false;
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char when[64], note[400];
        strftime(when, sizeof when, "%A, %H:%M", &tm);
        snprintf(note, sizeof note,
                 "(They have just switched the computer on again. It is %s. Welcome them back in one or two short "
                 "sentences, naturally mentioning one specific thing you remember from before (from the portrait or "
                 "your last conversation). No question unless you really need one.)",
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
        toast("%s is waking up (loading %s)...", who, models_selected()->name);
        A.toast_until = A.now + 600000; /* until it is awake */
        said = 1;
    } else if (said == 1 && mind_builtin_ready()) {
        toast("%s is awake", who);
        said = 2;
    }
}

void desktop_draw(void) {
    waking_note();
    preview_frame();     /* a design being written, or one they applied or dropped */
    portal_take_input(); /* a file chooser an app asked for has the pointer and keys */
    capture_input();     /* Print Screen: the capture bar has the pointer and keys while it is up */
    theme_frame_begin(); /* light/dark: the hotkey, redesigns, a pending switch */
    caption_input();     /* a click on the companion's caption is for it alone */
    wm_keys();           /* Alt+Tab, Alt+F4, Super+arrows */
    if (A.companion_at && A.now >= A.companion_at) {
        A.companion_at = 0;
        open_window(WIN_COMPANION);
    }
    companion_update();
    alive_tick(); /* senses, moments worth a word, ambient lights */
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
    tray_frame_begin(); /* its popovers and toasts take the pointer first */
    bool cached = wm_scene_cached(); /* while a window is moved, the picture below it is kept */
    if (!cached && wallpaper_animated(&D))
        wallpaper_frame(A.frame, &D, (float)A.now / 1000.0f);
    else if (!cached)
        canvas_blit(A.frame, A.wall, 0, 0);
    if (!A.ui)
        desktop_enter();
    fm_doc *ui = A.ui;
    static uint64_t last;
    float dt = last ? (float)(A.now - last) : 16;
    last = A.now;
    ui->in = &A.in;
    fm_layout(ui, (rect){0, 0, A.W, A.H});
    fm_animate(ui, dt);
    launcher_input(ui); /* while the launcher is up, input is its own */

    /* windows in z order */
    int order[WIN_MAX], n = 0;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized)
            order[n++] = w;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && A.win[order[j - 1]].z > A.win[order[j]].z; j--) {
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
    if (A.in.pressed && top >= 0)
        A.win[top].z = ++A.ztop;
    input_state ui_in = A.in;
    if (top >= 0) {
        ui_in.mx = ui_in.my = -10000;
        ui_in.pressed = ui_in.clicked = false;
    }
    ui->in = &ui_in;
    fm_input(ui, 3);
    ui->in = &A.in;
    bool keys_to_ui = ui->focus != NULL;
    if (A.in.right_clicked && top < 0 && ui->hovered)
        explain_why(ui->hovered);

    if (!cached)
        fm_render_layer(ui, A.frame, 0);
    input_state saved = A.in;
    int focused = focused_window();
    for (int i = 0; i < n; i++) {
        int w = order[i];
        if (w != top)
            A.in.clicked = A.in.pressed = false;
        /* Typing goes to the front window (the one last clicked or opened). */
        bool typable = w == WIN_COMPANION || w == WIN_NOTES || w == WIN_TERMINAL || w == WIN_FILES || w == WIN_SETTINGS ||
                       w == WIN_PREVIEW;
        bool keyboard_owner = !keys_to_ui && typable && (w == focused || (focused < 0 && w == WIN_COMPANION));
        if (!keyboard_owner)
            A.in.nkeys = 0;
        if (w == top) {
            ui->in = &A.in;
            /* an app with its own title bar gets every click: there is no frame of ours to press */
            if (!(A.win[w].view && A.win[w].view->client_frame))
                fm_window_input(ui, w, A.win[w].r, w == focused);
        }
        wm_draw_window(w, w == focused, draw_window);
        A.in = saved;
    }
    wm_draw_ghosts(); /* closing, or flying to the taskbar */
    fm_render_layer(ui, A.frame, 1);
    fm_render_layer(ui, A.frame, 2);
    tray_frame_end();
    launcher_draw(); /* above everything, the tray's popovers too */

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
        snprintf(q, sizeof q, "%s now?", A.power_restart ? "Restart" : D.apps[APP_POWER].label);
        draw_text_centered(A.frame, HF(D.heading_px * 0.8f), (rect){r.x, r.y + 36, r.w, 50}, q, D.surface_text);
        font *f = BF(D.body_px);
        if (ui_button((rect){r.x + 40, r.y + 130, 200, 50}, "Not yet", f, color_alpha(D.surface_text, 0.12f), D.surface_text,
                      D.radius * 0.6f, true))
            A.confirm_power = false;
        if (ui_button((rect){r.x + r.w - 240, r.y + 130, 200, 50}, "Yes", f, D.accent, readable_on(D.accent), D.radius * 0.6f, true)) {
            alive_shutdown(); /* the lights go back to their own apps */
            store_save(); /* the last word goes to the disk */
            sync();
            reboot(A.power_restart ? RB_AUTOBOOT : RB_POWER_OFF);
        }
        for (int i = 0; i < A.in.nkeys; i++)
            if (A.in.keys[i] == K_ESC)
                A.confirm_power = false;
    }
    caption_draw();
    alive_draw(); /* the senses panel, or a command waiting for their yes */
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
    theme_frame_end(); /* the old look fades out over the new */
    capture_draw();    /* the capture bar, over everything; the recording pill; what was made */
}

uint64_t fm_host_time_ms(void) { return A.now; }
