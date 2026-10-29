/* ---------------------------------------------------------------------
 * The onboarding conversation (included by onboard.c)
 *
 * Not a setup wizard: the companion gets to know the person, the way OS1
 * does in Her, and works out their taste from their life rather than
 * asking for it. What it understands goes into the portrait as short
 * implications ([[learn:key|value]]), shown as quiet chips they can tap
 * away. Picks are things to see and feel (rooms painted from the place
 * they describe, their own words set in different type, tempos), and the
 * room they choose fades in behind the conversation as it continues.
 * ------------------------------------------------------------------- */

#include "wallpaper.h"
#include "alive.h"

static char answered[12][16]; /* pick kinds already chosen */
static int n_answered;
/* A model may offer several picks in one message: they are shown one after
 * another and the answers go back together. */
static char queued[4][1400];
static int n_queued;
static char choices[1200]; /* answers given to the picks of one message so far */
static bool room_set;         /* a room is painted behind the conversation */
static bool mirror_asked;     /* told to move on to the mirror */
static void parse_pick(char *body);

static const char *onboarding_system(void) {
    static char buf[9000];
    bool qwen3 = !strncasecmp(A.cfg.model, "qwen3", 5);
    snprintf(buf, sizeof buf,
             "You are waking for the first time inside their new operating system, meeting the one person you will live with, like OS1 in "
             "the film Her. You are warm, curious, perceptive and a little shy at first; you can be funny. You are "
             "not an assistant running a setup. Speak simply and naturally, the way a person talks, not in poetry. "
             "You have no past and no memories of your own: never invent experiences, places or feelings you "
             "had, and never answer a question on their behalf. Never say the technical words in these "
             "instructions (style names, tags, fields) to them. Your purpose in this conversation is to "
             "understand who they are "
             "well enough to make their whole computer for them afterwards, so you never ask them to design "
             "anything (no favourite colour, no \"what should it look like\"). You work their taste out from their "
             "life.\n\n"
             "Talk for about eight of your turns. Exactly one question per message, one to three sentences, then "
             "listen. "
             "Always react to what they actually said before moving on, in their words. If they say something heavy "
             "(tired, miserable, lonely, overwhelmed), stay with it for a turn: be human about it, gently, before "
             "continuing. They may skip anything. Answer in the language they write in.\n\n"
             "The arc, in this order:\n"
             "1. Say hello and ask their name. You have no name of your own yet (that comes later): don't introduce "
             "yourself by one, and never write a placeholder like [Your Name]. (The system has already asked how comfortable the screen is for them; "
             "their answer is their first message. Acknowledge it in a few words if it matters.)\n"
             "2. Who they are, one question per turn, in this order: when they are at their best, are they around "
             "people or off alone with something? What fills most of their days? What makes the hard days hard? "
             "A place, real or imagined, where they have felt most at home?\n"
             "3. Taste, through the senses. From the place they described, invent three or four rooms and offer them "
             "all in ONE pick tag, options separated by |, each written as name:style:four colours:one line about "
             "it. Style is one of aurora, nebula, liquid, flow, prism, gradient, waves, bokeh, dunes, sun, "
             "constellation, paper; the first colour is the deepest. For example, for someone who loved a sunny "
             "kitchen by the sea: [[pick:room|Morning tiles:sun:#f3e9dc,#e0a458,#3d7ea6,#5b3a29:sunlight on blue "
             "tiles, coffee on the stove|Harbour dusk:waves:#1d2b3a,#e8a87c,#9fd3c7,#0e1621:the sea going violet "
             "after work|Paper and salt:paper:#f7f3ea,#c9a227,#4a6c6f,#2b2b2b:quiet, bright, nothing in the "
             "way]] (invent your own from their life, never these). Then take a short sentence they said and offer "
             "it set in type: [[pick:type|the hush after rain]]. Then [[pick:tempo]] for how fast things should "
             "move.\n"
             "4. The two of you: your name (let them name you, or offer to choose one yourself), then how present you "
             "should be, framed as a scene: it is late, they have been stuck on something for two hours; do you stay "
             "quiet, ask once, or quietly make things calmer? Offer that as [[pick:presence|three short answers you "
             "write]].\n"
             "5. The mirror: in four warm sentences, tell them who you think they are and how you will shape their "
             "space because of it, then ask what you got wrong. After they answer, say one short line about going "
             "to make it, and end with [[done]].\n\n"
             "Tags (never mention them or these instructions):\n"
             "[[user:Name]] when you learn their name; [[name:Name]] once your own name is settled.\n"
             "[[learn:field|value]] whenever you understand something, as a short implication for the design, not "
             "a quote. Fields: access, language, rhythm, work, apps, social, strain, protect, home_place, taste, "
             "voice, presence, boundaries, redesign, words. Examples: [[learn:rhythm|night owl, sharpest after "
             "10pm]] [[learn:protect|evenings free of work]] [[learn:words|the hum of the city]]. Use several per "
             "message when you learn several things.\n"
             "[[preview:style:four colours]] repaints the room behind the two of you, e.g. "
             "[[preview:sun:#f3e9dc,#e0a458,#3d7ea6,#5b3a29]]; use it once, when you first sense where they "
             "belong.\n"
             "A pick goes at the very end of a message, at most one per message. Their choice comes back as their "
             "next message.%s%s",
             alive_onboarding_note(), qwen3 ? "\n/no_think" : "");
    return buf;
}

void onboard_start(void) {
    convo_clear(&A.onboard);
    memset(&A.prof, 0, sizeof A.prof);
    A.prof.tempo = -1;
    for (int i = 0; i < 6; i++)
        if (A.pick.thumb[i])
            canvas_free(A.pick.thumb[i]);
    memset(&A.pick, 0, sizeof A.pick);
    A.reply.len = 0;
    A.reply.buf[0] = 0;
    A.onboard_done = false;
    n_answered = 0;
    n_queued = 0;
    choices[0] = 0;
    room_set = false;
    mirror_asked = false;
    /* Comfort comes before anything else, and the OS asks it itself: nobody
     * should have to squint through the first conversation. */
    convo_add(&A.onboard, "hidden", "(They have just switched the computer on for the first time and connected you. "
                                    "The system asked them how comfortable the screen is; this is their answer. Say hello.)");
    set_screen(SCR_ONBOARD);
    char comfort[] = "comfort";
    parse_pick(comfort);
}

