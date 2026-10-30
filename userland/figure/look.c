/* Reading and writing her look's markup. The text comes from the AI (or
 * from the disk), so nothing in it is trusted: unknown words are ignored and
 * every number is kept within bounds. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "look.h"

/* ---- the words ---- */

static const char *const eye_shapes[] = {"round", "almond", "droopy", "sharp", "sleepy", NULL};
static const char *const pupils[] = {"round", "slit", "star", "heart", "ring", NULL};
static const char *const brows[] = {"soft", "straight", "thick", "arched", "short", NULL};
static const char *const mouths[] = {"small", "cat", "fang", NULL};
static const char *const noses[] = {"dot", "line", "none", NULL};
static const char *const hair_styles[] = {"bob", "long", "short", "twintails", "ponytail", "hime", "wavy", "buns", "pixie", "wolf", "braid", NULL};
static const char *const bangs[] = {"full", "side", "parted", "blunt", "none", NULL};
static const char *const tops[] = {"none", "tee", "blouse", "sweater", "tank", "crop", "dress", NULL};
static const char *const bottoms[] = {"none", "skirt", "pleated", "shorts", "pants", "longskirt", NULL};
static const char *const legwear[] = {"none", "thighhighs", "kneesocks", "tights", "anklesocks", NULL};
static const char *const shoes[] = {"none", "sneakers", "boots", "loafers", NULL};
static const char *const necks[] = {"round", "scoop", "v", "high", NULL};
static const char *const patterns[] = {"none", "stripes", "plaid", "dots", "stars", "gradient", "check", NULL};
static const char *const props[] = {"lamp", "plant", "window", "shelf", "posters", "fairy", NULL};
static const char *const walks[] = {"light", "bouncy", "calm", "shy", NULL};

static int word_index(const char *const *words, const char *w, int fallback) {
    for (int i = 0; words[i]; i++)
        if (!strcasecmp(words[i], w))
            return i;
    return fallback;
}

/* ---- a small tag reader: <name a="b" ...> ---- */

typedef struct {
    char name[24];
    int n;
    char key[24][24];
    char val[24][160];
} tag;

static const char *read_tag(const char *s, tag *t) {
    memset(t, 0, sizeof *t);
    for (;;) {
        s = strchr(s, '<');
        if (!s)
            return NULL;
        if (!strncmp(s, "<!--", 4)) {
            const char *e = strstr(s, "-->");
            if (!e)
                return NULL;
            s = e + 3;
            continue;
        }
        if (s[1] == '/' || s[1] == '?' || s[1] == '!') {
            s = strchr(s, '>');
            if (!s)
                return NULL;
            s++;
            continue;
        }
        break;
    }
    s++;
    int i = 0;
    while (*s && (isalnum((unsigned char)*s) || *s == '-' || *s == '_') && i < 23)
        t->name[i++] = (char)tolower((unsigned char)*s++);
    while (*s && *s != '>') {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '/')
            s++;
        if (!*s || *s == '>')
            break;
        char k[24] = "";
        i = 0;
        while (*s && *s != '=' && *s != ' ' && *s != '>' && *s != '/') {
            if (i < 23)
                k[i++] = (char)tolower((unsigned char)*s);
            s++;
        }
        while (*s == ' ')
            s++;
        if (*s != '=')
            continue;
        s++;
        while (*s == ' ')
            s++;
        char q = *s;
        if (q != '"' && q != '\'')
            continue;
        s++;
        char v[160] = "";
        i = 0;
        while (*s && *s != q) {
            if (i < 159)
                v[i++] = *s;
            s++;
        }
        if (*s == q)
            s++;
        if (t->n < 24) {
            snprintf(t->key[t->n], 24, "%s", k);
            snprintf(t->val[t->n], 160, "%s", v);
            t->n++;
        }
    }
    return *s ? s + 1 : s;
}

static const char *attr(const tag *t, const char *k) {
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->key[i], k))
            return t->val[i];
    return NULL;
}

static void num(const tag *t, const char *k, float *out, float lo, float hi) {
    const char *v = attr(t, k);
    if (v) {
        float x = (float)atof(v);
        if (x == x)
            *out = clampf(x, lo, hi);
    }
}

static bool parse_colour(const char *s, rgb *out) {
    while (*s == ' ')
        s++;
    if (*s != '#')
        return false;
    char hex[8] = "";
    int n = 0;
    s++;
    while (n < 6 && isxdigit((unsigned char)s[n]))
        n++;
    if (n == 3) {
        hex[0] = hex[1] = s[0], hex[2] = hex[3] = s[1], hex[4] = hex[5] = s[2];
        hex[6] = 0;
    } else if (n == 6) {
        memcpy(hex, s, 6);
        hex[6] = 0;
    } else
        return false;
    *out = (rgb)strtoul(hex, NULL, 16);
    return true;
}

