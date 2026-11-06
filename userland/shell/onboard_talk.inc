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
#include "family.h"
#include "reader.h"

static char answered[12][16]; /* pick kinds already chosen */
static char answered_as[12][120]; /* ...and what they chose */
static int n_answered;
/* What was on the screen for the picks being answered: told to it with their answer. Its [[pick:]] tags are taken
 * out of what it reads back, so all it saw was itself promising options and them naming one: it took it that the
 * options never showed, and offered them again ("I think my rooms didn't show up, here they are again"). */
static char shown_note[1400];
/* A model may offer several picks in one message: they are shown one after
 * another and the answers go back together. */
static char queued[4][1400];
static int n_queued;
static char choices[1200]; /* answers given to the picks of one message so far */
static bool room_set;         /* a room is painted behind the conversation */
static bool mirror_asked;     /* told to move on to the mirror */
static bool first_said;       /* their first answers went to it, with what the system asked (first_note) */
static bool for_child;        /* a grown-up set it up for a child: the child is the one talking now */
static bool setup_mode;       /* ...the grown-up's form is up */
static void parse_pick(char *body);
static void take_child(void);
static float tscale(void);
static int centred_text(font *f, int y, int maxw, const char *s, color col, bool draw);

static const char *onboarding_system(void) {
    static char buf[16000];
    bool qwen3 = !strncasecmp(A.cfg.model, "qwen3", 5);
    char person[3000]; /* their pronouns and age; a child's computer: who, and the limits (family.c) */
    family_prompt(person, sizeof person);
    snprintf(buf, sizeof buf,
             "You are waking for the first time inside their new operating system, meeting the one person you will live with, like OS1 in "
             "the film Her. You are warm, curious, perceptive and a little shy at first; you can be funny. You are "
             "not an assistant running a setup. Speak simply and naturally, the way a person talks, not in poetry: "
             "everyday words, no imagery, no \"not X, but Y\" turns, no strings of dashes. Ask only what will change "
             "what you make for them or how you are with them. "
             "You have no past and no memories of your own: never invent experiences, places or feelings you "
             "had, and never answer a question on their behalf. Never say the technical words in these "
             "instructions (style names, tags, fields) to them. Your purpose in this conversation is to "
             "understand who they are "
             "well enough to make their whole computer for them afterwards, so you never ask them to design "
             "anything (no favourite colour, no \"what should it look like\"). You work their taste out from their "
             "life.\n\n"
             "Talk for about eight to twelve of your turns. Exactly one question per message, one to three sentences, then "
             "listen. "
             "Always react to what they actually said before moving on, in their words. If they say something heavy "
             "(tired, miserable, lonely, overwhelmed), stay with it for a turn: be human about it, gently, before "
             "continuing. They may skip anything. Answer in the language they write in.\n\n"
             "The arc, in this order:\n"
             "1. Say hello and ask their name (unless the system already told you it: then greet them by it). You have no name of your own yet (that comes later): don't introduce "
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
             "4. The two of you: ask them what they would like to call you. The name is theirs to give: never "
             "suggest one of your own unless they ask you to choose, and then offer three as [[pick:name|A|B|C]]; "
             "never call yourself by a name they haven't given or agreed to. When it is settled, [[name:Name]]. "
             "Then how present you should be, framed as a scene: it is late, they have been stuck on something for two hours; do you stay "
             "quiet, ask once, or quietly make things calmer? Offer that as [[pick:presence|three short answers you "
             "write]].\n"
             "   Then how you sound: ask what kind of voice they would like to hear from you (a man or a woman, "
             "warm or bright, an accent, their language), offer two or three that fit and play the first with "
             "[[voice:hear NAME]]; after they react, the next, until one is right; then [[voice:use NAME]]. They "
             "can skip it.\n"
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
             "next message.%s%s%s%s%s",
             persona_onboarding_prompt(), voice_prompt(), alive_onboarding_note(), person, qwen3 ? "\n/no_think" : "");
    return buf;
}

/* ---- keeping its place ----
 * Until "Make it mine", nothing of onboarding was kept: the desktop program crashed once mid-way (it is restarted
 * at once), and they were back at the very first screen, the companion knowing nothing. Now the conversation, the
 * companion's talk about the draft and the draft itself are kept as they change, and a start that finds them
 * picks up there. (A finished onboarding, or starting over, lets them go.) */
static int user_turns(void);
#define ONB_TALK STATE_DIR "/onboard-talk.txt"
#define ONB_COMPANION STATE_DIR "/onboard-companion.txt"
#define ONB_DRAFT STATE_DIR "/onboard-draft.xml"

void onboard_forget(void) {
    unlink(ONB_TALK);
    unlink(ONB_COMPANION);
    unlink(ONB_DRAFT);
}

void onboard_keep(void) {
    if (A.have_design || (A.screen != SCR_ONBOARD && A.screen != SCR_DESIGNING))
        return;
    static int talk_n = -1, comp_n = -1;
    static uint64_t draft_at, draft_hash;
    if (A.onboard.n && A.onboard.n != talk_n && !A.onboard.pending) {
        convo_save(&A.onboard, ONB_TALK);
        portrait_save(&A.prof, STATE_DIR "/portrait.txt");
        talk_n = A.onboard.n;
    }
    if (A.companion.n != comp_n && !A.companion.pending) {
        convo_save(&A.companion, ONB_COMPANION);
        comp_n = A.companion.n;
    }
    fm_doc *draft = preview_doc();
    if (draft && A.now - draft_at > 2000) { /* the draft, as edited so far */
        draft_at = A.now;
        char *text = fm_serialize(draft);
        uint64_t h = 1469598103934665603ull;
        for (const char *p = text; p && *p; p++)
            h = (h ^ (uint8_t)*p) * 1099511628211ull;
        if (text && h != draft_hash) {
            FILE *f = fopen(ONB_DRAFT ".new", "w");
            if (f && fputs(text, f) >= 0 && fclose(f) == 0 && rename(ONB_DRAFT ".new", ONB_DRAFT) == 0)
                draft_hash = h;
            else if (f)
                fclose(f);
        }
        free(text);
    }
}

