#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../lib/ai.h"
#include "design.h"
#include "font.h"
#include "gfx.h"
#include "sys.h"
#include "textedit.h"

#define MAX_MSGS 160
#define STATE_DIR "/etc/os"

enum screen { SCR_SPLASH, SCR_CHOOSE, SCR_MODELS, SCR_CONNECT, SCR_ONBOARD, SCR_DESIGNING, SCR_DESKTOP };

struct msg {
    char role[12]; /* "user", "assistant", or "hidden" (sent to the AI, not shown) */
    char *text;
};

struct pick {
    bool active;
    char kind[16];
    int n;
    char label[6][48];
    char value[6][96];
    color swatch[6][4];
    bool has_swatch[6];
    char desc[6][140];   /* rooms: a line about the place */
    int wp_kind[6];      /* rooms: the wallpaper that paints it */
    char font[6][32];    /* type: the face each sample is set in */
    char sentence[160];  /* type: their words */
    canvas *thumb[6];    /* rooms: a living thumbnail */
};

struct textfield {
    char buf[1024];
    int len;
    bool secret;
    text_edit ed; /* caret, selection and undo (textedit.h) */
};

struct conversation {
    struct msg msgs[MAX_MSGS];
    int n;
    job *pending;
    uint64_t pending_since;
    char error[300];
    int errors; /* consecutive failures; reset by a successful reply */
    char *picture[3]; /* the newest pictures (JPEG, base64), sent with message picture_at to a model that sees */
    int picture_at;   /* -1: none */
};

enum window_id { WIN_FILES, WIN_COMPANION, WIN_NOTES, WIN_SETTINGS, WIN_ABOUT, WIN_TERMINAL, WIN_PREVIEW, WIN_COUNT };
/* After the built-in windows: one slot per Linux app window (Wayland). */
#define WIN_APPS 16
#define WIN_MAX (WIN_COUNT + WIN_APPS)

struct window {
    bool open;
    rect r;
    int z;
    struct wl_view *view; /* app windows: what the app drew */
    struct fxwin *fx;     /* or a Facet app running in the shell (fxwin.c) */
    rect body;            /* where the content went last frame */
    bool fitted;          /* sized to the app's first frame */
    int asked_w, asked_h; /* the size last asked of the app */
    uint64_t opened_at; /* for the arrival animation */
    bool minimized, maximized;
    bool on_top;        /* kept above the others (its right-click menu's "Keep on top") */
    rect restore;       /* size before maximizing */
    int glass_said;     /* (what the log was last told of its frame's glass) */
    bool body_opaque;   /* last frame, nothing behind showed through its body (the layer below may skip it) */
};

typedef struct app {
    canvas *frame, *wall;
    int W, H;
    input_state in;
    enum screen screen;
    uint64_t now, screen_since;
    bool dirty;   /* something changed that the next frame must show (set by anyone) */
    bool face_new; /* a new picture of the companion's face came (presence.c): a frame, but no fading after it */
    bool drawing; /* this frame is drawn (main.c: only when something may have changed) */

    struct ai_config cfg;
    const struct ai_provider *provider;

    /* choose / connect */
    int choose_scroll;
    struct textfield url, key;
    int focus; /* 0 url, 1 key */
    job *connect_job;
    char connect_status[300];
    char **models;
    int nmodels, model_sel, model_scroll;

    /* onboarding */
    struct conversation onboard;
    struct textfield reply;
    struct pick pick;
    profile prof;
    bool onboard_done;
    int design_attempt;
    char design_instruction[600]; /* what the person asked for, kept across retries */
    int onboard_nudges;           /* times we asked the AI to cover what it skipped */
    job *design_job;
    char design_problems[300];

    /* desktop */
    design d;
    bool have_design;
    bool menu_open;
    bool bar_focused, in_hot_corner;
    struct textfield bar;
    struct window win[WIN_MAX];
    int ztop;
    int drag_win, drag_dx, drag_dy;
    char files_path[512];
    int files_scroll;
    struct conversation companion;
    struct textfield chat;
    struct textfield notes;
    char notes_path[512]; /* the file Notes is editing (/root/notes.txt by default) */
    bool notes_readonly;  /* opened a file too long to edit here */
    char toast[240];
    uint64_t toast_until;
    bool redesigning;
    bool confirm_power;
    bool power_restart;
    struct fm_doc *ui;  /* the desktop as the AI wrote it (Facet markup) */
    int empty_replies; /* consecutive empty answers from the AI */
    uint64_t companion_at; /* when to open the companion after the reveal (0: no) */
    bool returning;        /* booted into a desktop they already had: welcome them back */
    bool restarted;        /* ...or only the desktop program started again (the computer stayed on): no welcome */
    char caption[800];     /* the companion's words as a subtitle, when its window is closed */
    uint64_t caption_until;
} app;

