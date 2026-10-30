#pragma once
/* Facet markup engine.
 *
 * The AI describes a person's whole desktop as a document: XML-like markup
 * for structure, a CSS-like stylesheet for looks. This engine parses both,
 * resolves styles, lays elements out, draws them with the OS's renderer, and
 * handles input. Nothing about the layout is built in: a Windows XP style
 * taskbar, a Mac style dock and menu bar, or apps orbiting a light are all
 * just different documents.
 *
 * It is deliberately small and not web technology: a fixed, OS-specific
 * vocabulary of elements, a subset of CSS properties, flex/grid/stack/orbit
 * layout, and a closed set of actions. That keeps documents easy for a model
 * to write, easy to validate, and easy for the companion to read and drive.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../font.h"
#include "../gfx.h"
#include "../sys.h"

typedef struct fm_node fm_node;
typedef struct fm_doc fm_doc;

/* ---- values ---- */

enum fm_unit { FM_AUTO, FM_PX, FM_PCT, FM_FULL };

typedef struct fm_len {
    enum fm_unit unit;
    float v;
} fm_len;

enum fm_bg_kind { FM_BG_NONE, FM_BG_SOLID, FM_BG_LINEAR, FM_BG_RADIAL };

typedef struct fm_bg {
    enum fm_bg_kind kind;
    color a, b;
    float angle; /* degrees, linear only: 180 = top to bottom */
} fm_bg;

/* ---- computed style ---- */

enum fm_dir { FM_ROW, FM_COLUMN };
enum fm_mode { FM_FLEX, FM_GRID, FM_STACK, FM_ORBIT };
enum fm_align { FM_A_STRETCH, FM_A_START, FM_A_CENTER, FM_A_END };
enum fm_justify { FM_J_START, FM_J_CENTER, FM_J_END, FM_J_BETWEEN, FM_J_AROUND, FM_J_EVENLY };
enum fm_overflow { FM_OV_VISIBLE, FM_OV_CLIP, FM_OV_SCROLL };
enum fm_textalign { FM_T_START, FM_T_CENTER, FM_T_END };

typedef struct fm_style {
    bool hidden;              /* display: none */
    int mode;                 /* FM_FLEX, FM_GRID, FM_STACK, FM_ORBIT */
    enum fm_dir dir;
    bool wrap;
    bool dir_set;             /* the design said which way (lists otherwise follow their container) */
    bool absolute;            /* position: absolute */
    fm_len left, top, right, bottom;
    fm_len width, height, min_w, min_h, max_w, max_h;
    float grow;
    float pad[4], margin[4];  /* top right bottom left */
    float gap;
    enum fm_align align, self_align;
    bool self_align_set;
    enum fm_justify justify;
    enum fm_overflow overflow;

    fm_bg bg;
    float blur;               /* backdrop blur: frosted glass */
    float saturate;           /* backdrop vibrancy, 1 = none */
    float border_w;
    color border;
    fm_bg border_bg;          /* gradient borders */
    float radius[4];          /* tl tr br bl */
    float shadow_x, shadow_y, shadow_blur;
    color shadow;
    float glow_size;
    color glow;
    float opacity;

    color color;              /* text and icon colour */
    char font[32];
    float font_size;
    enum fm_textalign text_align;
    float line_height;        /* multiple of the font's line */
    bool uppercase;
    /* typography (fm/type.c): all inherited, like CSS */
    short font_weight;        /* 100..900, 0 = regular */
    bool italic;
    char text_case;           /* 0, 'u'pper, 'l'ower, 'c'apitalize */
    float letter_spacing;     /* px, plus letter_spacing_em of the size */
    float letter_spacing_em;
    float tshadow_x, tshadow_y, tshadow_blur;
    color tshadow;            /* text-shadow colour; 0 = none */
    bool nowrap;              /* white-space: nowrap */
    bool clip_text;           /* text-overflow: clip (the default cuts with an ellipsis) */
    int line_clamp;           /* most lines a text shows; 0 = as many as fit */

    float scale;
    float tx, ty;             /* translate */
    float tx_pct, ty_pct;     /* translate in % of its own size (translateX(-50%) centring) */
    float transition;         /* ms */
    float z;

    float icon_size;
    char icon_style[12];      /* filled | line | glow | duotone | plain */
    char icon_shape[12];      /* circle | squircle | square | none */
    int columns;              /* grid */
    float orbit;              /* orbit radius */
    char place[16], dock[16]; /* the place=/dock= attributes, also accepted as style */
} fm_style;

/* ---- tree ---- */

#define FM_MAX_ATTRS 16

typedef struct fm_attr {
    char *name, *value;
} fm_attr;

enum fm_kind { FM_ELEM, FM_TEXT };

