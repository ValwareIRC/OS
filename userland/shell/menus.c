/* The right-click menus of a design that has none of its own yet: the companion makes them, once, for that design.
 * A menu is as much a part of a design as its windows: what it is, its shape, where it appears, how one is picked
 * (<menu for="..."> elements, with do="ID" where each action goes and <acts/> for the rest, and their style:
 * fm_reference). Designs written from now on have them; the ones written before don't. So the first time such a
 * design opens a menu, the companion's mind is asked, in the background, to make them; what comes back becomes
 * part of the design, kept with it. Until then the actions sit in a ring around the pointer, in its colours
 * (fm/css.c's builtin rules). Never a list in a box: that was every other system's menu, and it was this one's. */
#include "app.h"
#include "fm/fm.h"
#include "../lib/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void desktop_save_ui(void);

static const fm_doc *asked_for; /* compared, never followed: the design they were asked for */
static job *asking;

/* The design has a menu of its own: a <menu for="..."> in it. */
static bool has_own_menus(const fm_doc *d) {
    for (const fm_node *c = d && d->desktop ? d->desktop->first : NULL; c; c = c->next)
        if (c->kind == FM_ELEM && !strcmp(c->tag, "menu") && !c->generated && c->id[0] != '_' && fm_attr_get(c, "for"))
            return true;
    return false;
}

struct ask {
    struct ai_config cfg;
    char *system, *user;
};

static void ask_child(void *arg, char **result) {
    struct ask *q = arg;
    struct ai_message m = {"user", q->user, NULL, 0};
    char *reply = NULL, err[300] = "";
    if (ai_chat(&q->cfg, q->system, &m, 1, 2000, &reply, err, sizeof err) == 0 && reply) {
        *result = reply;
    } else {
        free(reply);
        size_t n = strlen(err) + 8;
        *result = malloc(n);
        if (*result)
            snprintf(*result, n, "ERR %s", err);
    }
    free(q->system);
    free(q->user);
    free(q);
}

void menus_check_styled(void) {
    if (asking || !A.ui || A.ui == asked_for || !A.provider || has_own_menus(A.ui))
        return;
    asked_for = A.ui;
    struct ask *q = calloc(1, sizeof *q);
    char *doc = fm_serialize(A.ui);
    if (!q || !doc) {
        free(q);
        free(doc);
        return;
    }
    sbuf s;
    sb_init(&s);
    sb_add(&s,
           "You designed the desktop below (a Facet document). It has no right-click menus of its own yet. Make "
           "them, as part of this design: what a menu IS here, its shape, its layout, where it appears and how one "
           "is picked, in this design's own look and spirit, so it belongs to it and to nothing else. Not a list of "
           "words in a box: that is every other system's menu. Think of what fits this design: a ring around the "
           "pointer, tiles, a strip along an edge, actions growing out of the thing clicked, one big action with "
           "the rest small, or something new. The kinds (for=): app (an app's icon or button: open, uninstall, "
           "ask), window (a window: minimize or show, maximize, close, ask), desktop (the desktop or a part of it "
           "that is no app: redesign, settings, ask), text (a text field: undo, cut, copy, paste, select-all), file "
           "(Files: open, rename, copy, cut, paste, move-to-trash, new-folder, select-all, the sorts...). One menu "
           "may serve several kinds (for=\"app window\"). Put an element with do=\"ID\" where an action goes, "
           "<acts/> for the rest (style how they are laid out). The menus are popovers: placement=\"pointer\", "
           "\"around\", \"on\", \"center\", or place=\"...\". Style :hover and :focus alike (the arrow keys "
           "choose too), .danger, .off, .on, .quiet, text.note, text.keys.\n"
           "Reply with the <menu> elements, then one <style> with their rules; nothing else.\n\nTHE LANGUAGE\n");
    sb_add(&s, fm_reference());
    q->system = s.p;
    sbuf u;
    sb_init(&u);
    sb_add(&u, "The design:\n\n");
    sb_add(&u, doc);
    free(doc);
    q->user = u.p;
    q->cfg = A.cfg;
    asking = job_start_thread(ask_child, q);
    if (!asking) {
        free(q->system);
        free(q->user);
        free(q);
        return;
    }
    LOG("menus: this design has none of its own: asking the companion for them\n");
}

/* Its answer: the rules, kept with the design they were asked for (if it is still the one in use). */
void menus_tick(void) {
    char *res = NULL;
    if (!asking || !job_poll(asking, &res))
        return;
    job_free(asking);
    asking = NULL;
    if (!res || !strncmp(res, "ERR", 3)) {
        LOG("menus: no answer (%.200s)\n", res ? res + (res[3] ? 4 : 3) : "");
        asked_for = NULL; /* (asked again the next time one opens) */
        free(res);
        return;
    }
    /* the rules alone: a fence around them (```css ... ```) taken off */
    char *css = res;
    char *fence = strstr(css, "```");
    if (fence) {
        css = fence + 3;
        css += strcspn(css, "\n");
        char *end = strstr(css, "```");
        if (end)
            *end = 0;
    }
    /* the <menu> elements into the design's desktop, their <style> into its stylesheet */
    int menus = 0;
    sbuf els;
    sb_init(&els);
    for (char *p = css; (p = strstr(p, "<menu")) && strlen(css) < 60000;) {
        char *end = strstr(p, "</menu>");
        char *self = strchr(p, '>');
        if (end && (!self || self[-1] != '/' || self > end))
            end += 7;
        else if (self && self[-1] == '/')
            end = self + 1;
        else
            break;
        sb_addn(&els, p, (size_t)(end - p));
        sb_add(&els, "\n");
        menus++;
        p = end;
    }
    char *st = strstr(css, "<style>"), *st_end = st ? strstr(st, "</style>") : NULL;
    if (st_end)
        *st_end = 0;
    const char *rules_text = st ? st + 7 : "";
    if (A.ui != asked_for) { /* another design since: it asks for its own */
        LOG("menus: the design changed meanwhile; not kept\n");
    } else if (!menus || !A.ui->desktop) {
        LOG("menus: what came back had no <menu> (%zu bytes); not kept\n", strlen(res));
        asked_for = NULL;
    } else {
        fm_node *first = fm_parse_fragment(A.ui, A.ui->desktop, els.p);
        if (rules_text[0])
            fm_css_patch(A.ui, rules_text);
        A.ui->changed = true;
        desktop_save_ui();
        LOG("menus: the companion made this design's menus (%d, %s)\n", menus, first ? "kept" : "unreadable");
    }
    sb_free(&els);
    free(res);
}