extern app A;

/* Event log on the console (serial). Used by tests/demo.py to find where
 * things are on screen, and handy when debugging on real hardware. */
#include <stdio.h>
/* Times a step of the frame for the log's "time:" line (main.c). */
uint64_t prof_now_us(void);
void prof_add(const char *name, uint64_t us);
#define PROF(name, stmt)                                                                                         \
    do {                                                                                                         \
        uint64_t prof_t0_ = prof_now_us();                                                                       \
        stmt;                                                                                                    \
        prof_add(name, prof_now_us() - prof_t0_);                                                                \
    } while (0)
#define LOG(...) fprintf(stderr, "shell: " __VA_ARGS__)
#define LOGRECT(name, r) LOG("rect %s %d %d %d %d\n", name, (r).x, (r).y, (r).w, (r).h)

uint64_t now_ms(void);
void set_screen(enum screen s);
/* The release this build is (shown in About; the image is dist/Operating-System-<it>.iso). */
#define OS_VERSION "0.10.0"

void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Apps were installed or removed (Flatpak): read the list again (desktop.c). */
void desktop_apps_changed(void);

/* conversation helpers (onboard.c) */
void convo_add(struct conversation *c, const char *role, const char *text);
void convo_send(struct conversation *c, const char *system, int max_tokens);
/* Returns true when a reply arrived (appended) or failed (c->error set). */
bool convo_poll(struct conversation *c);
/* Puts pictures on the newest message (they replace any before; NULLs are fine; it takes them). */
void convo_picture(struct conversation *c, char *a, char *b);
void convo_picture3(struct conversation *c, char *a, char *b, char *z);
void convo_clear(struct conversation *c);
void convo_save(const struct conversation *c, const char *path);
int convo_load(struct conversation *c, const char *path); /* returns messages read */
void strip_markdown(char *s);

/* widgets (ui.c) */
bool ui_button(rect r, const char *label, font *f, color bg, color fg, float radius, bool enabled);
bool ui_hover(rect r);
void ui_textfield(rect r, struct textfield *t, bool focused, const char *placeholder, font *f, color bg, color fg,
                  color muted, float radius);
/* Feeds this frame's typed keys into the field; returns true on Enter. */
bool ui_type(struct textfield *t);
/* The right-click menu of a text field: its input first in the frame, drawn over everything at the end. */
void ui_text_menu_input(void);
void ui_text_menu_draw(void);
/* The right-click menu of anything on the desktop (textmenu.c draws it, desktop.c fills it and does what is
 * chosen): opened at a point, items added, each a label and what choosing it does. */
void ctx_menu_open(int x, int y);
void ctx_menu_add(const char *label, const char *act, bool danger);
/* What it is for (for="..." on the design's <menu>, which a design styles): app, window, desktop, text, file. */
void ctx_menu_for(const char *kind);
void ctx_menu_target(int x, int y, int w, int h); /* where the thing clicked is (a design's placement="on") */
void ctx_menu_sample_into(struct fm_doc *d, int x, int y, const char *kind); /* shown open in a preview (preview.c) */
/* An action with all it carries (id "" or NULL: from what it does or its name; icon and desc may be NULL). */
bool ctx_menu_action(const char *id, const char *label, const char *act, int flags, const char *keys,
                     const char *icon, const char *desc);