struct fm_node {
    enum fm_kind kind;
    char tag[24];
    char id[48];            /* from the document, or generated ("_12") */
    char *text;             /* FM_TEXT content */
    fm_attr attrs[FM_MAX_ATTRS];
    int nattrs;
    int line;               /* source line, for error messages */
    fm_node *parent, *first, *last, *next;
    bool generated;         /* made by a live element (applist, windowlist) */
    int app, win;           /* for generated nodes: which app / window */

    /* interaction state */
    bool hover, active, focus, open;
    float a_hover, a_open, a_press; /* animated 0..1 */
    float flash;                    /* 1 when the companion touched it, fades */
    int scroll;

    /* results of the last frame */
    fm_style st;                    /* target style for the current states */
    struct {                        /* animated values easing toward `st` */
        fm_bg bg;
        color color, border, glow, shadow;
        float glow_size, scale, opacity, tx, ty;
        bool init;
        float vx, vy, vw, vh;       /* where it is drawn, easing to `vis` during a morph */
        bool vinit;
    } av;
    bool fresh;                     /* new in a morph: fades in */
    rect box;                       /* border box from layout */
    rect vis;                       /* box after transforms: where it is drawn and hit */
    float vscale;                   /* accumulated scale (fonts and icons follow it) */
    float mw, mh;                   /* measured preferred size */
    bool laid_out;
    int content_h;                  /* height of the children, for scrolling */
};

/* ---- documents ---- */

typedef struct fm_problem {
    int line;
    char msg[160];
    bool fatal;
} fm_problem;

#define FM_MAX_PROBLEMS 48

struct fm_rule;

struct fm_doc {
    fm_node *root;          /* <facet> */
    fm_node *desktop;       /* <desktop> */
    fm_node *window_tpl;    /* <template id="window"> */
    struct fm_rule *rules;
    int nrules;
    char *vars[64][2];      /* custom properties: name, value */
    int nvars;
    char *css;              /* stylesheet source (the <style> text) */
    fm_problem problems[FM_MAX_PROBLEMS];
    int nproblems;
    int next_id;
    fm_node *focus;         /* text input with the keyboard */
    fm_node *pressed;
    fm_node *hovered;       /* deepest node under the pointer */
    rect screen, work;      /* whole screen; area left for windows */
    const struct input_state *in; /* this frame's input, set by the host */
    int cur_win;            /* window whose frame is being laid out/drawn, or -1 */
    int hover_win;          /* window whose frame has the pointer, or -1 */
    bool frame_focused;
    bool changed;           /* tree edited: the host may want to save it */
    float clock;            /* ms of animation so far (fm_animate) */
    float entrance_at;      /* when the staggered entrance began; < 0 none */
    float morph_until;      /* clock until which shapes and colours glide (fm_morph_from) */
};

/* Parses markup and appends the elements to `parent`. Returns the first. */
fm_node *fm_parse_fragment(fm_doc *d, fm_node *parent, const char *markup);
/* Runs a node's action (what="..."), as if it had been clicked. */
void fm_activate(fm_doc *d, fm_node *n);
/* Window frames from <template id="window">: lay out at `r`, handle the
 * pointer (call for the topmost window under it), draw. */
bool fm_window_input(fm_doc *d, int win, rect r, bool focused);
/* The app id of an element: app="files" etc. -1 if none. */
int fm_app_index(const char *id);

/* Parses a whole document (<facet> ... </facet>). Never returns NULL for
 * non-empty input: problems are recorded in doc->problems. */