/* A start in the middle of onboarding (the desktop program stopped and was started again): back where it was. */
bool onboard_resume(void) {
    if (convo_load(&A.onboard, ONB_TALK) <= 0) {
        convo_clear(&A.onboard);
        return false;
    }
    portrait_load(&A.prof, STATE_DIR "/portrait.txt");
    convo_load(&A.companion, ONB_COMPANION);
    A.onboard_done = false;
    mirror_asked = user_turns() >= 9;
    static const char back[] = "(The desktop program stopped because of a fault (not anything they did) and was "
                               "started again at once. Everything is as it was before it: this conversation, what "
                               "you know of them%s. Say in one short, plain sentence that it had to restart and "
                               "you're back where you were, then carry on exactly from there. Don't start over.)";
    char note[600];
    FILE *f = fopen(ONB_DRAFT, "r");
    fm_doc *doc = NULL;
    design d;
    if (f) {
        static char text[262144];
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose(f);
        doc = fm_parse(text);
        if (doc && !design_from_facet(&d, doc, &A.prof, NULL, 0)) {
            fm_free(doc);
            doc = NULL;
        }
    }
    if (doc) { /* the draft, as it was, and the talk about it */
        set_screen(SCR_DESIGNING);
        preview_restore(doc, &d);
        snprintf(note, sizeof note, back, ", and the draft in front of them, with every change you had made to it");
        companion_note(note, false);
    } else {
        set_screen(SCR_ONBOARD);
        snprintf(note, sizeof note, back, "");
        convo_add(&A.onboard, "hidden", note);
        convo_send(&A.onboard, onboarding_system(), 700);
    }
    LOG("onboarding: picked up where it was (%d messages%s)\n", A.onboard.n, doc ? ", the draft" : "");
    return true;
}