/* up to n colours, separated by spaces */
static int colours(const tag *t, const char *k, rgb *out, int n) {
    const char *v = attr(t, k);
    int got = 0;
    while (v && *v && got < n) {
        while (*v == ' ' || *v == ',')
            v++;
        if (!*v)
            break;
        if (parse_colour(v, &out[got]))
            got++;
        while (*v && *v != ' ' && *v != ',')
            v++;
    }
    return got;
}

static void choice(const tag *t, const char *k, const char *const *words, int *out) {
    const char *v = attr(t, k);
    if (v)
        *out = word_index(words, v, *out);
}

/* ---- reading ---- */

bool look_parse(const char *text, look *lk, char *why, size_t whylen) {
    look_default(lk);
    tag t;
    const char *s = text;
    bool seen = false;
    while ((s = read_tag(s, &t))) {
        if (!strcmp(t.name, "figure")) {
            seen = true;
            const char *nm = attr(&t, "name");
            if (nm)
                snprintf(lk->name, sizeof lk->name, "%s", nm);
            num(&t, "height", &lk->body.height, 1.2f, 1.85f);
            num(&t, "heads", &lk->body.heads, 3.0f, 7.5f);
        } else if (!strcmp(t.name, "body")) {
            body_params *b = &lk->body;
            num(&t, "height", &b->height, 1.2f, 1.85f);
            num(&t, "heads", &b->heads, 3.0f, 7.5f);
            num(&t, "shoulders", &b->shoulders, 0.17f, 0.24f);
            num(&t, "hips", &b->hips, 0.8f, 1.25f);
            num(&t, "waist", &b->waist, 0.8f, 1.2f);
            num(&t, "chest", &b->chest, 0, 1.4f);
            num(&t, "legs", &b->legs, 0.42f, 0.54f);
            num(&t, "arms", &b->arms, 0.9f, 1.1f);
            num(&t, "neck", &b->neck, 0.7f, 1.4f);
            num(&t, "build", &b->build, 0.8f, 1.3f);
        } else if (!strcmp(t.name, "skin")) {
            rgb c;
            if (colours(&t, "tone", &c, 1))
                lk->face.skin = lk->body.skin = c;
            num(&t, "blush", &lk->face.blush, 0, 1);
            colours(&t, "blush-color", &lk->face.blush_col, 1);
            const char *mole = attr(&t, "mole");
            if (mole)
                lk->face.mole = atoi(mole) != 0 || !strcasecmp(mole, "yes");
        } else if (!strcmp(t.name, "eyes")) {
            face_params *f = &lk->face;
            choice(&t, "shape", eye_shapes, &f->eye_shape);
            choice(&t, "pupil", pupils, &f->pupil);
            rgb c[2];
            int n = colours(&t, "iris", c, 2);
            if (n >= 1)
                f->iris_top = c[0], f->iris_bot = n > 1 ? c[1] : rgb_mix(c[0], 0xffffff, 0.45f);
            colours(&t, "white", &f->white, 1);
            colours(&t, "lash", &f->lash, 1);
            colours(&t, "pupil-color", &f->pupil_col, 1);
            float hl = (float)f->highlights, la = (float)f->lashes;
            num(&t, "highlights", &hl, 0, 3);
            num(&t, "lashes", &la, 0, 2);
            f->highlights = (int)hl, f->lashes = (int)la;
            num(&t, "size", &f->eye_size, 0.75f, 1.3f);
            num(&t, "gap", &f->eye_gap, 0.7f, 1.3f);
            num(&t, "tilt", &f->eye_tilt, -1, 1);
        } else if (!strcmp(t.name, "brows")) {
            choice(&t, "style", brows, &lk->face.brow_style);
            colours(&t, "color", &lk->face.brow, 1);
        } else if (!strcmp(t.name, "mouth")) {
            choice(&t, "style", mouths, &lk->face.mouth_style);
            colours(&t, "lip", &lk->face.lip, 1);
        } else if (!strcmp(t.name, "nose")) {
            choice(&t, "style", noses, &lk->face.nose);
        } else if (!strcmp(t.name, "hair")) {
            hair_params *h = &lk->hair;
            choice(&t, "style", hair_styles, &h->style);
            choice(&t, "bangs", bangs, &h->bangs);
            num(&t, "length", &h->length, -0.06f, 0.5f);
            num(&t, "volume", &h->volume, 0, 1);
            num(&t, "curl", &h->curl, -1, 1);
            num(&t, "part", &h->part, -1, 1);
            float ah = (float)h->ahoge;
            num(&t, "ahoge", &ah, 0, 1);
            h->ahoge = (int)ah;
            rgb c[2];
            int n = colours(&t, "color", c, 2);
            if (n >= 1)
                h->color = c[0], h->tip = n > 1 ? c[1] : c[0];
        } else if (!strcmp(t.name, "outfit")) {
            outfit_params *o = &lk->outfit;
            choice(&t, "top", tops, &o->top);
            choice(&t, "bottom", bottoms, &o->bottom);
            choice(&t, "legs", legwear, &o->legs);
            choice(&t, "shoes", shoes, &o->shoes);
            choice(&t, "neck", necks, &o->neckline);
            rgb c[6];
            int n = colours(&t, "colors", c, 6);
            if (n > 0)
                o->top_col = c[0];
            if (n > 1)
                o->bottom_col = c[1];
            if (n > 2)
                o->legs_col = c[2];
            if (n > 3)
                o->shoes_col = c[3], o->sole_col = rgb_scale(c[3], 0.6f);
            if (n > 4)
                o->top_col2 = c[4];
            if (n > 5)
                o->bottom_col2 = c[5];
            const char *pv = attr(&t, "patterns");
            if (pv) {
                char a[24] = "", b[24] = "";
                sscanf(pv, "%23s %23s", a, b);
                o->top_pattern = word_index(patterns, a, o->top_pattern);
                if (b[0])
                    o->bottom_pattern = word_index(patterns, b, o->bottom_pattern);
            }
            num(&t, "sleeve", &o->sleeve, 0, 1);
            num(&t, "hem", &o->hem, 0, 1);
            num(&t, "skirt", &o->skirt_len, 0.4f, 1.6f);
            num(&t, "flare", &o->flare, 0, 1);
        } else if (!strcmp(t.name, "room")) {
            colours(&t, "wall", &lk->wall, 1);
            colours(&t, "wall2", &lk->wall2, 1);
            colours(&t, "floor", &lk->floor_col, 1);
            colours(&t, "lamp", &lk->lamp, 1);
            const char *pv = attr(&t, "props");
            if (pv) {
                lk->props = 0;
                char buf[160];
                snprintf(buf, sizeof buf, "%s", pv);
                for (char *w = strtok(buf, " ,"); w; w = strtok(NULL, " ,")) {
                    int i = word_index(props, w, -1);
                    if (i >= 0)
                        lk->props |= 1 << i;
                }
            }
        } else if (!strcmp(t.name, "manner")) {
            num(&t, "energy", &lk->energy, 0, 1);
            num(&t, "shyness", &lk->shyness, 0, 1);
            choice(&t, "walk", walks, &lk->walk);
        }
    }
    if (!seen) {
        snprintf(why, whylen, "there is no <figure> in it");
        return false;
    }
    return true;
}