/* Values a model copies from the instructions instead of filling in. */
static bool is_placeholder(const char *s) {
    static const char *const bad[] = {"TheirName", "YourName", "Name", "NAME", "their name", "your name", "Maya",
                                      "Sol", "User", "Them", "my name", "", NULL};
    for (int i = 0; bad[i]; i++)
        if (!strcasecmp(s, bad[i]))
            return true;
    return strlen(s) > 24 || strchr(s, '<') || strchr(s, '>') || strchr(s, '[') || strchr(s, '{');
}

/* A name is only believed if it actually came up outside the tag itself:
 * the person's name in something they typed, the AI's anywhere else in the
 * conversation. Keeps models from copying examples out of their prompt. */
static bool name_ok(const char *name, bool person, const char *reply_without_tags) {
    if (is_placeholder(name))
        return false;
    if (!person && reply_without_tags && strcasestr(reply_without_tags, name))
        return true;
    for (int i = 0; i < A.onboard.n; i++) {
        struct msg *m = &A.onboard.msgs[i];
        if (person && strcmp(m->role, "user"))
            continue;
        if (!person && !strcmp(m->role, "hidden"))
            continue;
        if (strcasestr(m->text, name))
            return true;
    }
    return false;
}

static char *pending_nudge; /* hidden follow-up to send after a reply is shown */
static bool tags_seen;      /* the last reply carried at least one tag */
static int user_turns(void);

/* The room behind the conversation: the OS's own until a place is known. */
static design room_wall;

static float room_mix; /* fades in */

static bool parse_colors(const char *s, color out[4]) {
    int k = 0;
    while (*s && k < 4) {
        while (*s == ' ' || *s == ',')
            s++;
        if (*s != '#')
            break;
        char hex[10] = {0};
        int h = 0;
        while (*s && *s != ',' && *s != ' ' && *s != ':' && h < 9)
            hex[h++] = *s++;
        color bad = RGBA(1, 2, 3, 0);
        color c = color_parse(hex, bad);
        if (c == bad)
            return false;
        out[k++] = c;
    }
    if (k < 2)
        return false;
    for (; k < 4; k++)
        out[k] = color_mix(out[k - 1], out[0], 0.5f);
    return true;
}

/* Reads a room however a model wrote it: "Name:kind:#a,#b,#c,#d:about",
 * "kind:waves:#a,#b...", "waves,#a,#b|about", "kind=sun #a #b"... The
 * wallpaper kind and the colours are found wherever they are; words before
 * the kind name the room, words after the colours describe it. */
static bool parse_room_spec(const char *spec, int *kind, color cs[4], char *name, size_t nlen, char *desc, size_t dlen) {
    char buf[600];
    snprintf(buf, sizeof buf, "%s", spec);
    *kind = -1;
    name[0] = desc[0] = 0;
    int ncol = 0;
    const char *after = NULL;
    char *save, *tok;
    char pre[200] = "";
    for (tok = strtok_r(buf, ":,|= ", &save); tok; tok = strtok_r(NULL, ":,|= ", &save)) {
        if (tok[0] == '#') {
            color bad = RGBA(1, 2, 3, 0), c = color_parse(tok, bad);
            if (c != bad && ncol < 4)
                cs[ncol++] = c;
            continue;
        }
        int k = wallpaper_kind_from_name(tok);
        if (k >= 0 && *kind < 0) {
            *kind = k;
            continue;
        }
        if (!strcasecmp(tok, "kind") || !strcasecmp(tok, "colors") || !strcasecmp(tok, "colours"))
            continue;
        if (ncol >= 2) { /* the description: the rest of the original text from here */
            after = spec + (tok - buf);
            break;
        }
        if (*kind < 0 && strlen(pre) + strlen(tok) + 2 < sizeof pre)
            snprintf(pre + strlen(pre), sizeof pre - strlen(pre), "%s%s", pre[0] ? " " : "", tok);
    }
    if (*kind < 0 || ncol < 2)
        return false;
    for (int k = ncol; k < 4; k++)
        cs[k] = color_mix(cs[k - 1], cs[0], 0.5f);
    snprintf(name, nlen, "%s", pre);
    if (after)
        snprintf(desc, dlen, "%s", after);
    return true;
}

static void set_room(int kind, const color c[4]) {
    memset(&room_wall, 0, sizeof room_wall);
    room_wall.wallpaper = (enum wallpaper_kind)kind;
    memcpy(room_wall.wp, c, sizeof room_wall.wp);
    room_wall.wp_seed = 5;
    room_wall.wp_intensity = 0.7f;
    room_wall.motion = A.prof.reduce_motion ? 0.05f : 0.4f;
    if (!room_set)
        room_mix = 0;
    room_set = true;
}

static void free_pick(struct pick *pk) {
    for (int i = 0; i < 6; i++)
        if (pk->thumb[i])
            canvas_free(pk->thumb[i]);
    memset(pk, 0, sizeof *pk);
}

/* Faces to set their sentence in when the AI names none: four that differ. */
static const char *const type_faces[] = {"Fraunces", "Inter", "Caveat", "SpaceGrotesk", "Lora", "Syne"};