fm_doc *fm_parse(const char *text);
void fm_free(fm_doc *d);
/* Serialises the document back to markup (for saving and for the AI). */
char *fm_serialize(const fm_doc *d);
bool fm_has_fatal(const fm_doc *d);
void fm_report(fm_doc *d, int line, bool fatal, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* Tree helpers */
const char *fm_attr_get(const fm_node *n, const char *name);
void fm_attr_set(fm_node *n, const char *name, const char *value);
fm_node *fm_find(fm_doc *d, const char *selector_or_id);
fm_node *fm_node_new(fm_doc *d, const char *tag);
void fm_append(fm_node *parent, fm_node *child);
void fm_remove(fm_node *n);
char *fm_text_of(const fm_node *n); /* concatenated text content (malloc'd) */

/* Stylesheet: (re)parse doc->css plus built-in defaults into rules. */
void fm_css_load(fm_doc *d);
/* Adds or replaces rules from CSS text (used by companion edits). */
void fm_css_patch(fm_doc *d, const char *css);
/* Resolves styles for the whole tree for the current states. */
void fm_style_tree(fm_doc *d);
const char *fm_var(const fm_doc *d, const char *name);
/* Light and dark, set by the shell (theme.c) before fm_css_load: which
 * colour-scheme blocks apply (@dark / @light / @media (prefers-color-scheme:
 * ...)): 0 none, 1 light, 2 dark. And, when the document has no block for
 * the scheme wanted, a mapping the shell applies to every colour it names. */
extern int fm_scheme;
extern color (*fm_color_filter)(color c);
/* A colour as written, with no scheme mapping. */
bool fm_parse_color_raw(const char *s, color *out);
/* A property from a plain `tag { }` rule for something that is not an
 * element (the terminal app: `terminal { background: #000 }`), or NULL. */
const char *fm_rule_value(const fm_doc *d, const char *tag, const char *prop);
/* The window frame laid out last: its box, corner radii and border, its
 * <slot>'s box and the content rect inside the slot's padding. */
bool fm_window_slot(fm_doc *d, rect *frame, float radius[4], float *border, rect *slot, rect *content);
/* Typography properties (fm/type.c), for the stylesheet: returns 1 if it
 * took the declaration, -1 if it is typography with a bad value, 0 if not
 * typography at all. */
int fm_type_apply(fm_style *st, const char *prop, const char *value);
bool fm_type_font_word(fm_style *st, const char *word);   /* "bold", "italic", ... in a font shorthand */
bool fm_type_weight_number(fm_style *st, float num, const char *rest); /* "600 14px Inter" */
void fm_type_inherit(fm_style *st, const fm_style *parent);

/* Layout and drawing. Layers: 0 = below windows, 1 = above windows. */
void fm_layout(fm_doc *d, rect screen);
void fm_render_layer(fm_doc *d, canvas *c, int layer);
void fm_render_window_frame(fm_doc *d, canvas *c, int win, rect r, bool focused, rect *content_out);
int fm_layer_of(const fm_node *n);

/* Input. Returns true if the pointer is over something in that layer. */
bool fm_hit_layer(fm_doc *d, int layer, int x, int y);
/* Feeds this frame's input; `layer_mask` says which layers may react
 * (bit 0: below windows, bit 1: above). Returns true if it used the click. */
bool fm_input(fm_doc *d, int layer_mask);
void fm_animate(fm_doc *d, float dt_ms);
/* Surfaces arrive one after another (on first showing a design). */
void fm_entrance(fm_doc *d);
/* Another version of the same desktop takes over: what both have (by id,
 * else by place in the tree) starts where it was in `from` and glides and
 * recolours into place over `ms`; what is new fades in. */
void fm_morph_from(fm_doc *to, const fm_doc *from, float ms);
void fm_close_popovers(fm_doc *d, fm_node *except);
void fm_set_open(fm_doc *d, fm_node *n, bool open);
fm_node *fm_hotkey(fm_doc *d, const char *key);

/* Companion access */
char *fm_describe(fm_doc *d);              /* compact text view of the UI */
extern float fm_contrast_floor;             /* minimum text contrast (WCAG ratio) */
/* Applies one [[ui:...]] command; returns a short result for the log. */
const char *fm_command(fm_doc *d, const char *cmd, fm_node **touched);
void fm_flash(fm_node *n);

/* ---- provided by the shell (desktop.c) ---- */

int fm_host_app_count(void);
const char *fm_host_app_id(int app);     /* "files", "companion", ... */
const char *fm_host_app_label(int app);
int fm_host_window_count(void);          /* open windows, for <windowlist> */
int fm_host_window_app(int i);
const char *fm_host_window_title(int i);
bool fm_host_window_focused(int i);
void fm_host_action(fm_doc *d, fm_node *src, const char *action);
/* Fills `out` for bind="..." keys (user.name, ai.name, greeting, ...). */
bool fm_host_bind(const char *key, char *out, size_t n);
void fm_host_draw_view(const char *app, rect r);   /* built-in app bodies */
void fm_host_draw_orb(rect r, bool active);
/* <avatar frame="rect|circle" tint="none|codec">: her live portrait (the light when she has no face) */
/* shot="face|bust|waist|full": how close the camera is */
void fm_host_draw_avatar(rect r, const char *frame, const char *tint, const char *shot, bool active);
void fm_host_draw_icon(rect r, int app, const fm_style *st, bool hover);
font *fm_host_font(const char *family, float px);
color fm_host_wallpaper_tone(void);      /* average wallpaper colour */
/* Tray elements (volume, network, notifications, quicksettings; tray.c):
 * width beside the icon for their label, and drawing them in `col`. */
float fm_host_tray_extra(const fm_node *n);
void fm_host_draw_tray(fm_node *n, rect r, color col, float op);

/* ---- for the AI ---- */

const char *fm_reference(void);          /* element and property reference */
const char *fm_examples(void);           /* a few very different documents */
bool fm_parse_color(const char *s, color *out);
uint64_t fm_host_time_ms(void);         /* for animations in <canvas> */
