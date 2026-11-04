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
struct text_edit;
/* An <input>/<search> field's editor: caret, selection, undo (made on first use). */
struct text_edit *fmi_editor(fm_node *n);

/* ---- values ---- */

enum fm_unit { FM_AUTO, FM_PX, FM_PCT, FM_FULL };

typedef struct fm_len {
    enum fm_unit unit;
    float v;
} fm_len;

enum fm_bg_kind { FM_BG_NONE, FM_BG_SOLID, FM_BG_LINEAR, FM_BG_RADIAL, FM_BG_CONIC };
#define FM_STOPS 12

typedef struct fm_bg {
    enum fm_bg_kind kind;
    color a, b;  /* the first and last colours (what animations blend between) */
    float angle; /* degrees: linear 180 = top to bottom; conic: where it starts, 0 = up */
    /* gradients: their stops, positions 0..1 of the way (or of one repeat) */
    int nstops;
    color stop_c[FM_STOPS];
    float stop_p[FM_STOPS];
    bool repeating;
    bool circle;      /* radial: a circle rather than an ellipse fitting the box */
    float cx, cy;     /* radial and conic: the centre, 0..1 of the box */
} fm_bg;

/* ---- computed style ---- */

enum fm_dir { FM_ROW, FM_COLUMN };
enum fm_mode { FM_FLEX, FM_GRID, FM_STACK, FM_ORBIT };
enum fm_align { FM_A_STRETCH, FM_A_START, FM_A_CENTER, FM_A_END };
enum fm_justify { FM_J_START, FM_J_CENTER, FM_J_END, FM_J_BETWEEN, FM_J_AROUND, FM_J_EVENLY };
enum fm_overflow { FM_OV_VISIBLE, FM_OV_CLIP, FM_OV_SCROLL };
enum fm_textalign { FM_T_START, FM_T_CENTER, FM_T_END };