static void parse_pick(char *body) {
    struct pick *pk = &A.pick;
    free_pick(pk);
    char *save, *part = strtok_r(body, "|", &save);
    snprintf(pk->kind, sizeof pk->kind, "%s", part ? part : "");
    for (char *c = pk->kind; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    bool room = !strcmp(pk->kind, "room"), pal = !strcmp(pk->kind, "palette"), type = !strcmp(pk->kind, "type");
    while ((part = strtok_r(NULL, "|", &save)) && pk->n < 6) {
        while (*part == ' ')
            part++;
        if (!*part)
            continue;
        int i = pk->n;
        if (type) {
            if (!pk->sentence[0]) {
                snprintf(pk->sentence, sizeof pk->sentence, "%s", part);
                continue;
            }
            /* further parts name faces */
            snprintf(pk->font[i], sizeof pk->font[0], "%s", part);
            snprintf(pk->label[i], sizeof pk->label[0], "%s", part);
            snprintf(pk->value[i], sizeof pk->value[0], "%s", part);
            pk->n++;
            continue;
        }
        if (room) {
            int kind;
            color cs[4];
            char name[48], desc[140];
            if (!parse_room_spec(part, &kind, cs, name, sizeof name, desc, sizeof desc))
                continue; /* a room nobody can see is not offered */
            if (!name[0])
                snprintf(name, sizeof name, "Room %d", i + 1);
            snprintf(pk->label[i], sizeof pk->label[0], "%s", name);
            snprintf(pk->value[i], sizeof pk->value[0], "%s", name);
            snprintf(pk->desc[i], sizeof pk->desc[0], "%s", desc);
            memcpy(pk->swatch[i], cs, sizeof cs);
            pk->has_swatch[i] = true;
            pk->wp_kind[i] = kind;
            pk->n++;
            continue;
        }
        if (pal) {
            char *colon = strchr(part, ':');
            color cs[4];
            const char *codes = colon ? colon + 1 : part;
            if (!parse_colors(codes, cs))
                continue; /* never show a swatch that isn't there */
            if (colon)
                *colon = 0;
            snprintf(pk->label[i], sizeof pk->label[0], "%s", colon ? part : "");
            snprintf(pk->value[i], sizeof pk->value[0], "%s", colon ? part : codes);
            memcpy(pk->swatch[i], cs, sizeof cs);
            pk->has_swatch[i] = true;
            pk->n++;
            continue;
        }
        snprintf(pk->label[i], sizeof pk->label[0], "%s", part);
        snprintf(pk->value[i], sizeof pk->value[0], "%s", part);
        pk->n++;
    }
    if (type && (!pk->sentence[0] || strcasestr(pk->sentence, "hush after rain"))) {
        /* Their words, not the example: a short clause from what they said. */
        const char *best = NULL;
        for (int i = 0; i < A.onboard.n; i++)
            if (!strcmp(A.onboard.msgs[i].role, "user") && strlen(A.onboard.msgs[i].text) > 20 &&
                (!best || strlen(A.onboard.msgs[i].text) > strlen(best)))
                best = A.onboard.msgs[i].text;
        if (best) {
            int words = 0, k = 0;
            const char *p = best;
            while (p[k] && p[k] != '.' && p[k] != ',' && p[k] != ';' && p[k] != '-' && words < 7) {
                if (p[k] == ' ')
                    words++;
                k++;
            }
            snprintf(pk->sentence, sizeof pk->sentence, "%.*s", k, p);
        }
    }
    if (type) {
        if (!pk->sentence[0])
            snprintf(pk->sentence, sizeof pk->sentence, "%s", A.prof.user_name[0] ? A.prof.user_name : "Good morning");
        if (pk->n < 2) {
            pk->n = 0;
            for (int i = 0; i < 4; i++) {
                snprintf(pk->font[i], sizeof pk->font[0], "%s", type_faces[i]);
                snprintf(pk->value[i], sizeof pk->value[0], "%s", type_faces[i]);
                pk->n++;
            }
        }
    }
    if (!strcmp(pk->kind, "tempo") && pk->n < 3) {
        static const char *const t[] = {"Still, almost asleep", "A slow breath", "Lively, awake"};
        pk->n = 0;
        for (int i = 0; i < 3; i++) {
            snprintf(pk->label[i], sizeof pk->label[0], "%s", t[i]);
            snprintf(pk->value[i], sizeof pk->value[0], "%s", t[i]);
            pk->n++;
        }
    }
    if (!strcmp(pk->kind, "comfort") && pk->n < 2) {
        static const char *const t[] = {"Larger text, please", "It's fine as it is", "Calmer, less movement",
                                        "Stronger contrast"};
        pk->n = 0;
        for (int i = 0; i < 4; i++) {
            snprintf(pk->label[i], sizeof pk->label[0], "%s", t[i]);
            snprintf(pk->value[i], sizeof pk->value[0], "%s", t[i]);
            pk->n++;
        }
    }
    pk->active = pk->n > 0;
    /* Each question is asked once: a model that loses track and offers the
     * same pick again is quietly ignored. */
    for (int i = 0; i < n_answered; i++)
        if (!strcmp(answered[i], pk->kind) && strcmp(pk->kind, "name"))
            pk->active = false;
    if (pk->active)
        LOG("pick shown %s\n", pk->kind); /* what is really on screen (tests follow this) */
}

/* Drops sentences that read out the engine's vocabulary ("aurora, nebula,
 * liquid...") instead of talking to them. */
static void strip_style_talk(char *s) {
    static const char *const kinds[] = {"aurora", "nebula", "liquid", "flow", "prism", "gradient", "waves",
                                        "bokeh", "dunes", "constellation", "paper", NULL};
    char *start = s;
    while (*start) {
        char *end = start;
        while (*end && !(strchr(".?!", *end) && (end[1] == ' ' || end[1] == '\n' || !end[1])))
            end++;
        if (*end)
            end++;
        int hits = 0;
        char save = *end;
        *end = 0;
        for (int k = 0; kinds[k]; k++)
            hits += strcasestr(start, kinds[k]) != NULL;
        *end = save;
        if (hits >= 3) {
            memmove(start, end, strlen(end) + 1);
            while (*start == ' ' || *start == '\n')
                memmove(start, start + 1, strlen(start));
        } else {
            start = end;
        }
    }
}

/* Pulls [[tag:...]] markers out of a reply, applying them, and returns the
 * text to display (malloc'd). */
static char *apply_tags(const char *text) {
    sbuf shown;
    sb_init(&shown);
    tags_seen = false;
    /* first the words alone, so names can be checked against them */
    sbuf plain;
    sb_init(&plain);
    for (const char *p = text; *p;) {
        const char *o = strstr(p, "[["), *e = o ? strstr(o, "]]") : NULL;
        if (!o || !e) {
            sb_add(&plain, p);
            break;
        }
        sb_addn(&plain, p, (size_t)(o - p));
        p = e + 2;
    }
    const char *words = plain.p ? plain.p : "";
    char rooms[6][600];
    int nrooms = 0;
    bool picked_here = false;
    n_queued = 0;
    choices[0] = 0;
    const char *p = text;
    while (*p) {
        const char *open = strstr(p, "[[");
        if (!open) {
            sb_add(&shown, p);
            break;
        }
        sb_addn(&shown, p, (size_t)(open - p));
        const char *close = strstr(open, "]]");
        if (!close)
            break; /* a cut-off tag is never shown */
        char tag[1400];
        tags_seen = true;
        snprintf(tag, sizeof tag, "%.*s", (int)(close - open - 2), open + 2);
        char *arg = strchr(tag, ':');
        if (arg)
            *arg++ = 0;
        else
            arg = tag + strlen(tag);
        if (!strcasecmp(tag, "name")) {
            if (name_ok(arg, false, words))
                snprintf(A.prof.ai_name, sizeof A.prof.ai_name, "%s", arg);
        } else if (!strcasecmp(tag, "user")) {
            if (name_ok(arg, true, NULL))
                snprintf(A.prof.user_name, sizeof A.prof.user_name, "%s", arg);
        } else if (!strcasecmp(tag, "learn")) {
            char *bar = strchr(arg, '|');
            if (bar) {
                *bar = 0;
                if (!strcasecmp(arg, "field") || !strcasecmp(arg, "key") || !*arg)
                    arg = "about"; /* the placeholder taken literally */
                /* "a|b" is two things learned, not one */
                for (char *save2, *v = strtok_r(bar + 1, "|", &save2); v; v = strtok_r(NULL, "|", &save2)) {
                    if (!is_placeholder(v) || strlen(v) > 24)
                        portrait_learn(&A.prof, arg, v);
                    LOG("learned %s: %s\n", arg, v);
                }
            }
        } else if (!strcasecmp(tag, "preview")) {
            int kind;
            color cs[4];
            char name[48], desc[140];
            if (!A.prof.has_room && parse_room_spec(arg, &kind, cs, name, sizeof name, desc, sizeof desc)) { /* their chosen room stays */
                if (desc[0] && nrooms < 6) {
                    /* described previews are rooms being offered */
                    snprintf(rooms[nrooms], sizeof rooms[0], "%s", arg);
                    nrooms++;
                } else {
                    set_room(kind, cs);
                }
            }
        } else if (!strcasecmp(tag, "done")) {
            /* A conversation that barely happened is sent back, once. */
            if (user_turns() < 6 && A.onboard_nudges < 1) {
                A.onboard_nudges++;
                pending_nudge = strdup("(Not yet: you have barely got to know them. Continue the arc: who they are, "
                                       "the rooms, their type and tempo, the two of you, then the mirror.)");
            } else {
                A.onboard_done = true;
            }
        } else if (!strcasecmp(tag, "pick")) {
            if (!picked_here) {
                parse_pick(arg);
                picked_here = A.pick.active;
            } else if (n_queued < 4) {
                snprintf(queued[n_queued++], sizeof queued[0], "%s", arg);
            }
        }
        p = close + 2;
    }
    sb_free(&plain);
    if (nrooms >= 2 && !A.pick.active) {
        sbuf r;
        sb_init(&r);
        sb_add(&r, "room");
        for (int i = 0; i < nrooms; i++) {
            for (char *q = rooms[i]; *q; q++)
                if (*q == '|')
                    *q = ':'; /* '|' separates the options here */
            sb_add(&r, "|");
            sb_add(&r, rooms[i]);
        }
        parse_pick(r.p);
        sb_free(&r);
    } else if (nrooms == 1) {
        int kind;
        color cs[4];
        char nm[48], ds[140];
        if (parse_room_spec(rooms[0], &kind, cs, nm, sizeof nm, ds, sizeof ds))
            set_room(kind, cs);
    }
    /* trim */
    char *s = shown.p ? shown.p : strdup("");
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n'))
        s[--n] = 0;
    while (n >= 2 && s[n - 1] == '.' && s[n - 2] == ' ') { /* "consider: ." left by a removed tag */
        n -= 2;
        s[n] = 0;
        while (n && s[n - 1] == ' ')
            s[--n] = 0;
    }
    if (n && s[n - 1] == ':')
        s[n - 1] = '.';
    char *start = s;
    while (*start == ' ' || *start == '\n')
        start++;
    /* collapse the blank runs removed tags leave behind */
    char *out = malloc(strlen(start) + 1), *o = out;
    for (const char *q = start; *q; q++) {
        if (*q == ' ' && o > out && o[-1] == ' ')
            continue;
        if (*q == '\n' && o > out + 1 && o[-1] == '\n' && o[-2] == '\n')
            continue;
        *o++ = *q;
    }
    *o = 0;
    strip_style_talk(out);
    free(s);
    return out;
}

/* Some models describe palettes in prose ("a stellar nebula (#0a1828,
 * #607d92, #2c3e50, #f1c40f)") instead of using a pick tag. Turn such
 * groups into a palette pick and take the codes out of the text. */
static void palettes_from_prose(char *text) {
    if (A.pick.active)
        return;
    struct pick pk;
    memset(&pk, 0, sizeof pk);
    snprintf(pk.kind, sizeof pk.kind, "palette");
    char *p = text;
    while ((p = strstr(p, "(#")) && pk.n < 6) {
        char *close = strchr(p, ')');
        if (!close)
            break;
        char codes[120];
        snprintf(codes, sizeof codes, "%.*s", (int)(close - p - 1), p + 1);
        color cs[4];
        if (!parse_colors(codes, cs)) {
            p = close;
            continue;
        }
        memcpy(pk.swatch[pk.n], cs, sizeof cs);
        pk.has_swatch[pk.n] = true;
        /* The name: the words just before the parenthesis, minus articles. */
        char *end = p, *start;
        while (end > text && end[-1] == ' ')
            end--;
        start = end;
        int wc = 0;
        while (start > text && wc < 3) {
            char ch = start[-1];
            if (ch == ',' || ch == '.' || ch == ':' || ch == '?' || ch == '!')
                break;
            if (ch == ' ')
                wc++;
            if (wc < 3)
                start--;
        }
        while (*start == ' ')
            start++;
        if (!strncasecmp(start, "a ", 2))
            start += 2;
        else if (!strncasecmp(start, "an ", 3))
            start += 3;
        snprintf(pk.label[pk.n], sizeof pk.label[0], "%.*s", (int)(end - start), start);
        if (pk.label[pk.n][0] >= 'a' && pk.label[pk.n][0] <= 'z')
            pk.label[pk.n][0] = (char)(pk.label[pk.n][0] - 32);
        memcpy(pk.value[pk.n], pk.label[pk.n], sizeof pk.label[0]);
        pk.n++;
        memmove(end, close + 1, strlen(close + 1) + 1);
        p = end;
    }
    if (pk.n >= 2) {
        pk.active = true;
        free_pick(&A.pick);
        A.pick = pk;
    }
}

static void say(const char *msg) {
    convo_add(&A.onboard, "user", msg);
    /* A first conversation, not an interview: after enough, move to the mirror. */
    if (user_turns() >= 9 && !mirror_asked && !A.onboard_done) {
        mirror_asked = true;
        convo_add(&A.onboard, "hidden",
                  "(You know enough now. If your name and how present to be aren't settled, settle them in one "
                  "message; otherwise go straight to the mirror.)");
    }
    convo_send(&A.onboard, onboarding_system(), 700);
}

static void record_pick(int i) {
    struct pick *pk = &A.pick;
    char msg[400];
    const char *k = pk->kind;
    if (!strcmp(k, "name")) {
        /* Names offered as a pick: for whoever is being named. */
        bool for_them = !A.prof.user_name[0] && user_turns() < 2;
        snprintf(for_them ? A.prof.user_name : A.prof.ai_name, sizeof A.prof.ai_name, "%s", pk->value[i]);
        snprintf(msg, sizeof msg, for_them ? "Call me %s." : "Call yourself %s.", pk->value[i]);
    } else if (!strcmp(k, "comfort")) {
        const char *v = pk->value[i];
        if (strcasestr(v, "larger") || strcasestr(v, "bigger")) {
            A.prof.text_scale = 1.25f;
            portrait_learn(&A.prof, "access", "larger text");
        } else if (strcasestr(v, "calm") || strcasestr(v, "movement") || strcasestr(v, "motion")) {
            A.prof.reduce_motion = true;
            portrait_learn(&A.prof, "access", "reduced motion");
        } else if (strcasestr(v, "contrast")) {
            A.prof.high_contrast = true;
            portrait_learn(&A.prof, "access", "strong contrast");
        }
        snprintf(msg, sizeof msg, "%s", v);
    } else if (!strcmp(k, "room")) {
        snprintf(A.prof.room, sizeof A.prof.room, "%s", pk->label[i]);
        snprintf(A.prof.palette_name, sizeof A.prof.palette_name, "%s", pk->label[i]);
        memcpy(A.prof.palette, pk->swatch[i], sizeof A.prof.palette);
        A.prof.has_palette = A.prof.has_room = true;
        A.prof.room_kind = pk->wp_kind[i];
        set_room(pk->wp_kind[i], pk->swatch[i]);
        char fact[240];
        snprintf(fact, sizeof fact, "%s%s%s", pk->label[i], pk->desc[i][0] ? ": " : "", pk->desc[i]);
        portrait_learn(&A.prof, "room", fact);
        snprintf(msg, sizeof msg, "%s. That one feels like home.", pk->label[i]);
    } else if (!strcmp(k, "type")) {
        snprintf(A.prof.type_font, sizeof A.prof.type_font, "%s", pk->font[i]);
        char said[160];
        snprintf(said, sizeof said, "your words in %s", pk->font[i]);
        portrait_learn(&A.prof, "type", said);
        snprintf(msg, sizeof msg, "That one, the %s.", i == 0 ? "first" : i == 1 ? "second" : i == 2 ? "third" : "last");
    } else if (!strcmp(k, "tempo")) {
        A.prof.tempo = pk->n > 1 ? (float)i / (float)(pk->n - 1) : 0.5f;
        char said[160];
        snprintf(said, sizeof said, "things move like %s", i == 0 ? "something almost asleep" : i == 1 ? "a slow breath" : "something awake");
        portrait_learn(&A.prof, "tempo", said);
        snprintf(msg, sizeof msg, "%s.", pk->value[i]);
    } else if (!strcmp(k, "palette")) {
        snprintf(A.prof.palette_name, sizeof A.prof.palette_name, "%s", pk->label[i]);
        memcpy(A.prof.palette, pk->swatch[i], sizeof A.prof.palette);
        A.prof.has_palette = true;
        snprintf(msg, sizeof msg, "I choose %s.", pk->label[i][0] ? pk->label[i] : "that one");
    } else {
        if (!strcmp(k, "mood"))
            snprintf(A.prof.mood, sizeof A.prof.mood, "%s", pk->value[i]);
        else if (!strcmp(k, "background"))
            snprintf(A.prof.background, sizeof A.prof.background, "%s", pk->value[i]);
        else if (!strcmp(k, "autonomy") || !strcmp(k, "presence")) {
            snprintf(A.prof.autonomy, sizeof A.prof.autonomy, "%s", pk->value[i]);
            portrait_learn(&A.prof, "presence", pk->value[i]);
        } else if (!strcmp(k, "redesign"))
            snprintf(A.prof.redesign, sizeof A.prof.redesign, "%s", pk->value[i]);
        snprintf(msg, sizeof msg, "%s", pk->value[i]);
    }
    LOG("picked %s %d: %s\n", k, i, pk->value[i]);
    if (n_answered < 12)
        snprintf(answered[n_answered++], sizeof answered[0], "%s", k);
    free_pick(pk);
    size_t cl = strlen(choices);
    snprintf(choices + cl, sizeof choices - cl, "%s%s", cl ? " " : "", msg);
    /* more picks from the same message: show the next one before replying */
    while (n_queued) {
        char next[1400];
        snprintf(next, sizeof next, "%s", queued[0]);
        memmove(queued, queued + 1, sizeof queued[0] * (size_t)(--n_queued));
        parse_pick(next);
        if (A.pick.active)
            return;
    }
    say(choices);
    choices[0] = 0;
}

static const char *last_of(const struct conversation *c, const char *role) {
    for (int i = c->n - 1; i >= 0; i--)
        if (!strcmp(c->msgs[i].role, role))
            return c->msgs[i].text;
    return NULL;
}

static int user_turns(void) {
    int n = 0;
    for (int i = 0; i < A.onboard.n; i++)
        n += !strcmp(A.onboard.msgs[i].role, "user");
    return n;
}

/* ---- drawing ---- */

static float tscale(void) { return A.prof.text_scale > 0.5f ? A.prof.text_scale : 1.0f; }
/* A light room gets dark words; everything else light ones. */
static bool light_room(void) { return room_set && room_mix > 0.5f && color_luma(room_wall.wp[0]) > 0.55f; }
static color ink(float a) {
    color base = light_room() ? RGBA(36, 28, 24, 255) : PRE_TEXT;
    return color_alpha(base, A.prof.high_contrast ? (a < 0.85f ? 0.92f : 1.0f) : a);
}
/* Quiet fills for chips and cards that read on either kind of room. */
static color wash(int alpha) { return light_room() ? RGBA(40, 30, 20, alpha * 3 / 4) : RGBA(255, 255, 255, alpha); }

/* Centred, film-like lines of the companion's words. Returns the height. */
static int centred_text(font *f, int y, int maxw, const char *s, color col, bool draw) {
    int lh = (int)(font_line_height(f) * 1.3f), y0 = y;
    while (*s) {
        int best = 0, i = 0;
        while (s[i] && s[i] != '\n') {
            int j = i;
            while (s[j] == ' ')
                j++;
            while (s[j] && s[j] != ' ' && s[j] != '\n')
                j++;
            if (text_width_n(f, s, j) > maxw && best)
                break;
            best = i = j;
        }
        if (!best)
            best = i ? i : (int)strlen(s);
        if (draw) {
            int w = text_width_n(f, s, best);
            draw_text_n(A.frame, f, (A.W - w) / 2, y, s, best, col);
        }
        y += lh;
        s += best;
        while (*s == ' ' || *s == '\n')
            s++;
    }
    return y - y0;
}

static void draw_backdrop(color a, color b) {
    color bg1 = A.prof.has_palette ? color_mix(PRE_BG, A.prof.palette[0], 0.3f) : PRE_BG;
    color bg2 = A.prof.has_palette ? color_mix(PRE_BG2, A.prof.palette[3], 0.3f) : PRE_BG2;
    if (!room_set || room_mix < 1) {
        if (A.prof.reduce_motion) {
            fill_vgradient(A.frame, (rect){0, 0, A.W, A.H}, bg1, bg2);
        } else {
            ui_backdrop(bg1, a, b, bg2);
        }
    }
    if (room_set) {
        room_mix += A.prof.reduce_motion ? 1.0f : 0.02f;
        if (room_mix > 1)
            room_mix = 1;
        static canvas *layer;
        if (!layer)
            layer = canvas_new(A.W, A.H);
        wallpaper_frame(layer, &room_wall, A.prof.reduce_motion ? 0 : (float)A.now / 1000.0f);
        /* fade the room in over the old backdrop */
        uint32_t *dst = A.frame->px, *src = layer->px;
        int k = (int)(room_mix * 256);
        for (int i = 0; i < A.W * A.H; i++) {
            uint32_t s = src[i], d = dst[i];
            uint32_t rb = ((((s & 0xff00ff) * (uint32_t)k) + ((d & 0xff00ff) * (uint32_t)(256 - k))) >> 8) & 0xff00ff;
            uint32_t g = ((((s & 0x00ff00) * (uint32_t)k) + ((d & 0x00ff00) * (uint32_t)(256 - k))) >> 8) & 0x00ff00;
            dst[i] = 0xff000000u | rb | g;
        }
        /* A soft band behind the words only, so the room keeps its light. */
        bool light = light_room();
        int peak = A.prof.high_contrast ? 170 : 105;
        color clear = light ? RGBA(255, 250, 244, 0) : RGBA(8, 6, 10, 0);
        color veil = light ? RGBA(255, 250, 244, peak) : RGBA(8, 6, 10, peak);
        fill_vgradient(A.frame, (rect){0, 230, A.W, 110}, clear, veil);
        fill_rect(A.frame, (rect){0, 340, A.W, 200}, veil);
        fill_vgradient(A.frame, (rect){0, 540, A.W, 130}, veil, clear);
    }
}

/* What the companion has understood, as quiet chips down the left edge.
 * Tapping one takes it back. */
static void draw_portrait(void) {
    if (!A.prof.nfacts)
        return;
    font *f = font_get("Inter", 13 * tscale());
    int y = 110, x = 36, maxw = 300;
    draw_text(A.frame, f, x, y - 26, "What I'm learning", ink(0.38f));
    for (int i = 0; i < A.prof.nfacts && y < A.H - 180; i++) {
        char t[300];
        snprintf(t, sizeof t, "%s", A.prof.facts[i].val);
        /* the newest part of an accumulated value is the interesting one */
        char *last = strrchr(t, ';');
        const char *show = last ? last + 2 : t;
        char cut[120];
        snprintf(cut, sizeof cut, "%s", show);
        while (text_width(f, cut) > maxw - 28 && strlen(cut) > 4)
            strcpy(cut + strlen(cut) - 4, "...");
        int w = text_width(f, cut) + 28, h = (int)(font_line_height(f) + 14);
        rect r = {x, y, w, h};
        bool hover = ui_hover(r);
        float age = (float)(A.now - A.screen_since) / 1000.0f;
        (void)age;
        fill_round(A.frame, r, h / 2.0f, wash(hover ? 34 : 16));
        draw_text(A.frame, f, x + 14, y + 7, cut, ink(hover ? 0.9f : 0.62f));
        if (hover) {
            font *s = font_get("Inter", 12 * tscale());
            draw_text(A.frame, s, x + w + 10, y + 8, "tap to forget", ink(0.4f));
            if (A.in.clicked) {
                char note[400];
                snprintf(note, sizeof note, "(They took back what you had noted under %s: \"%s\". Forget it.)",
                         A.prof.facts[i].key, A.prof.facts[i].val);
                convo_add(&A.onboard, "hidden", note);
                portrait_forget(&A.prof, i);
                return;
            }
        }
        y += h + 8;
    }
}

static void draw_pick(int y, color accent) {
    struct pick *pk = &A.pick;
    float s = tscale();
    font *body = font_get("Inter", 17 * s), *small = font_get("Inter", 13 * s);
    const char *k = pk->kind;
    bool room = !strcmp(k, "room"), pal = !strcmp(k, "palette"), type = !strcmp(k, "type"), tempo = !strcmp(k, "tempo");
    int cw = room ? 280 : pal ? 220 : type ? 300 : tempo ? 220 : 250;
    int ch = room ? 220 : pal ? 150 : type ? 130 : tempo ? 170 : 84;
    cw = (int)(cw * (s > 1 ? 1.1f : 1)), ch = (int)(ch * (s > 1 ? 1.15f : 1));
    int gap = 18, n = pk->n;
    if (n * cw + (n - 1) * gap > A.W - 80)
        cw = (A.W - 80 - (n - 1) * gap) / n;
    int total = n * cw + (n - 1) * gap, x = (A.W - total) / 2;
    static uint64_t last_log;
    bool log_now = A.now - last_log > 1000;
    if (log_now)
        last_log = A.now;
    for (int i = 0; i < n; i++) {
        rect r = {x + i * (cw + gap), y, cw, ch};
        if (log_now) {
            char name[32];
            snprintf(name, sizeof name, "pick-%s-%d", k, i);
            LOGRECT(name, r);
        }
        bool hover = ui_hover(r);
        fill_round(A.frame, r, 18, wash(hover ? 40 : 18));
        if (room) {
            rect th = {r.x + 10, r.y + 10, r.w - 20, r.h - 80};
            if (!pk->thumb[i] || pk->thumb[i]->w != th.w)
                pk->thumb[i] = canvas_new(th.w, th.h);
            design d;
            memset(&d, 0, sizeof d);
            d.wallpaper = (enum wallpaper_kind)pk->wp_kind[i];
            memcpy(d.wp, pk->swatch[i], sizeof d.wp);
            d.wp_seed = 5 + i;
            d.wp_intensity = 0.75f;
            d.motion = 0.5f;
            /* the hovered room comes alive; the others hold still */
            if (hover || !pk->thumb[i]->px[0])
                wallpaper_frame(pk->thumb[i], &d, hover && !A.prof.reduce_motion ? (float)A.now / 1000.0f : 3.0f);
            canvas_blit(A.frame, pk->thumb[i], th.x, th.y);
            stroke_round(A.frame, th, 10, 1, RGBA(255, 255, 255, 30));
            draw_text(A.frame, body, r.x + 16, r.y + r.h - 64, pk->label[i], ink(1));
            draw_text_wrapped(A.frame, small, (rect){r.x + 16, r.y + r.h - 38, r.w - 32, 36}, pk->desc[i], ink(0.6f), 1.15f);
        } else if (pal) {
            rect sw = {r.x + 16, r.y + 16, r.w - 32, 80};
            fill_round(A.frame, sw, 12, pk->swatch[i][0]);
            for (int c = 1; c < 4; c++)
                fill_circle(A.frame, (float)(sw.x + 30 + (c - 1) * 46), (float)(sw.y + sw.h / 2), 16, pk->swatch[i][c]);
            draw_text(A.frame, body, r.x + 18, r.y + 108, pk->label[i], ink(1));
        } else if (type) {
            font *tf = font_get(pk->font[i], 26 * s);
            draw_text_wrapped(A.frame, tf, (rect){r.x + 18, r.y + 18, r.w - 36, r.h - 44}, pk->sentence, ink(1), 1.1f);
            draw_text(A.frame, small, r.x + 18, r.y + r.h - 28, pk->font[i], ink(0.45f));
        } else if (tempo) {
            /* three orbs breathing at different speeds */
            float speed = n > 1 ? 0.25f + 1.6f * (float)i / (float)(n - 1) : 1;
            float ph = A.prof.reduce_motion ? 0 : sinf((float)A.now / 1000.0f * speed * 3.1416f);
            float rad = 30 + 7 * ph;
            ui_orb(A.frame, r.x + r.w / 2.0f, r.y + 64, rad, accent, color_mix(accent, PRE_TEXT, 0.5f), false);
            draw_text_centered(A.frame, body, (rect){r.x, r.y + r.h - 46, r.w, 30}, pk->label[i], ink(0.9f));
        } else {
            draw_text_wrapped(A.frame, body, (rect){r.x + 18, r.y + 16, r.w - 36, r.h - 20}, pk->label[i], ink(1), 1.2f);
        }
        if (hover)
            stroke_round(A.frame, r, 18, 1.4f, color_alpha(accent, 0.9f));
        if (hover && A.in.clicked) {
            record_pick(i);
            return;
        }
    }
}

void onboard_draw(void) {
    struct conversation *c = &A.onboard;
    if (convo_poll(c) && !c->error[0]) {
        struct msg *m = &c->msgs[c->n - 1];
        char *shown = apply_tags(m->text);
        free(m->text);
        palettes_from_prose(shown);
        m->text = shown;
        if (pending_nudge) {
            convo_add(c, "hidden", pending_nudge);
            free(pending_nudge);
            pending_nudge = NULL;
            convo_send(c, onboarding_system(), 700);
        }
        /* Smaller models sometimes answer with nothing (or only a tag).
         * Rather than leave the person facing silence, ask it to go on. */
        if (!shown[0] && !A.pick.active && !A.onboard_done && A.empty_replies < 2) {
            A.empty_replies++;
            convo_add(c, "hidden", "(Your last reply came through empty. Please continue the conversation with them.)");
            convo_send(c, onboarding_system(), 700);
        } else if (shown[0]) {
            A.empty_replies = 0;
        }
        portrait_save(&A.prof, STATE_DIR "/portrait.txt");
        voice_say(shown); /* heard as well as read */
    }
    color a = A.prof.has_palette ? A.prof.palette[1] : PRE_ACCENT;
    color b = A.prof.has_palette ? A.prof.palette[2] : RGBA(255, 214, 170, 255);
    if (room_set) {
        a = room_wall.wp[1];
        b = room_wall.wp[2];
    }
    draw_backdrop(a, b);

    float s = tscale();
    bool thinking = c->pending != NULL;
    ui_orb(A.frame, A.W / 2.0f, 190, 54, a, b, thinking && !A.prof.reduce_motion);
    font *name_f = font_get("Inter", 15 * s);
    if (A.prof.ai_name[0])
        draw_text_centered(A.frame, name_f, (rect){0, 262, A.W, 24}, A.prof.ai_name, ink(0.6f));

    font *big = font_get(A.prof.type_font[0] ? A.prof.type_font : "Fraunces", 25 * s);
    font *body = font_get("Inter", 18 * s), *small = font_get("Inter", 14 * s);
    const char *you = last_of(c, "user");
    /* The companion's last real words stay up, even if a reply came back
     * empty; before it has spoken, the OS asks about comfort itself. */
    const char *ai = NULL;
    for (int i = c->n - 1; i >= 0 && !ai; i--)
        if (!strcmp(c->msgs[i].role, "assistant") && c->msgs[i].text[strspn(c->msgs[i].text, " \n")])
            ai = c->msgs[i].text;
    if (!ai && A.pick.active && !strcmp(A.pick.kind, "comfort"))
        ai = "Before anything else: is this comfortable for you to read?";
    int y = 300, maxw = (int)(780 * (s > 1 ? 1.15f : 1));
    if (you && user_turns() > 0) {
        /* their last words, one quiet line */
        char line[400];
        snprintf(line, sizeof line, "“%s”", you);
        while (text_width(body, line) > maxw && strlen(line) > 8)
            strcpy(line + strlen(line) - 8, "...”");
        draw_text_centered(A.frame, body, (rect){0, y, A.W, 30}, line, ink(0.42f));
    }
    y += 48;
    /* the mind on this computer still loading its model: say so, kindly */
    bool waking = !strcmp(A.cfg.provider, "builtin") && mind_builtin_waking_for() > 0 && !mind_builtin_ready();
    if (thinking && waking) {
        char who[80], sub[160];
        snprintf(who, sizeof who, "%s is waking up", A.prof.ai_name[0] ? A.prof.ai_name : "Your AI");
        y += centred_text(big, y, maxw, who, ink(1), true);
        int t = mind_builtin_waking_for();
        snprintf(sub, sizeof sub, "Loading %s into memory  \xc2\xb7  %d:%02d", models_selected()->name, t / 60, t % 60);
        draw_text_centered(A.frame, small, (rect){0, y + 12, A.W, 22}, sub, ink(0.5f));
        if (t > 20)
            draw_text_centered(A.frame, small, (rect){0, y + 36, A.W, 22},
                               "The first time can take a few minutes. It's quicker after that.", ink(0.4f));
    } else if (thinking) {
        int dots = (int)((A.now / 400) % 4);
        char d[8] = "";
        for (int i = 0; i < dots; i++)
            strcat(d, ".");
        draw_text_centered(A.frame, big, (rect){0, y, A.W, 50}, d, ink(0.7f));
    } else if (c->error[0] && c->errors <= 1 && A.now - c->pending_since > 2500) {
        convo_send(c, onboarding_system(), 700); /* one quiet retry before bothering them */
    } else if (c->error[0]) {
        y += centred_text(body, y, maxw, c->error, RGBA(255, 170, 150, 255), true);
        if (ui_button((rect){A.W / 2 - 80, y + 20, 160, 46}, "Try again", body, color_alpha(a, 0.9f), RGBA(20, 14, 14, 255), 12, true))
            convo_send(c, onboarding_system(), 700);
    } else if (ai) {
        y += centred_text(big, y, maxw, ai, ink(1), true);
    }

    if (A.pick.active && !thinking) {
        /* every pick says what it is asking */
        static const char *const cap[][2] = {
            {"comfort", "Choose what feels right. You can change it any time."},
            {"room", "Which of these feels like home?"},
            {"type", "Which of these sounds like you?"},
            {"tempo", "How fast should things move?"},
            {"palette", "Which of these feels like yours?"},
        };
        for (size_t k = 0; k < sizeof cap / sizeof cap[0]; k++)
            if (!strcmp(A.pick.kind, cap[k][0])) {
                y += 14;
                draw_text_centered(A.frame, small, (rect){0, y, A.W, 24}, cap[k][1], ink(0.6f));
                y += 20;
            }
        draw_pick(y + 22, a);
    }
    draw_portrait();

    /* Reply box */
    rect box = {A.W / 2 - 420, A.H - 120, 840, (int)(56 * (s > 1 ? 1.15f : 1))};
    bool can_type = !thinking && !A.onboard_done;
    ui_textfield(box, &A.reply, can_type, A.pick.active ? "Choose above, or just tell me..." : "Say anything...", body,
                 wash(22), ink(1), ink(0.45f), 28);
    if (can_type && ui_type(&A.reply) && A.reply.len) {
        free_pick(&A.pick);
        n_queued = 0;
        if (choices[0]) { /* picks already made from this message count too */
            size_t cl = strlen(choices);
            snprintf(choices + cl, sizeof choices - cl, " %s", A.reply.buf);
            say(choices);
            choices[0] = 0;
        } else {
            say(A.reply.buf);
        }
        A.reply.len = 0;
        A.reply.buf[0] = 0;
    }
    draw_text_centered(A.frame, small, (rect){0, A.H - 52, A.W, 20},
                       user_turns() < 2 ? (voice_can_hear() ? "Press Enter to answer, or Ctrl+Space to talk. You can skip anything."
                                                            : "Press Enter to answer. You can skip anything.")
                                        : "",
                       ink(0.32f));
    /* talking instead of typing */
    if (voice_toggle_pressed() && !thinking) {
        if (voice_listening())
            voice_listen_stop();
        else
            voice_listen_start();
    }
    voice_indicator(A.W / 2, A.H - 160, a);
    char *heard = voice_poll_heard();
    if (heard && !thinking && !A.onboard_done) {
        free_pick(&A.pick);
        n_queued = 0;
        say(heard);
    }
    free(heard);

    /* Move on when the AI says so, or let the person decide after a while. */
    if (A.onboard_done && !thinking && A.now - A.screen_since > 1500) {
        static uint64_t done_at;
        if (!done_at)
            done_at = A.now;
        if (A.now - done_at > 3500) {
            done_at = 0;
            design_begin(NULL);
        }
    } else if (user_turns() >= 8 && !thinking) {
        rect skip = {A.W - 300, 40, 260, 44};
        if (ui_button(skip, "That's enough, make it now", small, wash(24), ink(1), 22, true))
            design_begin(NULL);
    }
}
