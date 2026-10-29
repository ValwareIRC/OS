#include "keymap.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

/* Each layout is the US layout plus the keys that differ on it. A key's
 * three strings are what it types alone, with Shift and with AltGr (one
 * character each, UTF-8). A string starting with \1 is a dead key: the
 * accent waits for the next key and lands on it (´ then e is é). */

struct key {
    unsigned char code;
    const char *plain, *shift, *altgr;
};

#define D "\1"

/* US: Linux key codes 2-53, plus the extra ISO key (86). */
static const struct key us[] = {
    {2, "1", "!", 0},   {3, "2", "@", 0},   {4, "3", "#", 0},   {5, "4", "$", 0},   {6, "5", "%", 0},
    {7, "6", "^", 0},   {8, "7", "&", 0},   {9, "8", "*", 0},   {10, "9", "(", 0},  {11, "0", ")", 0},
    {12, "-", "_", 0},  {13, "=", "+", 0},  {16, "q", "Q", 0},  {17, "w", "W", 0},  {18, "e", "E", 0},
    {19, "r", "R", 0},  {20, "t", "T", 0},  {21, "y", "Y", 0},  {22, "u", "U", 0},  {23, "i", "I", 0},
    {24, "o", "O", 0},  {25, "p", "P", 0},  {26, "[", "{", 0},  {27, "]", "}", 0},  {30, "a", "A", 0},
    {31, "s", "S", 0},  {32, "d", "D", 0},  {33, "f", "F", 0},  {34, "g", "G", 0},  {35, "h", "H", 0},
    {36, "j", "J", 0},  {37, "k", "K", 0},  {38, "l", "L", 0},  {39, ";", ":", 0},  {40, "'", "\"", 0},
    {41, "`", "~", 0},  {43, "\\", "|", 0}, {44, "z", "Z", 0},  {45, "x", "X", 0},  {46, "c", "C", 0},
    {47, "v", "V", 0},  {48, "b", "B", 0},  {49, "n", "N", 0},  {50, "m", "M", 0},  {51, ",", "<", 0},
    {52, ".", ">", 0},  {53, "/", "?", 0},  {55, "*", "*", 0},  {57, " ", " ", 0},  {86, "\\", "|", 0},
    {0, 0, 0, 0},
};

static const struct key gb[] = {
    {3, "2", "\"", 0}, {4, "3", "£", 0}, {5, "4", "$", "€"}, {40, "'", "@", 0}, {41, "`", "¬", "¦"},
    {43, "#", "~", 0}, {86, "\\", "|", 0}, {18, "e", "E", "é"}, {30, "a", "A", "á"}, {23, "i", "I", "í"},
    {24, "o", "O", "ó"}, {22, "u", "U", "ú"}, {0, 0, 0, 0},
};

static const struct key de[] = {
    {3, "2", "\"", "²"}, {4, "3", "§", "³"}, {7, "6", "&", 0},   {8, "7", "/", "{"},  {9, "8", "(", "["},
    {10, "9", ")", "]"}, {11, "0", "=", "}"}, {12, "ß", "?", "\\"}, {13, D "´", D "`", 0},
    {16, "q", "Q", "@"}, {18, "e", "E", "€"}, {21, "z", "Z", 0},  {26, "ü", "Ü", 0},  {27, "+", "*", "~"},
    {39, "ö", "Ö", 0},   {40, "ä", "Ä", 0},   {41, D "^", "°", 0}, {43, "#", "'", 0},  {44, "y", "Y", 0},
    {50, "m", "M", "µ"}, {51, ",", ";", 0},   {52, ".", ":", 0},  {53, "-", "_", 0},  {86, "<", ">", "|"},
    {0, 0, 0, 0},
};

static const struct key fr[] = {
    {2, "&", "1", 0},     {3, "é", "2", D "~"}, {4, "\"", "3", "#"}, {5, "'", "4", "{"},  {6, "(", "5", "["},
    {7, "-", "6", "|"},   {8, "è", "7", D "`"}, {9, "_", "8", "\\"}, {10, "ç", "9", "^"}, {11, "à", "0", "@"},
    {12, ")", "°", "]"},  {13, "=", "+", "}"},  {16, "a", "A", 0},   {17, "z", "Z", 0},  {18, "e", "E", "€"},
    {26, D "^", D "¨", 0}, {27, "$", "£", "¤"}, {30, "q", "Q", 0},   {39, "m", "M", 0},  {40, "ù", "%", 0},
    {41, "²", 0, 0},      {43, "*", "µ", 0},    {44, "w", "W", 0},   {50, ",", "?", 0},  {51, ";", ".", 0},
    {52, ":", "/", 0},    {53, "!", "§", 0},    {86, "<", ">", 0},   {0, 0, 0, 0},
};

static const struct key es[] = {
    {2, "1", "!", "|"},   {3, "2", "\"", "@"}, {4, "3", "·", "#"},  {5, "4", "$", "~"},  {7, "6", "&", "¬"},
    {8, "7", "/", 0},     {9, "8", "(", 0},    {10, "9", ")", 0},   {11, "0", "=", 0},   {12, "'", "?", 0},
    {13, "¡", "¿", 0},    {18, "e", "E", "€"}, {26, D "`", D "^", "["}, {27, "+", "*", "]"}, {39, "ñ", "Ñ", 0},
    {40, D "´", D "¨", "{"}, {41, "º", "ª", "\\"}, {43, "ç", "Ç", "}"}, {51, ",", ";", 0}, {52, ".", ":", 0},
    {53, "-", "_", 0},    {86, "<", ">", 0},   {0, 0, 0, 0},
};