/* A side's border width: 0 top, 1 right, 2 bottom, 3 left */
#define FM_BW(st, k) ((st)->bsides ? (st)->bside[k] : (st)->border_w)

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
    float border_w;           /* every side's (the widest, when they differ) */
    color border;
    fm_bg border_bg;          /* gradient borders */
    bool bsides;              /* the sides differ: bside/bcol hold each (top right bottom left) */
    float bside[4];
    color bcol[4];
    char bstyle;              /* 0 solid, 'd'ashed, 'o' dotted, '2' double, 'i'nset, 'u' outset, 'r'idge, 'g'roove */
    int ninset;               /* inset box-shadows without blur: edges drawn inside the border (bevels) */
    float inset_x[6], inset_y[6];
    color inset_c[6];
    float radius[4];          /* tl tr br bl */
    float shadow_x, shadow_y, shadow_blur;
    color shadow;
    /* every outer box-shadow, the first one on top (the first is also shadow_* above, for animation) */
    int nshadow;
    float sh_x[4], sh_y[4], sh_blur[4], sh_spread[4];
    color sh_c[4];
    /* text-shadows after the first */
    int ntsh;
    float tsh_x[3], tsh_y[3], tsh_blur[3];
    color tsh_c[3];
    float glow_size;
    color glow;
    float opacity;

    color color;              /* text and icon colour */
    char font[32];
    float font_size;
    enum fm_textalign text_align;
    short text_rot;           /* text turned: 90 reads top to bottom, -90 bottom to top, 180 upside down (inherited) */
    bool crisp_text;          /* font-smooth: none: every pixel of a letter on or off (inherited) */
    char cursor[64];          /* the pointer over it: arrow, pointer, text, ..., or url(file) x y (inherited) */
    color sb_thumb, sb_track; /* scrollbar-color (0: from the text colour) */
    float sb_width;           /* scrollbar-width: px (thin 6, auto 10), -1 none */
    char decoration;          /* text-decoration: 'u'nderline, 'l'ine-through, 'o'verline (drawn by rich text) */
    /* animation: name duration [timing] [delay] [count|infinite] [direction] [fill] (@keyframes) */
    char anim_name[40];
    float anim_dur, anim_delay, anim_count; /* ms, ms, times (-1 forever) */
    char anim_dir;            /* 'n'ormal, 'r'everse, 'a'lternate, 'A'lternate-reverse */
    char anim_ease;           /* 'l'inear, 'e'ase, 'i'n, 'o'ut, 'b'oth (in-out), 's'teps */
    int anim_steps;
    float anim_bez[4];        /* cubic-bezier(...) */
    char anim_fill;           /* 'n'one, 'f'orwards, 'b'ackwards, 'B'oth */
    bool pixelated;           /* image-rendering: pixelated: pictures scaled by whole pixels, never blurred */
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
    float rotate;             /* degrees, the whole box turned about its centre (drawn turned; laid out upright) */
    float skew_x, skew_y;     /* degrees */
    /* filter: what is drawn of it, changed (all 1 / 0 when none) */
    bool has_filter;
    float f_blur, f_bright, f_contrast, f_gray, f_sepia, f_sat, f_hue, f_invert, f_opacity;
    char blend;               /* mix-blend-mode: 0 normal, 'm'ultiply, 's'creen, 'o'verlay, 'd'arken, 'l'ighten,
                               * 'D'ifference, 'x' exclusion, 'c'olor-dodge, 'b' color-burn, 'h'ard-light, 'S'oft-light */
    /* clip-path: 'c'ircle, 'e'llipse, 'i'nset, 'p'olygon; numbers as fractions of the box (or px when > 1.5 for inset) */
    char clip;
    int clip_n;
    float clip_v[34];
    fm_bg mask;               /* mask-image: a gradient whose opacity is how much of it shows */
    /* background pictures: background(-image): url(...), with -size, -position and -repeat */
    char bg_img[160];
    bool bg_img_over;         /* listed after a gradient: the gradient is drawn over the picture */
    char bg_size;             /* 'a'uto, 'c'over, 'C'ontain, 'p'x (bg_sw x bg_sh), 'f' a fraction */
    float bg_sw, bg_sh, bg_px, bg_py; /* size; where (0..1 of the free room) */
    char bg_repeat;           /* 'r' both, 'x', 'y', 'n'o */
    float tx, ty;             /* translate */
    float tx_pct, ty_pct;     /* translate in % of its own size (translateX(-50%) centring) */
    float transition;         /* ms */
    float z;

    float icon_size;
    char icon_style[12];      /* filled | line | glow | duotone | plain */
    char icon_shape[12];      /* circle | squircle | square | none */
    int columns;              /* grid: a count (when no template) */
    char grid_cols[120], grid_rows[120]; /* grid-template-columns / -rows, as written (sized at layout) */
    char grid_areas[240];     /* grid-template-areas: "head head" "side main" */
    float row_gap, col_gap;   /* -1: the gap */
    /* as a grid item: lines (1-based, 0 auto, negative from the end), spans, or a named area */
    short gc0, gc1, gr0, gr1, gcs, grs;
    char grid_area[20];
    /* anchor positioning: this element is an anchor (anchor-name: --x); or it is placed against one
     * (position-anchor: --x, with anchor() in top/right/bottom/left, position-area, anchor-size()) */
    char anchor_name[32], pos_anchor[32];
    char anc_side[4];         /* for top, right, bottom, left: the anchor's 't','r','b','l','c'entre, 0 none */
    float anc_off[4];         /* + / - px after it */
    char anc_of[4][32];       /* anchor(--other bottom): another anchor for that side */
    char area_v, area_h;      /* position-area: 't','c','b' / 'l','c','r' ('T' 'B' 'L' 'R': spanning toward) */
    char anc_w, anc_h;        /* width / height: anchor-size(width|height) */
    bool flip_block, flip_inline; /* position-try-fallbacks */
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
    bool generated;         /* made by a live element (applist, windowlist), or ::before / ::after */
    char pseudo;            /* 'b' a ::before, 'a' an ::after (generated from the style) */
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
    struct text_edit *ed;           /* <input>/<search>: caret, selection, undo (textedit.h) */
    float mw, mh;                   /* measured preferred size */
    bool laid_out;
    int content_h;                  /* height of the children, for scrolling */
    struct fm_rich *rich;           /* a paragraph of rich text: its words, placed (fm/rich.c) */
    bool rich_on;                   /* laid out as rich text this frame */
    char anim_was[40];              /* the animation it is running, and since when (ms) */
    double anim_t0;
    rect rich_at;                   /* where its words start */
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
    rect occ[24];           /* what windows hide completely, while the layer below them is drawn (fm_set_occluders) */
    int nocc;
    struct fm_rule *rules;
    int nrules;
    char *vars[64][2];      /* custom properties: name, value */
    int nvars;
    char *css;              /* stylesheet source (the <style> text) */
    fm_problem problems[FM_MAX_PROBLEMS];
    int nproblems;
    int next_id;
    bool has_pseudo;        /* some rule styles a ::before or ::after */
    int media_w, media_h;   /* the size @media rules were judged at */
    struct fm_keyframes *kf; /* @keyframes */
    int nkf;
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
/* The pointer the design asks for at x, y ("" for the system's own) */
const char *fm_cursor(fm_doc *d, int x, int y);
/* Something is animating (@keyframes): the host keeps frames coming */
extern bool fm_animating;
/* The size @media rules are judged by (the screen); a change reloads the sheet */
extern int fm_media_w, fm_media_h;
/* After a patched rule for `sel`: the rules that still win on n for its properties, named in out. */
int fm_css_who_wins(fm_doc *d, fm_node *n, const char *sel, char *out, size_t cap);
/* Takes a selector's rules (or some of their properties: "height; width") out of the design's own sheet. */
int fm_css_unstyle(fm_doc *d, const char *selector, const char *props);
/* Resolves styles for the whole tree for the current states. */
void fm_style_tree(fm_doc *d);
const char *fm_var(const fm_doc *d, const char *name);
/* Light and dark, set by the shell (theme.c) before fm_css_load: which
 * colour-scheme blocks apply (@dark / @light / @media (prefers-color-scheme:
 * ...)): 0 none, 1 light, 2 dark. And, when the document has no block for
 * the scheme wanted, a mapping the shell applies to every colour it names. */
