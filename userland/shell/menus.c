/* The right-click menus of a design that has none of its own yet: the companion writes them, once, in the design's
 * own look. They are as much a part of a design as its windows (fm_reference says so, and designs written from
 * now on have them), but the ones written before didn't: their menus were the system's plain one. So the first
 * time such a design opens a menu, the companion's mind is asked, in the background, for the CSS of its menus
 * (menu, menu button, :hover, :focus, .danger, .off, .on, .quiet, menu[for=...]); what comes back becomes part of
 * the design, kept with it, and the menus look like it from then on. Until then they have its colours, font and
 * corners (fm/css.c's builtin rules). */
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

/* A rule for menus, by its tag (not #startmenu, not menubar, not a word in a value). */
static bool has_menu_rules(const char *css) {
    for (const char *p = css ? css : ""; (p = strstr(p, "menu")); p += 4) {
        char before = p > css ? p[-1] : ' ', after = p[4];
        if (isalnum((unsigned char)before) || before == '-' || before == '_' || before == '#' || before == '.')
            continue;
        if (isalnum((unsigned char)after) || after == '-' || after == '_')
            continue;
        /* a selector: what follows, up to "{" or ";" or "}", reaches "{" first */
        const char *brace = strchr(p, '{'), *semi = strchr(p, ';'), *close = strchr(p, '}');
        if (brace && (!semi || brace < semi) && (!close || brace < close))
            return true;
    }
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
    if (asking || !A.ui || A.ui == asked_for || !A.provider || has_menu_rules(A.ui->css))
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
           "You designed the desktop below (a Facet document). Its right-click menus have no look of their own yet: "
           "the system shows them in its plain colours. Write their CSS, as part of this design: in its own look, "
           "the way its other menus, panels, buttons and type look, so they belong to it and to nothing else.\n"
           "A menu is <menu for=\"app|window|desktop|text|file\"> (what was right-clicked) holding <button> items "
           "(each one thing to do: .danger removes something, .off can't be done now, .on is the one chosen, "
           ".quiet the small last line), <divider/>, <text class=\"keys\"> (a shortcut, after a <spacer/>) and "
           "<text class=\"note\">. Style menu, menu button, menu button:hover and menu button:focus (the arrow keys "
           "choose too: style them alike), .danger, .off, .on, .quiet, menu divider, menu text.keys, menu text.note, "
           "and menu[for=window] and the rest for any kind that should differ. Use this design's own variables, "
           "fonts and materials.\n"
           "Reply with the CSS rules and nothing else: no markup, no explanation.\n\nTHE LANGUAGE\n");
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
    if (A.ui != asked_for) { /* another design since: it asks for its own */
        LOG("menus: the design changed meanwhile; not kept\n");
    } else if (!has_menu_rules(css) || strchr(css, '<') || strlen(css) > 12000) {
        LOG("menus: what came back wasn't menu rules (%zu bytes); not kept\n", strlen(css));
    } else {
        fm_css_patch(A.ui, css);
        A.ui->changed = true;
        desktop_save_ui();
        int rules = 0;
        for (const char *p = css; (p = strchr(p, '{')); p++)
            rules++;
        LOG("menus: the companion wrote this design's menus (%d rules)\n", rules);
    }
    free(res);
}