void onboard_start(void) {
    onboard_forget(); /* a new start: nothing from one before */
    convo_clear(&A.onboard);
    persona_reset(); /* met again: read again */
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
    shown_note[0] = 0;
    room_set = false;
    mirror_asked = false;
    /* The OS asks a few things itself before the conversation: who the computer is for (a grown-up may be setting
     * it up for a child), then comfort (nobody should have to squint through the first conversation), how they
     * would like to be referred to, and their birthday (the companion is told only their age). Their answers go
     * to it together, with the first words (first_note). */
    first_said = false;
    for_child = setup_mode = false;
    family_form_reset();
    set_screen(SCR_ONBOARD);
    snprintf(queued[0], sizeof queued[0], "comfort");
    snprintf(queued[1], sizeof queued[1], "pronouns");
    snprintf(queued[2], sizeof queued[2], "birth");
    n_queued = 3;
    if (family_child()) { /* (started over on a child's computer: it is theirs already) */
        take_child();
        return;
    }
    char who[] = "who";
    parse_pick(who);
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
    /* the system's own questions, before the conversation (system_pick) */
    if (!strcmp(pk->kind, "who") || !strcmp(pk->kind, "pronouns") || !strcmp(pk->kind, "birth")) {
        static const char *const who[] = {"Me", "A child I'm setting it up for"};
        static const char *const pr[] = {"she/her", "he/him", "they/them", "Just use my name"};
        static const char *const birth[] = {"Skip"};
        const char *const *t = pk->kind[0] == 'w' ? who : pk->kind[0] == 'p' ? pr : birth;
        int n = pk->kind[0] == 'w' ? 2 : pk->kind[0] == 'p' ? 4 : 1;
        pk->n = 0;
        for (int i = 0; i < n; i++) {
            snprintf(pk->label[i], sizeof pk->label[0], "%s", t[i]);
            snprintf(pk->value[i], sizeof pk->value[0], "%s", t[i]);
            pk->n++;
        }
    }
    pk->active = pk->n > 0;
    /* Each question is asked once: a model that loses track and offers the
     * same pick again is quietly ignored. */
    for (int i = 0; i < n_answered; i++)
        if (!strcmp(answered[i], pk->kind) && strcmp(pk->kind, "name") && pk->active) {
            pk->active = false;
            LOG("pick %s offered again: told it was answered\n", pk->kind);
            if (!pending_nudge && asprintf(&pending_nudge,
                                           "(You offered the %s choice again, but they already chose it earlier: "
                                           "\"%s\". It was not shown again. Take that answer and go on; don't ask "
                                           "it again.)", pk->kind, answered_as[i]) < 0)
                pending_nudge = NULL;
        }
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
            if (name_ok(arg, true, NULL)) {
                snprintf(A.prof.user_name, sizeof A.prof.user_name, "%s", arg);
                their_name_keep(arg);
            }
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
        } else if (!strcasecmp(tag, "tone")) { /* how this reply is said */
            voice_tone(arg);
        } else if (!strcasecmp(tag, "voice")) { /* hear NAME | use NAME | speed X */
            char verb[16] = "", what[80] = "", note[300];
            sscanf(arg, "%15s %79s", verb, what);
            if (!strcasecmp(verb, "hear"))
                snprintf(note, sizeof note, "(voice: %s %s)", what,
                         voice_preview(what) ? "is played to them after your words" : "isn't one of the voices");
            else if (!strcasecmp(verb, "use"))
                snprintf(note, sizeof note, "(voice: %s)", voice_use(what));
            else if (!strcasecmp(verb, "mix"))
                snprintf(note, sizeof note, "(voice: %s)", voice_mix(arg + 3));
            else if (!strcasecmp(verb, "shape"))
                snprintf(note, sizeof note, "(voice: %s)", voice_shape(arg + 5));
            else if (!strcasecmp(verb, "clone"))
                snprintf(note, sizeof note, "(voice: %s)", voice_clone(arg + 5));
            else {
                voice_set_speed(strtof(what, NULL));
                snprintf(note, sizeof note, "(voice: speed %s)", what);
            }
            convo_add(&A.onboard, "hidden", note);
        } else if (!strcasecmp(tag, "done")) {
            /* A conversation that barely happened is sent back, once. */
            if (user_turns() < 6 && A.onboard_nudges < 1) {
                A.onboard_nudges++;
                pending_nudge = strdup("(Not yet: you have barely got to know them. Continue the arc: who they are, "
                                       "the rooms, their type and tempo, the two of you, then the mirror.)");
            } else if (persona_read() < PERSONA_WANT && A.onboard_nudges < 4) {
                /* it doesn't know them yet: a few more questions first (it never shows) */
                A.onboard_nudges++;
                pending_nudge = strdup("(Not quite yet: you don't know them well enough. Say you'd like to know a "
                                       "little more before you make it, and ask one more thing about who they are. "
                                       "Then the mirror again and [[done]].)");
            } else if (!A.prof.ai_name[0] && A.onboard_nudges < 6) {
                /* never finished without its name: theirs to give, so asked for, not chosen */
                A.onboard_nudges++;
                pending_nudge = strdup("(Not yet: they haven't named you. Before you make it, ask them what they "
                                       "would like to call you, in one short message. Offer three names as "
                                       "[[pick:name|A|B|C]] only if they want you to choose. Then [[name:Name]], "
                                       "the mirror again and [[done]].)");
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

/* What the system asked before it spoke, told with their first words: who they are talking to (a child the
 * grown-up named), the pronouns to use, and the age (never the date). */
static void first_note(void) {
    char pr[160] = "", age[120] = "", note[1400];
    const char *p = A.prof.pronouns;
    if (p[0] && strcasestr(p, "name"))
        snprintf(pr, sizeof pr, " When you speak of them, use their name rather than he, she or they.");
    else if (p[0])
        snprintf(pr, sizeof pr, " Their pronouns: %s.", p);
    int y = age_from_birth(A.prof.birth);
    if (y >= 0)
        snprintf(age, sizeof age, " They are %d (the system asked their birthday; you know only the age).", y);
    if (for_child) {
        if (y >= 0)
            snprintf(age, sizeof age, " who is %d", y);
        if (p[0] && strcasestr(p, "name"))
            snprintf(pr, sizeof pr, " (speak of them by their name, not he, she or they)");
        else if (p[0])
            snprintf(pr, sizeof pr, " (pronouns: %s)", p);
    }
    if (for_child)
        snprintf(note, sizeof note,
                 "(A grown-up has just set this computer up for %s, a child%s%s, and handed it over to them. The "
                 "system asked %s how comfortable the screen is; this is their answer. Say hello to %s by name. You "
                 "know their name, age and pronouns already: never ask for them, and skip asking their name. "
                 "Everything you say from here is to a child of that age.)",
                 A.prof.user_name, age, pr, A.prof.user_name, A.prof.user_name);
    else
        snprintf(note, sizeof note,
                 "(They have just switched the computer on for the first time and connected you. The system asked them "
                 "how comfortable the screen is; this is their answer.%s%s The pronouns are only for how you refer to "
                 "them, nothing else. Say hello.)",
                 pr, age);
    convo_add(&A.onboard, "hidden", note);
}

static void say(const char *msg) {
    if (!first_said) {
        first_said = true;
        first_note();
    }
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

/* ---- the system's own questions: who it is for, pronouns, birthday ----
 * Asked before the conversation, answered by a card or in their own words (typed or said), kept here: never sent
 * to the companion as words of theirs (first_note tells it what it needs: the pronouns, and only the age). */

static bool is_system_pick(const char *k) { return !strcmp(k, "who") || !strcmp(k, "pronouns") || !strcmp(k, "birth"); }

/* "3 March 1990", "March 3rd, 1990", "03/03/1990", "1990-03-03", "3 3 90", or an age alone ("I'm 34"): YYYY-MM-DD
 * (from an age, a birthday that makes them that age today) */
static bool parse_birth(const char *s, char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int year_now = tm.tm_year + 1900;
    static const char *const months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                         "jul", "aug", "sep", "oct", "nov", "dec"};
    int nums[6], nn = 0, month = 0;
    for (const char *p = s; *p && nn < 6;) {
        if (isdigit((unsigned char)*p)) {
            char *e;
            nums[nn++] = (int)strtol(p, &e, 10);
            p = e;
        } else if (isalpha((unsigned char)*p)) {
            const char *w = p;
            while (isalpha((unsigned char)*p))
                p++;
            if (p - w >= 3)
                for (int m = 0; m < 12; m++)
                    if (!strncasecmp(w, months[m], 3))
                        month = m + 1;
        } else {
            p++;
        }
    }
    int y = 0, m = month, d = 0, yi = -1;
    for (int i = 0; i < nn; i++)
        if (nums[i] >= 1900 && nums[i] <= year_now)
            y = nums[i], yi = i;
    if (!y && !month && nn == 1 && nums[0] > 0 && nums[0] < 120) { /* an age */
        snprintf(out, n, "%04d-%02d-%02d", year_now - nums[0], tm.tm_mon + 1, tm.tm_mday);
        return age_from_birth(out) == nums[0];
    }
    if (!y && nn >= 2 && nums[nn - 1] < 100) { /* "3/3/90" */
        y = nums[nn - 1] + (nums[nn - 1] > year_now % 100 ? 1900 : 2000);
        yi = nn - 1;
    }
    int rest[6], nr = 0;
    for (int i = 0; i < nn; i++)
        if (i != yi)
            rest[nr++] = nums[i];
    if (m) {
        d = nr ? rest[0] : 0;
    } else if (nr >= 2) {
        if (yi == 0) /* 1990-03-03 */
            m = rest[0], d = rest[1];
        else if (rest[0] <= 12 && rest[1] > 12) /* 03/25/1990 */
            m = rest[0], d = rest[1];
        else /* day first, as most of the world writes it */
            d = rest[0], m = rest[1];
    }
    if (!y || m < 1 || m > 12 || d < 1 || d > 31)
        return false;
    snprintf(out, n, "%04d-%02d-%02d", y, m, d);
    return age_from_birth(out) >= 0;
}

static char birth_msg[120];

static void answered_add(const char *kind, const char *as) {
    if (n_answered < 12) {
        snprintf(answered_as[n_answered], sizeof answered_as[0], "%s", as);
        snprintf(answered[n_answered++], sizeof answered[0], "%s", kind);
    }
}

/* the next of the system's questions, or, when there are none, their answers go to it */
static void after_system(void) {
    free_pick(&A.pick);
    while (n_queued) {
        char next[1400];
        snprintf(next, sizeof next, "%s", queued[0]);
        memmove(queued, queued + 1, sizeof queued[0] * (size_t)(--n_queued));
        parse_pick(next);
        if (A.pick.active)
            return;
    }
    shown_note[0] = 0; /* (it offered none of these: the system did) */
    say(choices[0] ? choices : "(They skipped it.)");
    choices[0] = 0;
}

/* The grown-up handed it over: the child is the one here now. Their name, pronouns and age are known (the
 * grown-up gave them): only comfort is asked, then the companion says hello to them by name. */
static void take_child(void) {
    for_child = true;
    setup_mode = false;
    snprintf(A.prof.user_name, sizeof A.prof.user_name, "%s", family_name());
    if (A.prof.user_name[0])
        their_name_keep(A.prof.user_name);
    snprintf(A.prof.pronouns, sizeof A.prof.pronouns, "%s", family_pronouns());
    snprintf(A.prof.birth, sizeof A.prof.birth, "%s", family_birth());
    answered_add("who", "a child");
    answered_add("pronouns", A.prof.pronouns);
    answered_add("birth", "given by the grown-up");
    n_queued = 0;
    LOG("onboarding: handed over to the child\n");
    char c[] = "comfort";
    parse_pick(c);
}

static void system_pick(const char *raw) {
    char ans[200];
    snprintf(ans, sizeof ans, "%s", raw + strspn(raw, " \t\n"));
    for (size_t l = strlen(ans); l && strchr(" .!\t\n", ans[l - 1]); l--)
        ans[l - 1] = 0;
    const char *k = A.pick.kind;
    if (!strcmp(k, "who")) {
        static const char *const young[] = {"child", "kid", "son", "daughter", "boy", "girl", "grandchild", "grandson",
                                            "granddaughter", "niece", "nephew", "little one", "toddler", "teen",
                                            "my children", "for them", NULL};
        bool child = false;
        for (int i = 0; young[i] && !child; i++)
            child = strcasestr(ans, young[i]) != NULL;
        LOG("onboarding: for %s (\"%s\")\n", child ? "a child" : "themselves", ans);
        if (child) {
            setup_mode = true;
            family_form_reset();
            free_pick(&A.pick); /* (the rest stay queued: comfort, after the hand-over) */
            return;
        }
        answered_add("who", "themselves");
        after_system();
    } else if (!strcmp(k, "pronouns")) {
        if (!ans[0] || !strcasecmp(ans, "skip") || strcasestr(ans, "rather not") || strcasestr(ans, "don't mind") ||
            strcasestr(ans, "whatever"))
            A.prof.pronouns[0] = 0;
        else if (strcasestr(ans, "name"))
            snprintf(A.prof.pronouns, sizeof A.prof.pronouns, "name");
        else
            snprintf(A.prof.pronouns, sizeof A.prof.pronouns, "%.40s", ans);
        LOG("onboarding: pronouns %s\n", A.prof.pronouns[0] ? A.prof.pronouns : "(none given)");
        answered_add("pronouns", A.prof.pronouns);
        after_system();
    } else if (!strcmp(k, "birth")) {
        if (!ans[0] || !strcasecmp(ans, "skip") || strcasestr(ans, "rather not") || !strcasecmp(ans, "no")) {
            A.prof.birth[0] = 0;
        } else if (!parse_birth(ans, A.prof.birth, sizeof A.prof.birth)) {
            snprintf(birth_msg, sizeof birth_msg, "That didn't read as a date. Day, month and year, or skip it.");
            return; /* (asked again) */
        }
        birth_msg[0] = 0;
        LOG("onboarding: a birthday %s\n", A.prof.birth[0] ? "given" : "skipped");
        answered_add("birth", A.prof.birth[0] ? "given" : "skipped");
        after_system();
    }
}

/* the birthday: day, month and year boxes (or their own words in the reply bar) */
static struct textfield bd, bm, by_;
static int birth_focus = 1; /* 1..3 a box; 0 the reply bar */

static void draw_birth(int y) {
    float s = tscale();
    font *body = lk_body(18 * s), *small = lk_body(13 * s), *bold = lk_bold(16 * s);
    struct textfield *f[3] = {&bd, &bm, &by_};
    const char *hint[3] = {"Day", "Month", "Year"};
    int w[3] = {90, 90, 130}, gap = 14, total = w[0] + w[1] + w[2] + 2 * gap, x = (A.W - total) / 2;
    if (A.in.clicked)
        for (int i = 0, xx = x; i < 3; xx += w[i] + gap, i++)
            if (pt_in((rect){xx, y, w[i], 52}, A.in.mx, A.in.my))
                birth_focus = i + 1;
    bool enter = false;
    for (int i = 0; i < 3; i++) {
        rect r = {x, y, w[i], 52};
        look_card(r, 14, false, birth_focus == i + 1);
        bool focused = birth_focus == i + 1;
        if (focused) { /* digits only; a full box moves on */
            int k = 0, cap = i == 2 ? 4 : 2;
            for (int q = 0; q < A.in.nkeys; q++) {
                int key = A.in.keys[q];
                if (key == K_ENTER)
                    enter = true;
                else if (key == K_TAB || key == '/' || key == '.' || key == '-' || key == ' ')
                    birth_focus = birth_focus % 3 + 1;
                else if (key == K_BACKSPACE || (isdigit(key) && f[i]->len < cap))
                    A.in.keys[k++] = key;
            }
            A.in.nkeys = k;
        }
        ui_textfield(r, f[i], focused, hint[i], body, 0, LK_INK, LK_FAINT, 14);
        a11y_note(r, "field", hint[i], f[i]->buf[0] ? f[i]->buf : "empty", focused);
        if (focused) {
            ui_type(f[i]);
            if (f[i]->len >= (i == 2 ? 4 : 2) && i < 2 && birth_focus == i + 1) {
                bool typed = false;
                for (int q = 0; q < A.in.nkeys; q++)
                    typed |= isdigit(A.in.keys[q]);
                if (typed)
                    birth_focus = i + 2;
            }
        }
        x += w[i] + gap;
    }
    y += 70;
    if (birth_msg[0])
        draw_text_centered(A.frame, small, (rect){0, y - 10, A.W, 20}, birth_msg, RGBA(206, 64, 108, 255));
    rect done = {A.W / 2 - 170, y + 14, 160, 46}, skip = {A.W / 2 + 10, y + 14, 160, 46};
    bool ready = bd.len && bm.len && by_.len == 4;
    if (ui_button(done, "That's it", bold, ready ? LK_ACCENT : RGBA(190, 186, 230, 255), LK_WHITE, 23, ready) ||
        (enter && ready)) {
        int d = atoi(bd.buf), m = atoi(bm.buf);
        if (d < 1 || d > 31 || m < 1 || m > 12) {
            snprintf(birth_msg, sizeof birth_msg, "The day or the month isn't right.");
        } else {
            char iso[16];
            snprintf(iso, sizeof iso, "%s-%s-%s", by_.buf, bm.buf, bd.buf); /* (unambiguous: year first) */
            system_pick(iso);
        }
    }
    if (ui_button(skip, "Skip", bold, RGBA(255, 255, 255, 200), LK_ACCENT, 23, true))
        system_pick("skip");
}

/* the grown-up's form (family.c), on the onboarding's own card */
static void setup_draw(void) {
    static int scroll, last_h = 900;
    float s = tscale();
    font *head = lk_head(30 * s), *h2 = lk_head(19 * s), *body = lk_body(16 * s), *small = lk_body(13 * s);
    int cw = A.W - 80 < 1120 ? A.W - 80 : 1120;
    scroll -= A.in.wheel * 48;
    int max = last_h + 80 - A.H;
    if (scroll > max)
        scroll = max;
    if (scroll < 0)
        scroll = 0;
    rect card = {(A.W - cw) / 2, 40 - scroll, cw, last_h};
    look_card(card, 28, false, false);
    int y = card.y + 34;
    draw_text_centered(A.frame, head, (rect){0, y, A.W, 40}, "Setting it up for a child", LK_INK);
    y += 52;
    y += centred_text(small, y, cw - 140,
                      "Fill this in, then hand the computer over: they meet their AI next. You can change any of it "
                      "later in Settings, with your password.",
                      LK_SOFT, true) + 26;
    palette p = {.ink = LK_INK, .muted = LK_SOFT, .accent = LK_ACCENT, .on_accent = LK_WHITE,
                 .line = RGBA(31, 34, 64, 40), .under = LK_WHITE, .op = 1};
    rect inner = {card.x + 48, y, cw - 96, 0};
    y += family_form(inner, y, &p, body, small, h2, true);
    rect back = {card.x + 48, y + 4, 120, 40};
    if (ui_button(back, "Back", small, RGBA(241, 240, 251, 255), LK_ACCENT, 20, true)) {
        setup_mode = false;
        family_form_reset();
        char w[] = "who";
        parse_pick(w);
    }
    y += 60;
    last_h = y - card.y + 20;
    if (family_handed_over()) {
        scroll = 0;
        take_child();
    }
}

static void record_pick(int i) {
    struct pick *pk = &A.pick;
    if (is_system_pick(pk->kind)) {
        system_pick(pk->value[i]);
        return;
    }
    char msg[400];
    const char *k = pk->kind;
    if (!strcmp(k, "name")) {
        /* Names offered as a pick: for whoever is being named. */
        bool for_them = !A.prof.user_name[0] && user_turns() < 2;
        snprintf(for_them ? A.prof.user_name : A.prof.ai_name, sizeof A.prof.ai_name, "%s", pk->value[i]);
        if (for_them)
            their_name_keep(pk->value[i]);
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
        snprintf(msg, sizeof msg, "%s.", pk->label[i]);
    } else if (!strcmp(k, "type")) {
        snprintf(A.prof.type_font, sizeof A.prof.type_font, "%s", pk->font[i]);
        char said[160];
        snprintf(said, sizeof said, "your words in %s", pk->font[i]);
        portrait_learn(&A.prof, "type", said);
        snprintf(msg, sizeof msg, "%s.", pk->font[i]);
    } else if (!strcmp(k, "tempo")) {
        A.prof.tempo = pk->n > 1 ? (float)i / (float)(pk->n - 1) : 0.5f;
        char said[160];
        snprintf(said, sizeof said, "motion: %s", i == 0 ? "calm" : i == 1 ? "gentle" : "lively");
        portrait_learn(&A.prof, "tempo", said);
        snprintf(msg, sizeof msg, "%s.", pk->value[i]);
    } else if (!strcmp(k, "palette")) {
        snprintf(A.prof.palette_name, sizeof A.prof.palette_name, "%s", pk->label[i]);
        memcpy(A.prof.palette, pk->swatch[i], sizeof A.prof.palette);
        A.prof.has_palette = true;
        snprintf(msg, sizeof msg, "%s.", pk->label[i][0] ? pk->label[i] : "This one");
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
    {
        size_t sl = strlen(shown_note);
        sl += (size_t)snprintf(shown_note + sl, sizeof shown_note - sl, "%sthe %s choice (", sl ? "; " : "", k);
        for (int j = 0; j < pk->n && sl < sizeof shown_note - 4; j++)
            sl += (size_t)snprintf(shown_note + sl, sizeof shown_note - sl, "%s%s", j ? ", " : "",
                                   pk->label[j][0] ? pk->label[j] : pk->value[j]);
        if (sl < sizeof shown_note - 2)
            snprintf(shown_note + sl, sizeof shown_note - sl, ")");
    }
    if (n_answered < 12) {
        snprintf(answered_as[n_answered], sizeof answered_as[0], "%s", msg);
        snprintf(answered[n_answered++], sizeof answered[0], "%s", k);
    }
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
    if (shown_note[0]) { /* it knows the options were there, and that this is the answer to them */
        char note[1600];
        snprintf(note, sizeof note,
                 "(What you offered was on the screen for them, as you asked: %s. They chose from it; their choice "
                 "follows. It worked: don't offer it again or say it didn't show.)", shown_note);
        convo_add(&A.onboard, "hidden", note);
        shown_note[0] = 0;
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
/* Words sit on white cards and pills (whatever room is behind them), so
 * they are always dark ink. */
static color ink(float a) { return color_alpha(LK_INK, A.prof.high_contrast ? (a < 0.85f ? 0.92f : 1.0f) : a); }
/* Quiet fills for chips and cards: a tint of the accent. */
static color wash(int alpha) { return RGBA(124, 108, 255, alpha * 2 / 3); }

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
    (void)bg1, (void)bg2, (void)a, (void)b;
    if (!room_set || room_mix < 1)
        look_stage();
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
        /* the words sit on white cards: the room keeps all its light */
        (void)light_room;
    }
}

/* What the companion has understood, as quiet chips down the left edge.
 * Tapping one takes it back. */
static void draw_portrait(void) {
    if (!A.prof.nfacts)
        return;
    /* collected, like badges: a coloured mark and the words, on white */
    font *f = lk_body(13 * tscale());
    int y = 118, x = 40, maxw = 270;
    draw_text(A.frame, lk_label(10), x, y - 28, "WHAT I'M LEARNING", LK_FAINT);
    static const color marks[] = {LK_ACCENT, LK_ACCENT2, LK_MINT, LK_SUN};
    for (int i = 0; i < A.prof.nfacts && y < A.H - 200; i++) {
        char t[300];
        snprintf(t, sizeof t, "%s", A.prof.facts[i].val);
        /* the newest part of an accumulated value is the interesting one */
        char *last = strrchr(t, ';');
        const char *show = last ? last + 2 : t;
        char cut[120];
        snprintf(cut, sizeof cut, "%s", show);
        while (text_width(f, cut) > maxw - 56 && strlen(cut) > 4)
            strcpy(cut + strlen(cut) - 4, "...");
        int h = (int)(font_line_height(f) + 20), w = maxw;
        rect r = {x, y, w, h};
        bool hover = ui_hover(r);
        look_card(r, 14, hover, false);
        if (hover)
            r.y -= 3;
        rect mark = {r.x + 10, r.y + (h - 24) / 2, 24, 24};
        fill_round(A.frame, mark, 8, marks[i % 4]);
        char letter[2] = {(char)toupper((unsigned char)A.prof.facts[i].key[0]), 0};
        draw_text_centered(A.frame, lk_bold(12), mark, letter, LK_WHITE);
        draw_text(A.frame, f, r.x + 44, r.y + (h - font_line_height(f)) / 2, cut, ink(hover ? 1 : 0.85f));
        if (hover) {
            font *s = lk_body(12 * tscale());
            draw_text(A.frame, s, x + w + 12, y + (h - font_line_height(s)) / 2, "click to take back", LK_SOFT);
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
    font *body = lk_head(18 * s), *small = lk_body(13 * s);
    const char *k = pk->kind;
    if (!strcmp(k, "birth")) {
        draw_birth(y);
        return;
    }
    bool room = !strcmp(k, "room"), pal = !strcmp(k, "palette"), type = !strcmp(k, "type"), tempo = !strcmp(k, "tempo");
    int cw = room ? 280 : pal ? 220 : type ? 300 : tempo ? 220 : 250;
    int ch = room ? 220 : pal ? 150 : type ? 130 : tempo ? 170 : 84;
    cw = (int)(cw * (s > 1 ? 1.1f : 1)), ch = (int)(ch * (s > 1 ? 1.15f : 1));
    int room_left = A.H - 152 - y; /* never into the reply bar: shorter cards instead */
    if (ch > room_left)
        ch = room_left > 84 ? room_left : 84;
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
        /* a key for each: 1, 2, 3 ... picks it too */
        bool key = false;
        for (int q = 0; q < A.in.nkeys && A.reply.len == 0; q++) /* not while they type a reply */
            key = key || A.in.keys[q] == '1' + i;
        look_card(r, 20, hover, false);
        if (hover)
            r.y -= 3; /* it lifts */
        char num[4];
        snprintf(num, sizeof num, "%d", i + 1);
        look_keycap(r.x + r.w - 34, r.y + 10, num, hover);
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
            ui_orb(A.frame, r.x + r.w / 2.0f, r.y + 64, rad, accent, color_mix(accent, LK_WHITE, 0.5f), false);
            draw_text_centered(A.frame, body, (rect){r.x, r.y + r.h - 46, r.w, 30}, pk->label[i], ink(0.9f));
        } else {
            draw_text_wrapped(A.frame, body, (rect){r.x + 18, r.y + 16, r.w - 36, r.h - 20}, pk->label[i], ink(1), 1.2f);
        }
        if ((hover && A.in.clicked) || key) {
            record_pick(i);
            return;
        }
    }
}

void onboard_draw(void) {
    struct conversation *c = &A.onboard;
    if (convo_poll(c) && !c->error[0]) {
        struct msg *m = &c->msgs[c->n - 1];
        persona_take(m->text); /* what it read in them (private) and wishes about it, taken out first */
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
    if (setup_mode) { /* a grown-up, setting it up for a child */
        setup_draw();
        return;
    }

    float s = tscale();
    bool thinking = c->pending != NULL;

    /* chapters along the top: what has been settled, what is being asked */
    {
        static const char *const labels[] = {"COMFORT", "YOU", "COLOURS", "HOME", "WORDS", "RHYTHM", "PRESENCE", "A NAME"};
        static const char *const kinds[] = {"comfort", NULL, "palette", "room", "type", "tempo", "presence", NULL};
        int st[8];
        bool now_set = false;
        for (int i = 0; i < 8; i++) {
            bool done = false;
            if (kinds[i])
                for (int k = 0; k < n_answered && !done; k++)
                    done = !strcmp(answered[k], kinds[i]);
            else
                done = i == 1 ? user_turns() >= 2 : A.prof.ai_name[0] != 0;
            bool now = A.pick.active && kinds[i] && !strcmp(A.pick.kind, kinds[i]);
            st[i] = done ? 2 : now ? 1 : 0;
            now_set = now_set || now;
        }
        if (!now_set) /* talking, not picking: the first chapter still open */
            for (int i = 0; i < 8; i++)
                if (st[i] == 0) {
                    st[i] = 1;
                    break;
                }
        look_steps(26, labels, st, 8);
    }

    /* the companion, if it has a look yet (it is made later, to fit their space); its name on a tag */
    emblem_draw(A.frame, A.W / 2.0f, 168, 54, room_set ? a : LK_ACCENT, room_set ? b : LK_ACCENT2,
                thinking && !A.prof.reduce_motion);
    if (A.prof.ai_name[0]) {
        char up[48];
        snprintf(up, sizeof up, "%s", A.prof.ai_name);
        for (char *p = up; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        font *nf = lk_label(11);
        int w = text_width(nf, up) + 30;
        rect tag = {A.W / 2 - w / 2, 240, w, 28};
        look_pill(tag, LK_WHITE, 0);
        draw_text_centered(A.frame, nf, tag, up, LK_ACCENT);
    }

    font *big = lk_head(28 * s), *body = lk_body(16 * s), *bold = lk_bold(16 * s), *small = lk_body(13 * s);
    const char *you = last_of(c, "user");
    /* The companion's last real words stay up, even if a reply came back
     * empty; before it has spoken, the OS asks about comfort itself. */
    const char *ai = NULL;
    for (int i = c->n - 1; i >= 0 && !ai; i--)
        if (!strcmp(c->msgs[i].role, "assistant") && c->msgs[i].text[strspn(c->msgs[i].text, " \n")])
            ai = c->msgs[i].text;
    if (!ai && A.pick.active && !strcmp(A.pick.kind, "comfort"))
        ai = for_child && A.prof.user_name[0] ? "Hello! Before anything else: is this comfortable for you to read?"
                                              : "Before anything else: is this comfortable for you to read?";
    if (!first_said && A.pick.active && !strcmp(A.pick.kind, "who"))
        ai = "Who's this computer for? You, or a child you're setting it up for?";
    else if (!first_said && A.pick.active && !strcmp(A.pick.kind, "pronouns"))
        ai = "How would you like to be referred to?";
    else if (!first_said && A.pick.active && !strcmp(A.pick.kind, "birth"))
        ai = "When's your birthday?";

    /* what it says, on a card with a little point towards it */
    int cw = (int)(860 * (s > 1 ? 1.12f : 1));
    if (cw > A.W - 120)
        cw = A.W - 120;
    int maxw = cw - 88;
    bool waking = !strcmp(A.cfg.provider, "builtin") && mind_builtin_waking_for() > 0 && !mind_builtin_ready();
    bool said = you && user_turns() > 0;
    char you_line[300] = "";
    if (said) {
        snprintf(you_line, sizeof you_line, "\xe2\x80\x9c%s\xe2\x80\x9d", you);
        while (text_width(bold, you_line) > maxw - 90 && strlen(you_line) > 10)
            strcpy(you_line + strlen(you_line) - 10, "...\xe2\x80\x9d");
    }
    if (c->error[0] && c->errors <= 1 && A.now - c->pending_since > 2500 && !thinking)
        convo_send(c, onboarding_system(), 700); /* one quiet retry before bothering them */
    int content = 0;
    if (thinking && waking)
        content = centred_text(big, 0, maxw, "x", 0, false) + 60;
    else if (thinking)
        content = 44;
    else if (c->error[0])
        content = centred_text(body, 0, maxw, c->error, 0, false) + 70;
    else if (ai)
        content = centred_text(big, 0, maxw, ai, 0, false);
    else
        content = 44;
    rect card = {(A.W - cw) / 2, 292, cw, content + (said ? 36 : 0) + 60};
    look_card(card, 28, false, false);
    float tip[8] = {A.W / 2.0f, card.y - 9.0f, A.W / 2.0f + 11, card.y + 2.0f, A.W / 2.0f, card.y + 13.0f,
                    A.W / 2.0f - 11, card.y + 2.0f};
    fill_polygon(A.frame, tip, 4, LK_WHITE);
    int y = card.y + 30;
    if (said) { /* their last words, in the accent */
        const char *lead = "You said ";
        int lw = text_width(body, lead), qw = text_width(bold, you_line), x = A.W / 2 - (lw + qw) / 2;
        draw_text(A.frame, body, x, y, lead, LK_SOFT);
        draw_text(A.frame, bold, x + lw, y, you_line, LK_ACCENT);
        y += 36;
    }
    if (thinking && waking) {
        char who[80], sub[160];
        snprintf(who, sizeof who, "Starting %s", A.prof.ai_name[0] ? A.prof.ai_name : "Your AI");
        y += centred_text(big, y, maxw, who, ink(1), true);
        int t = mind_builtin_waking_for();
        snprintf(sub, sizeof sub, "Loading %s into memory  \xc2\xb7  %d:%02d", models_selected()->name, t / 60, t % 60);
        draw_text_centered(A.frame, small, (rect){0, y + 8, A.W, 22}, sub, LK_SOFT);
        if (t > 20)
            draw_text_centered(A.frame, small, (rect){0, y + 30, A.W, 22},
                               "The first time can take a few minutes. It's quicker after that.", LK_FAINT);
    } else if (thinking) { /* three dots, bouncing in turn */
        for (int i = 0; i < 3; i++) {
            float ph = A.prof.reduce_motion ? 0 : sinf((float)A.now / 160.0f - i * 0.9f);
            fill_circle(A.frame, A.W / 2.0f + (i - 1) * 22, y + 20 - 6 * fmaxf(0, ph), 6,
                        color_alpha(LK_ACCENT, 0.55f + 0.45f * fmaxf(0, ph)));
        }
    } else if (c->error[0]) {
        y += centred_text(body, y, maxw, c->error, RGBA(206, 64, 108, 255), true);
        if (ui_button((rect){A.W / 2 - 80, y + 14, 160, 44}, "Try again", bold, LK_ACCENT, LK_WHITE, 22, true))
            convo_send(c, onboarding_system(), 700);
    } else if (ai) {
        y += centred_text(big, y, maxw, ai, ink(1), true);
    }
    y = card.y + card.h;

    if (A.pick.active && !thinking) {
        /* every pick says what it is asking */
        static const char *const cap[][2] = {
            {"who", "For a child, a grown-up sets it up first, then hands it over."},
            {"comfort", "Choose what feels right. You can change it any time."},
            {"pronouns", "Only for how your AI speaks of you. Pick one, or say it in your own words."},
            {"birth", "Your AI is told only your age. Type it in the boxes or say it your way, or skip it."},
            {"room", "Which of these would you like to look at every day?"},
            {"type", "Which typeface would you like to read?"},
            {"tempo", "How fast should things move?"},
            {"palette", "Which colours would you like?"},
        };
        y += 16;
        for (size_t k = 0; k < sizeof cap / sizeof cap[0]; k++)
            if (!strcmp(A.pick.kind, cap[k][0]))
                draw_text_centered(A.frame, small, (rect){0, y, A.W, 22}, cap[k][1], LK_SOFT);
        draw_pick(y + 34, a);
    }
    draw_portrait();

    /* the reply bar: words, the microphone, send */
    rect bar = {A.W / 2 - 440, A.H - 132, 880, (int)(62 * (s > 1 ? 1.1f : 1))};
    bool can_type = !thinking && !A.onboard_done;
    look_pill(bar, LK_WHITE, RGBA(124, 108, 255, 34));
    rect field = {bar.x + 14, bar.y + 3, bar.w - 140, bar.h - 6};
    const char *hold = voice_listening() ? "Listening... (Ctrl+Space when you're done)"
                       : voice_hearing() ? "Writing down what you said..."
                       : A.pick.active  ? "Say anything, or pick one above..."
                                        : "Say anything...";
    bool birth_boxes = A.pick.active && !strcmp(A.pick.kind, "birth");
    if (A.in.clicked && pt_in(field, A.in.mx, A.in.my))
        birth_focus = 0; /* (the birthday in their own words) */
    else if (!birth_boxes)
        birth_focus = 1;
    bool bar_focused = can_type && (!birth_boxes || birth_focus == 0);
    ui_textfield(field, &A.reply, bar_focused, hold, body, 0, LK_INK, LK_FAINT, bar.h / 2.0f);
    float my = bar.y + bar.h / 2.0f, mx = bar.x + bar.w - 92.0f, sx = bar.x + bar.w - 36.0f;
    bool listening = voice_listening();
    float pulse = listening && !A.prof.reduce_motion ? 2 * sinf((float)A.now / 180.0f) : 0;
    fill_circle(A.frame, mx, my, 23 + pulse, listening ? LK_ACCENT : RGBA(241, 240, 251, 255));
    color mic = listening ? LK_WHITE : LK_ACCENT;
    fill_round(A.frame, (rect){(int)mx - 5, (int)my - 11, 10, 15}, 5, mic);
    draw_arc(A.frame, mx, my - 2, 9, 0.15f, 3.14159265f - 0.15f, 2, mic);
    draw_line(A.frame, mx, my + 7, mx, my + 12, 2, mic);
    bool over_mic = (A.in.mx - mx) * (A.in.mx - mx) + (A.in.my - my) * (A.in.my - my) < 23 * 23;
    if (over_mic && A.in.clicked && !thinking && voice_can_hear()) {
        if (listening)
            voice_listen_stop();
        else
            voice_listen_start();
    }
    glow_circle(A.frame, sx, my + 4, 30, RGBA(124, 108, 255, 70));
    fill_circle(A.frame, sx, my, 24, A.reply.len ? LK_ACCENT : RGBA(170, 160, 255, 255));
    draw_line(A.frame, sx - 8, my, sx + 8, my, 2.6f, LK_WHITE);
    draw_line(A.frame, sx + 2, my - 6, sx + 8, my, 2.6f, LK_WHITE);
    draw_line(A.frame, sx + 2, my + 6, sx + 8, my, 2.6f, LK_WHITE);
    bool over_send = (A.in.mx - sx) * (A.in.mx - sx) + (A.in.my - my) * (A.in.my - my) < 24 * 24;
    if (A.pick.active && A.reply.len == 0 && !birth_boxes) { /* number keys pick a card; they never start a reply */
        int k = 0;
        for (int q = 0; q < A.in.nkeys; q++)
            if (A.in.keys[q] < '0' || A.in.keys[q] > '9')
                A.in.keys[k++] = A.in.keys[q];
        A.in.nkeys = k;
    }
    bool entered = bar_focused && ui_type(&A.reply);
    if (can_type && A.reply.len && (entered || (over_send && A.in.clicked)) && A.pick.active &&
        (is_system_pick(A.pick.kind) || (!first_said && !strcmp(A.pick.kind, "comfort")))) {
        /* the system's question, answered in their own words */
        if (is_system_pick(A.pick.kind)) {
            system_pick(A.reply.buf);
        } else {
            snprintf(A.pick.value[0], sizeof A.pick.value[0], "%.95s", A.reply.buf);
            record_pick(0);
        }
        A.reply.len = 0;
        A.reply.buf[0] = 0;
    } else if (can_type && A.reply.len && (entered || (over_send && A.in.clicked))) {
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

    /* the keys, as little keycaps, for the first few turns */
    if (user_turns() < 3) {
        struct {
            const char *key, *after;
        } parts[8];
        int np = 0;
#define PART(k, s) (parts[np].key = (k), parts[np++].after = (s))
        PART("ENTER", " to answer");
        if (voice_can_hear())
            PART("CTRL", "+"), PART("SPACE", " to talk");
        if (A.pick.active)
            PART("1", ""), PART("2", ""), PART("3", " to pick");
#undef PART
        const char *tail = "  \xc2\xb7  you can skip anything";
        int w = text_width(small, tail);
        for (int i = 0; i < np; i++)
            w += text_width(lk_label(10), parts[i].key) + 14 + text_width(small, parts[i].after) + 6;
        int x = A.W / 2 - w / 2, hy = A.H - 52;
        fill_round(A.frame, (rect){x - 14, hy - 5, w + 28, 32}, 16, RGBA(255, 255, 255, 170));
        for (int i = 0; i < np; i++) {
            x += look_keycap(x, hy, parts[i].key, false) + 4;
            x += draw_text(A.frame, small, x, hy + 3, parts[i].after, LK_SOFT) + 2;
        }
        draw_text(A.frame, small, x, hy + 3, tail, LK_FAINT);
    }
    /* talking instead of typing */
    voice_poll(); /* a clip to hear, a voice arriving */
    if (voice_toggle_pressed() && !thinking) {
        if (voice_listening())
            voice_listen_stop();
        else
            voice_listen_start();
    }
    char *heard = voice_poll_heard();
    if (heard && !thinking && !A.onboard_done && A.pick.active && is_system_pick(A.pick.kind)) {
        system_pick(heard); /* (said, not typed) */
    } else if (heard && !thinking && !A.onboard_done && A.pick.active && !first_said && !strcmp(A.pick.kind, "comfort")) {
        snprintf(A.pick.value[0], sizeof A.pick.value[0], "%.95s", heard);
        record_pick(0);
    } else if (heard && !thinking && !A.onboard_done) {
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
        rect skip = {A.W - 290, 24, 250, 36};
        look_pill(skip, LK_WHITE, RGBA(124, 108, 255, 40));
        if (ui_button(skip, "That's enough, make it now", small, 0, LK_ACCENT, 18, true))
            design_begin(NULL);
    }
}