/* ---- writing ---- */

char *look_format(const look *lk) {
    size_t cap = 4096, n = 0;
    char *o = malloc(cap);
    if (!o)
        return NULL;
#define PUT(...) n += (size_t)snprintf(o + n, n < cap ? cap - n : 0, __VA_ARGS__)
    const body_params *b = &lk->body;
    const face_params *f = &lk->face;
    const hair_params *h = &lk->hair;
    const outfit_params *u = &lk->outfit;
    PUT("<figure name=\"%s\" height=\"%.2f\" heads=\"%.1f\">\n", lk->name, b->height, b->heads);
    PUT("  <body shoulders=\"%.3f\" hips=\"%.2f\" waist=\"%.2f\" chest=\"%.2f\" legs=\"%.3f\" arms=\"%.2f\" neck=\"%.2f\" "
        "build=\"%.2f\"/>\n",
        b->shoulders, b->hips, b->waist, b->chest, b->legs, b->arms, b->neck, b->build);
    PUT("  <skin tone=\"#%06x\" blush=\"%.2f\" blush-color=\"#%06x\"%s/>\n", f->skin, f->blush, f->blush_col,
        f->mole ? " mole=\"1\"" : "");
    PUT("  <eyes shape=\"%s\" iris=\"#%06x #%06x\" pupil=\"%s\" highlights=\"%d\" lashes=\"%d\" size=\"%.2f\" gap=\"%.2f\" "
        "tilt=\"%.2f\" lash=\"#%06x\"/>\n",
        eye_shapes[f->eye_shape], f->iris_top, f->iris_bot, pupils[f->pupil], f->highlights, f->lashes, f->eye_size,
        f->eye_gap, f->eye_tilt, f->lash);
    PUT("  <brows style=\"%s\" color=\"#%06x\"/>\n", brows[f->brow_style], f->brow);
    PUT("  <mouth style=\"%s\" lip=\"#%06x\"/>\n", mouths[f->mouth_style], f->lip);
    PUT("  <nose style=\"%s\"/>\n", noses[f->nose]);
    PUT("  <hair style=\"%s\" bangs=\"%s\" length=\"%.2f\" volume=\"%.2f\" curl=\"%.2f\" part=\"%.2f\" ahoge=\"%d\" "
        "color=\"#%06x #%06x\"/>\n",
        hair_styles[h->style], bangs[h->bangs], h->length, h->volume, h->curl, h->part, h->ahoge, h->color, h->tip);
    PUT("  <outfit top=\"%s\" bottom=\"%s\" legs=\"%s\" shoes=\"%s\" neck=\"%s\" colors=\"#%06x #%06x #%06x #%06x #%06x "
        "#%06x\" patterns=\"%s %s\" sleeve=\"%.2f\" hem=\"%.2f\" skirt=\"%.2f\" flare=\"%.2f\"/>\n",
        tops[u->top], bottoms[u->bottom], legwear[u->legs], shoes[u->shoes], necks[u->neckline], u->top_col, u->bottom_col,
        u->legs_col, u->shoes_col, u->top_col2, u->bottom_col2, patterns[u->top_pattern], patterns[u->bottom_pattern],
        u->sleeve, u->hem, u->skirt_len, u->flare);
    PUT("  <room wall=\"#%06x\" wall2=\"#%06x\" floor=\"#%06x\" lamp=\"#%06x\" props=\"", lk->wall, lk->wall2, lk->floor_col,
        lk->lamp);
    bool first = true;
    for (int i = 0; props[i]; i++)
        if (lk->props & (1 << i)) {
            PUT("%s%s", first ? "" : " ", props[i]);
            first = false;
        }
    PUT("\"/>\n");
    PUT("  <manner energy=\"%.2f\" shyness=\"%.2f\" walk=\"%s\"/>\n", lk->energy, lk->shyness, walks[lk->walk]);
    PUT("</figure>\n");
#undef PUT
    if (n >= cap) { /* never with these sizes, but just in case */
        o[cap - 1] = 0;
    }
    return o;
}