enum { CM_DANGER = 1, CM_OFF = 2, CM_ON = 4, CM_QUIET = 8, CM_SEP = 16 }; /* removes / can't now / chosen / small / a line before */
void ctx_menu_item(const char *label, const char *act, int flags, const char *keys);
void ctx_menu_note(const char *note);
bool ctx_menu_showing(void);
void ctx_menu_input(void);
void ctx_menu_draw(void);
void ctx_menu_take(int i);        /* what="menu:N" in the design's menu (desktop.c's fm_host_action) */
void ctx_menu_after_input(void);  /* after the design took the pointer: the arrows' choice shown */
/* For the screen reader: the menu up, and what is chosen in it (under the pointer or by the keys); false if none. */
bool ctx_menu_reading(char *out, size_t n);
int desktop_focused_window(void);
const char *desktop_window_title(int w);
void desktop_ctx_chosen(const char *act);
void text_menu_take(int i);                /* a text field's item, picked from the design's menu */
void files_menu_take(const char *what);    /* Files' (files.c) */
void menus_check_styled(void);             /* the design has no menu of its own yet: the companion writes it */
void menus_tick(void);                     /* (menus.c) its answer, each frame */
void ui_orb(canvas *c, float cx, float cy, float r, color a, color b, bool active);
void ui_cursor(canvas *c, int x, int y);
void ui_backdrop(color base, color l1, color l2, color depth);
void ui_icon(canvas *c, rect r, const design *d, int app, bool hover);
color readable_on(color bg);
/* A Flatpak app ID ("org.gimp.GIMP"): letters, digits, '.', '_' and '-', with a dot, nothing a shell would read
 * as anything else. Every ID put into a command passes this first. */
static inline bool flatpak_id_ok(const char *id) {
    int n = 0, dots = 0;
    for (const char *p = id; p && *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
        dots += c == '.';
    }
    return n > 2 && n < 256 && dots >= 1 && id[0] != '-' && id[0] != '.';
}
/* Draws an app's symbol (also used for folders = APP_FILES, documents = APP_NOTES). */
void icon_symbol(canvas *c, rect box, int app, float stroke, color col);

/* screens */
void choose_draw(void);
void connect_draw(void);
void models_draw(void); /* onboarding: choosing a model that runs on this computer (onboard.c) */
void models_draw(void); /* onboarding: a model on this computer (onboard.c) */
void onboard_start(void);
bool onboard_resume(void); /* a start in the middle of onboarding: back where it was (false: nothing kept) */
void onboard_keep(void);   /* once a frame: its place kept as it changes */
void onboard_forget(void); /* finished, or started over */
void onboard_draw(void);
void designing_draw(void);
void design_begin(const char *extra_instruction);
void desktop_enter(void);
void desktop_draw(void);
void desktop_set_ui(struct fm_doc *doc);
/* A frame that brings only the top app's new picture (desktop.c): after a full frame, whether its window is
 * nothing but that picture on the screen; then whether the next can be just the picture, and putting it there
 * (false: draw the whole frame). */
void desktop_note_clean(void);
bool desktop_app_in_front(void); /* the window in front is an app's (the person is working in it) */
bool desktop_app_frame_ready(void);
bool desktop_app_frame_put(rect *out);
/* A design takes over the desktop (from the preview): it morphs out of the
 * old one. `first`: the end of onboarding, the desktop arrives. */
void desktop_adopt(struct fm_doc *doc, const design *d, bool first);
void desktop_open_window(int w);
/* Another window (not the companion's) is in front: they are using something. Nothing of the companion's own
 * accord opens over it, so the keyboard stays theirs. */
bool desktop_window_in_use(void);
/* One frame of the whole OS; and frames with `draw` over everything until *done (main.c). */
void main_frame(void);
void main_modal(void (*draw)(void), const bool *done);
/* Erases everything kept on the disk, apps too, and restarts at the very first screen. */
void factory_reset(void);
/* The design's own spot for it on the screen (its <ai> or <avatar>), if it has one showing. */
bool desktop_her_spot(rect *r);
/* Their own name as they gave it (empty until then), kept apart from any design; and keeping it. */
const char *their_name(void);
void their_name_keep(const char *name);
/* The companion (desktop.c): their words, sent; a note it should act on
 * (`act`: its ui tags run); and its replies taken in, off the desktop. */
void companion_tell(const char *text);
void companion_note(const char *note, bool act);
/* Something it must know before it next speaks that needs no answer (they applied the preview themselves). */
void companion_knows(const char *note);
void facet_reference_write(void); /* Facet 2's reference, as a file the companion can read (desktop.c) */
void companion_note_picture(const char *note, bool act, char *picture); /* picture: base64 JPEG, taken */
void companion_note_pictures(const char *note, bool act, char *a, char *b, char *z);
/* What [[write]] tags did in the reply just taken (actions.c); read once. */
const char *actions_report(void);
void companion_tick(void);
/* The design job (onboard.c): takes in a finished design, a failure, or
 * starts a retry. */
void design_poll(void);

/* The symbol of what an app is, by its id or name (enum ui_glyph); -1 if nothing fits (desktop.c). */
int app_glyph_for(const char *id, const char *label);
