#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "gfx.h"

/* Facet: the design language the AI writes for each user. There is no
 * built-in theme: every colour, font, shape, layout choice, wallpaper and
 * piece of wording on the desktop comes from a Facet document the AI
 * produced from that person's onboarding. This header is the vocabulary;
 * design.c parses and validates the AI's JSON into it. */

enum wallpaper_kind { WP_GRADIENT, WP_AURORA, WP_WAVES, WP_BOKEH, WP_DUNES, WP_SUN, WP_CONSTELLATION, WP_PAPER,
                      WP_NEBULA, WP_LIQUID, WP_FLOW, WP_PRISM, WP_COUNT };
/* Kinds from WP_AURORA on marked animated in wallpaper_animated() move slowly. */
enum icon_style { ICONSTYLE_FILLED, ICONSTYLE_LINE, ICONSTYLE_GLOW, ICONSTYLE_DUOTONE };
enum material { MAT_GLASS, MAT_FROST, MAT_SOLID, MAT_NEON };
enum dock_pos { DOCK_BOTTOM, DOCK_LEFT, DOCK_TOP, DOCK_RIGHT };
enum dock_style { DOCK_PILL, DOCK_BAR, DOCK_ORBS };
enum icon_shape { ICON_CIRCLE, ICON_SQUIRCLE, ICON_SQUARE, ICON_GLYPH };
enum menu_style { MENU_GRID, MENU_LIST, MENU_SHEET, MENU_RADIAL };
enum clock_style { CLOCK_LARGE, CLOCK_CORNER, CLOCK_POETIC, CLOCK_HIDDEN, CLOCK_RING };
enum explorer_style { EXPLORER_LIST, EXPLORER_ICONS, EXPLORER_COLUMNS };
enum companion_pos { COMPANION_RIGHT, COMPANION_LEFT, COMPANION_FLOATING };

#define MAX_SECTIONS 4
#define MAX_ITEMS 8

enum app_id { APP_FILES, APP_COMPANION, APP_NOTES, APP_SETTINGS, APP_ABOUT, APP_TERMINAL, APP_REDESIGN, APP_POWER, APP_COUNT };

struct app_look {
    char label[32];
    char glyph[8]; /* one or two characters drawn as the icon */
};

struct menu_section {
    char title[48];
    int items[MAX_ITEMS]; /* enum app_id */
    int count;
};

typedef struct design {
    char ai_name[32];
    char user_name[32];
    char greeting[160];
    char tagline[96];

    color bg, bg2, surface, surface_text, text, muted, accent, accent2;
    float surface_opacity; /* 0.3 .. 1 */
    float radius;          /* corner radius in px */
    float blur;            /* frosted glass strength 0..24 */
    bool shadows;
    enum material material;
    float glow;   /* 0..1: how much light things give off */
    float motion; /* 0..1: how much things move */

    char font_heading[32], font_body[32];
    float heading_px, body_px;

    enum wallpaper_kind wallpaper;
    color wp[4];
    int wp_seed;
    float wp_intensity;

    /* How the person reaches their things. Whether a dock, a start menu or
     * any other familiar piece exists at all is the AI's choice; the only
     * rule (enforced by the validator) is that every app stays reachable. */
    bool use_dock, use_menu, use_desktop_icons, use_command_bar, use_orb;
    int hot_corner; /* -1 none, 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */
    int orb_corner; /* where the floating companion orb rests */

    enum dock_pos dock;
    enum dock_style dock_style;
    enum icon_shape icons;
    enum icon_style icon_style;
    bool dock_labels;
    int dock_items[APP_COUNT];
    int ndock;
    bool dock_menu_button;

    int desk_items[APP_COUNT];
    int ndesk;
    int desk_area; /* 0 left column, 1 right column, 2 bottom row, 3 scattered */

    int bar_position; /* command bar: 0 top, 1 centre, 2 bottom */
    char bar_placeholder[64];

    enum menu_style menu;
    char menu_title[48];
    struct menu_section sections[MAX_SECTIONS];
    int nsections;

    enum clock_style clock;
    enum explorer_style explorer;
    enum companion_pos companion;

    struct app_look apps[APP_COUNT];

    char rationale[400]; /* the AI's note on why it designed it this way */
} design;

/* What the onboarding learned; it seeds both the design prompt and the
 * deterministic fallback used when no AI is reachable. */
typedef struct profile {
    char user_name[32];
    char ai_name[32];
    char palette_name[48];
    color palette[4];
    bool has_palette;
    char mood[64];
    char background[64];   /* living art / photo / AI image */
    char autonomy[128];
    char redesign[64];     /* "only when asked", "evolve weekly", ... */
    char notes[16000];     /* the conversation, as given to the designer */

    /* The portrait: what the companion has understood about them, as
     * short implications ("rhythm: night owl, best after 10pm"), never
     * transcripts. Shown to them, editable, and kept on this computer. */
    struct fact {
        char key[24];
        char val[240];
    } facts[40];
    int nfacts;
    /* Comfort, chosen first and applied at once. */
    float text_scale;      /* 0 or 1: as designed */
    bool high_contrast, reduce_motion;
    float tempo;           /* motion 0 (still) .. 1 (lively); < 0 not chosen */
    char type_font[32];    /* the typeface their own words looked best in */
    bool has_room;         /* the place that felt like home, as a wallpaper */
    int room_kind;
    char room[64];
} profile;

/* Adds or extends a portrait fact (values for the same key accumulate). */
void portrait_learn(profile *p, const char *key, const char *val);
void portrait_forget(profile *p, int i);
bool portrait_save(const profile *p, const char *path);
bool portrait_load(profile *p, const char *path);
/* "key: value" lines, for prompts. */
void portrait_text(const profile *p, char *out, size_t len);
int wallpaper_kind_from_name(const char *name); /* -1 if unknown */
const char *wallpaper_name(int kind);

/* Parses AI output (JSON, possibly wrapped in prose or code fences).
 * Missing or invalid fields are filled from the profile; returns false only
 * if no JSON object could be found at all. `problems` lists what was fixed. */
bool design_from_json(design *d, const char *text, const profile *p, char *problems, int problems_len);
/* A design derived purely from the person's choices (no AI). */
void design_from_profile(design *d, const profile *p);
/* The instructions given to the AI for producing a Facet document. */
const char *design_prompt(void);
const char *app_default_name(int app);
const char *app_id(int app); /* "files", "companion", ... */
bool design_save(const design *d, const char *path);
bool design_load(design *d, const char *path, const profile *p);

struct fm_doc;
/* Fills the design record (colours, fonts, wallpaper, names, labels) from a
 * Facet markup document; problems gets what the AI should fix. */
bool design_from_facet(design *d, struct fm_doc *doc, const profile *p, char *problems, int plen);
/* Markup that reproduces a design record (fallback when no AI document). */
char *design_to_facet(const design *d);
const char *design_prompt_markup(void);