extern int fm_scheme;
/* The person's clock: 12-hour (3:05 pm) when true, else 24-hour (15:05). fm_strftime is strftime with their
 * choice: every hour a format writes as %H (or %R, %T) becomes 12-hour, with am/pm after the time. */
extern bool fm_clock_12h;
struct tm;
void fm_strftime(char *buf, size_t len, const char *fmt, const struct tm *tm);
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
void fm_render_window_frame(fm_doc *d, canvas *c, int win, rect r, bool focused, rect *content_out, int opaque_w,
                            int opaque_h); /* opaque_w 0: an app fills its content next (no glass blur there); >0: its opaque picture's size; -1: none */
int fm_layer_of(const fm_node *n);
/* What the windows hide completely, for the next fm_render_layer(0): what lies wholly inside one isn't drawn
 * (NULL, 0: nothing hidden). */
void fm_set_occluders(fm_doc *d, const rect *r, int n);
/* Whether the design's window frame paints itself opaque (solid, no glass, not faded): its radius out. */
bool fm_frame_opaque(fm_doc *d, float *radius);

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
/* <avatar frame="rect|circle" tint="none|codec">: its live portrait (the light when it has no face) */
/* shot="face|bust|waist|full": how close the camera is */
void fm_host_draw_avatar(rect r, const char *frame, const char *tint, const char *shot, bool active);
void fm_host_draw_icon(rect r, int app, const fm_style *st, bool hover);
font *fm_host_font(const char *family, float px);
color fm_host_wallpaper_tone(void);      /* average wallpaper colour */
/* The darkest and lightest of the wallpaper under r (false if there is no picture of it). */
bool fm_host_wallpaper_range(rect r, color *dark, color *light);
/* Tray elements (volume, network, notifications, quicksettings; tray.c):
 * width beside the icon for their label, and drawing them in `col`. */
float fm_host_tray_extra(const fm_node *n);
void fm_host_draw_tray(fm_node *n, rect r, color col, float op);

/* ---- for the AI ---- */

const char *fm_reference(void);          /* element and property reference */
const char *fm_examples(void);           /* a few very different documents */
bool fm_parse_color(const char *s, color *out);
uint64_t fm_host_time_ms(void);         /* for animations in <canvas> */

/* A canvas script outside any design (the companion's emblem, canvas.c):
 * draw(w, h, t) into r, with globals state, level, accent and accent2.
 * False (err says why) when it won't run; it isn't tried again until the
 * script changes. */
bool fm_canvas_emblem(const char *src, canvas *cv, rect r, const char *state, float level, color a, color b,
                      char *err, size_t errlen);