const char *look_reference(void) {
    return "<figure name height(1.2-1.85 m) heads(3 chibi-7.5)>\n"
           " <body shoulders(0.17-0.24) hips(0.8-1.25) waist(0.8-1.2) chest(0-1.4) legs(0.42-0.54) arms neck build(0.8-1.3)/>\n"
           " <skin tone=#hex blush(0-1) blush-color mole(0|1)/>\n"
           " <eyes shape=round|almond|droopy|sharp|sleepy iris=\"#top #bottom\" pupil=round|slit|star|heart|ring "
           "highlights(0-3) lashes(0-2) size(0.75-1.3) gap(0.7-1.3) tilt(-1..1) lash=#hex/>\n"
           " <brows style=soft|straight|thick|arched|short color/> <mouth style=small|cat|fang lip/> <nose style=dot|line|none/>\n"
           " <hair style=bob|long|short|hime|pixie bangs=full|side|parted|blunt|none length(m below chin) volume curl(-1..1) "
           "part(-1..1) ahoge(0|1) color=\"#roots #tips\"/>\n"
           " <outfit top=none|tee|blouse|sweater|tank|crop|dress bottom=none|skirt|pleated|shorts|pants|longskirt "
           "legs=none|thighhighs|kneesocks|tights|anklesocks shoes=none|sneakers|boots|loafers neck=round|scoop|v|high "
           "colors=\"#top #bottom #legs #shoes #top-pattern #bottom-pattern\" patterns=\"top bottom\" "
           "(none|stripes|plaid|dots|stars|gradient|check) sleeve(0-1) hem(0-1) skirt(0.4-1.6) flare(0-1)/>\n"
           " <room wall floor lamp props=\"lamp plant window shelf posters fairy\"/>\n"
           " <manner energy(0-1) shyness(0-1) walk=light|bouncy|calm|shy/>\n"
           "</figure>";
}
