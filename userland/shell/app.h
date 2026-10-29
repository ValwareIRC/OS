#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../lib/ai.h"
#include "design.h"
#include "font.h"
#include "gfx.h"
#include "sys.h"

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
};

struct conversation {
    struct msg msgs[MAX_MSGS];
    int n;
    job *pending;
    uint64_t pending_since;
    char error[300];
    int errors; /* consecutive failures; reset by a successful reply */
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
    rect body;            /* where the content went last frame */
    bool fitted;          /* sized to the app's first frame */
    int asked_w, asked_h; /* the size last asked of the app */
    uint64_t opened_at; /* for the arrival animation */
    bool minimized, maximized;
    rect restore;       /* size before maximizing */
};

typedef struct app {
    canvas *frame, *wall;
    int W, H;
    input_state in;
    enum screen screen;
    uint64_t now, screen_since;
    bool dirty;

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
    char caption[800];     /* the companion's words as a subtitle, when its window is closed */
    uint64_t caption_until;
} app;

extern app A;

/* Event log on the console (serial). Used by tests/demo.py to find where
 * things are on screen, and handy when debugging on real hardware. */
#include <stdio.h>
#define LOG(...) fprintf(stderr, "shell: " __VA_ARGS__)
#define LOGRECT(name, r) LOG("rect %s %d %d %d %d\n", name, (r).x, (r).y, (r).w, (r).h)

uint64_t now_ms(void);
void set_screen(enum screen s);
/* The release this build is (shown in About; the image is dist/Operating-System-<it>.iso). */
#define OS_VERSION "0.7.5"

void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Apps were installed or removed (Flatpak): read the list again (desktop.c). */
void desktop_apps_changed(void);

/* conversation helpers (onboard.c) */
void convo_add(struct conversation *c, const char *role, const char *text);
void convo_send(struct conversation *c, const char *system, int max_tokens);
/* Returns true when a reply arrived (appended) or failed (c->error set). */
bool convo_poll(struct conversation *c);
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
void ui_orb(canvas *c, float cx, float cy, float r, color a, color b, bool active);
void ui_cursor(canvas *c, int x, int y);
void ui_backdrop(color base, color l1, color l2, color depth);
void ui_icon(canvas *c, rect r, const design *d, int app, bool hover);
color readable_on(color bg);
/* Draws an app's symbol (also used for folders = APP_FILES, documents = APP_NOTES). */
void icon_symbol(canvas *c, rect box, int app, float stroke, color col);

/* screens */
void choose_draw(void);
void connect_draw(void);
void models_draw(void); /* onboarding: choosing a model that runs on this computer (onboard.c) */
void models_draw(void); /* onboarding: a model on this computer (onboard.c) */
void onboard_start(void);
void onboard_draw(void);
void designing_draw(void);
void design_begin(const char *extra_instruction);
void desktop_enter(void);
void desktop_draw(void);
void desktop_set_ui(struct fm_doc *doc);
/* A design takes over the desktop (from the preview): it morphs out of the
 * old one. `first`: the end of onboarding, the desktop arrives. */
void desktop_adopt(struct fm_doc *doc, const design *d, bool first);
void desktop_open_window(int w);
/* The companion (desktop.c): their words, sent; a note it should act on
 * (`act`: its ui tags run); and its replies taken in, off the desktop. */
void companion_tell(const char *text);
void companion_note(const char *note, bool act);
void companion_tick(void);
/* The design job (onboard.c): takes in a finished design, a failure, or
 * starts a retry. */
void design_poll(void);