/* Brazilian ABNT2, with its extra key by the right Shift (89). */
static const struct key br[] = {
    {3, "2", "@", "²"},   {4, "3", "#", "³"},   {5, "4", "$", "£"},  {6, "5", "%", "¢"},  {7, "6", D "¨", "¬"},
    {13, "=", "+", "§"},  {16, "q", "Q", "/"},  {17, "w", "W", "?"}, {18, "e", "E", "€"}, {26, D "´", D "`", 0},
    {27, "[", "{", "ª"},  {39, "ç", "Ç", 0},    {40, D "~", D "^", 0}, {41, "'", "\"", 0}, {43, "]", "}", "º"},
    {51, ",", "<", 0},    {52, ".", ">", 0},    {53, ";", ":", 0},   {86, "\\", "|", 0}, {89, "/", "?", "°"},
    {0, 0, 0, 0},
};

static const struct {
    const char *id, *name;
    const struct key *keys;
} layouts[] = {
    {"us", "English (US)", NULL},  {"gb", "English (UK)", gb}, {"de", "Deutsch", de},
    {"fr", "Français", fr},        {"es", "Español", es},      {"br", "Português (Brasil)", br},
};
#define NLAYOUTS ((int)(sizeof layouts / sizeof layouts[0]))

static int cur;
static int dead; /* accent waiting for the next key, or 0 */

int keymap_count(void) { return NLAYOUTS; }
const char *keymap_id(int i) { return i >= 0 && i < NLAYOUTS ? layouts[i].id : ""; }
const char *keymap_name(int i) { return i >= 0 && i < NLAYOUTS ? layouts[i].name : ""; }
const char *keymap_current(void) { return layouts[cur].id; }

bool keymap_set(const char *id) {
    for (int i = 0; i < NLAYOUTS; i++)
        if (!strcasecmp(layouts[i].id, id) || (!strcasecmp(id, "uk") && !strcmp(layouts[i].id, "gb")) ||
            (!strcasecmp(id, "pt-br") && !strcmp(layouts[i].id, "br"))) {
            cur = i;
            dead = 0;
            return true;
        }
    return false;
}

/* The first character of a UTF-8 string. */
static int utf8_first(const char *s, const char **rest) {
    const unsigned char *p = (const unsigned char *)s;
    int c = *p, n = 0;
    if (c < 0x80) n = 0;
    else if (c >= 0xf0) c &= 0x07, n = 3;
    else if (c >= 0xe0) c &= 0x0f, n = 2;
    else c &= 0x1f, n = 1;
    p++;
    for (; n > 0 && (*p & 0xc0) == 0x80; n--, p++)
        c = c << 6 | (*p & 0x3f);
    if (rest)
        *rest = (const char *)p;
    return c;
}

static const struct key *find(const struct key *t, int code) {
    for (; t && t->code; t++)
        if (t->code == code)
            return t;
    return NULL;
}

/* Accents on letters: which accent, the letters it goes on, and what they
 * become (in the same order). */
static const struct {
    const char *accent, *from, *to;
} compose[] = {
    {"´", "aeiouyAEIOUYcC", "áéíóúýÁÉÍÓÚÝçÇ"},
    {"`", "aeiouAEIOU", "àèìòùÀÈÌÒÙ"},
    {"^", "aeiouAEIOU", "âêîôûÂÊÎÔÛ"},
    {"~", "aonAON", "ãõñÃÕÑ"},
    {"¨", "aeiouyAEIOU", "äëïöüÿÄËÏÖÜ"},
};

static int combine(int accent, int c) {
    for (size_t i = 0; i < sizeof compose / sizeof compose[0]; i++) {
        if (utf8_first(compose[i].accent, NULL) != accent)
            continue;
        const char *f = compose[i].from, *t = compose[i].to;
        while (*f && *t) {
            int a = utf8_first(f, &f), b = utf8_first(t, &t);
            if (a == c)
                return b;
        }
    }
    return 0;
}

/* Caps Lock flips the case of letters, not of everything Shift would. */
static int other_case(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 32;
    if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
    return 0;
}

int keymap_translate(int code, bool shift, bool altgr, bool caps, int out[2]) {
    const struct key *k = find(layouts[cur].keys, code);
    const struct key *base = find(us, code);
    if (!k)
        k = base;
    if (!k)
        return 0;
    const char *s = altgr ? k->altgr : shift ? k->shift : k->plain;
    if (altgr && !s) /* no AltGr symbol on it: the key as it is */
        s = shift ? k->shift : k->plain;
    if (!s || !*s)
        return 0;
    bool is_dead = s[0] == '\1';
    int c = utf8_first(is_dead ? s + 1 : s, NULL);
    if (caps && !altgr && !is_dead) {
        int plain = k->plain ? utf8_first(k->plain, NULL) : 0, shifted = k->shift ? utf8_first(k->shift, NULL) : 0;
        if (other_case(plain) == shifted && shifted)
            c = shift ? plain : shifted;
    }
    if (is_dead) {
        if (dead) { /* the same accent twice (or another): the first one on its own */
            int prev = dead;
            dead = prev == c ? 0 : c;
            out[0] = prev;
            return 1;
        }
        dead = c;
        return 0;
    }
    if (dead) {
        int accent = dead;
        dead = 0;
        if (c == ' ') {
            out[0] = accent;
            return 1;
        }
        int m = combine(accent, c);
        if (m) {
            out[0] = m;
            return 1;
        }
        out[0] = accent;
        out[1] = c;
        return 2;
    }
    out[0] = c;
    return 1;
}
