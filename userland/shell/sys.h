#pragma once
/* The person's home, /home/<their account> ($HOME, set by init), and paths in it: home_path("/Documents").
 * home_path's result lives in one of eight buffers used in turn (enough for any one expression). */
const char *home_dir(void);
const char *home_path(const char *rest);
/* their account's name ("valerie"; "user" until they have told it), and whether a string can be one (home.c) */
const char *account_name(void);
#include <stdbool.h>
bool account_name_ok(const char *s);
void home_forget(void); /* (init: the account's name may have come back with their state) */
#include <stdbool.h>
#include "gfx.h"

/* ---- display: /dev/fb0 ---- */
bool display_open(void);
int display_width(void);
int display_height(void);
void display_present(const canvas *frame);
/* Resolution changes (Bochs/QEMU std VGA): whether a mode would work, and
 * switching to it (the framebuffer is mapped again; frames must follow). */
bool display_mode_possible(int w, int h);
bool display_set_mode(int w, int h);

/* ---- input: /dev/input/event0 (keys), event1 (pointer) ---- */
enum {
    K_NONE = 0,
    K_ENTER = 0x100, K_BACKSPACE, K_ESC, K_TAB, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_DELETE, K_HOME, K_END,
    K_PGUP, K_PGDN, K_META,
    K_PASTE, /* Ctrl+V or Shift+Insert: insert clipboard_text() */
    K_TALK,  /* Ctrl+Space: start or stop talking to the companion */
    K_PRINT, /* Print Screen: the capture bar (screenshots and recording) */
};

enum { KM_CTRL = 1, KM_SHIFT = 2 };

typedef struct input_state {
    int mx, my;        /* pointer position in screen pixels */
    bool down;         /* left button held */
    bool clicked;      /* left button released this frame (a click) */
    bool pressed;      /* left button went down this frame */
    bool right_clicked;
    int wheel;         /* scroll steps this frame */
    int keys[32];      /* characters (Unicode) and K_* codes typed this frame */
    uint8_t keymod[32]; /* KM_CTRL, KM_SHIFT: what was held with each (a quick chord can start and end in one frame) */
    int nkeys;
    bool ctrl, shift;
    bool moved;
    struct {
        uint16_t code; /* Linux KEY_* */
        bool down;     /* press or repeat; false: release */
    } raw[32];         /* every key event this frame, for Linux apps */
    int nraw;
} input_state;

bool input_open(int screen_w, int screen_h);
void input_set_screen(int screen_w, int screen_h); /* after a resolution change */
/* A held key types again after delay_ms, then rate_hz times a second. */
void input_set_repeat(int delay_ms, int rate_hz);
/* The mouse (Settings > Mouse): speed 0.2 to 3 (1: as the mouse says), acceleration on or off, scrolling that moves
 * the content with the fingers ("natural"), and the buttons swapped for the left hand. */
void input_set_mouse(float speed, bool accel, bool natural, bool left_handed);
/* whether the pointer is a tablet or a virtual machine's (absolute: speed means nothing to it) */
bool input_pointer_absolute(void);
int input_fds(int fds[2]);
/* Drains pending events into `in` (per-frame fields are reset first). */
void input_poll(input_state *in);
/* This frame's input withheld: no clicks, keys or wheel, the pointer nowhere (a dialog above has it). */
void input_clear(input_state *in);
/* Text from the host computer's clipboard (VMs), or "" if none. */
const char *clipboard_text(void);
/* Copies text inside this computer (Notes' copy and cut); the next copy on
 * the host replaces it, as the most recent copy should. */
void clipboard_set(const char *text);

/* ---- background work: AI calls run in a child process ---- */
typedef struct job job;
typedef void (*job_fn)(void *arg, char **result);   /* runs in the child */
job *job_start(job_fn fn, void *arg);
int job_fd(job *j);
/* Returns true when finished; *result is malloc'd (may be NULL). */
bool job_poll(job *j, char **result);
void job_free(job *j);

/* ---- the person's session ---- */
/* Apps run as an ordinary user, not as root (Chromium, for one, won't
 * sandbox itself as root); the session's runtime directory is theirs. */
#define SESSION_UID 1000
#define SESSION_RUNTIME "/run/user/1000"
/* In a child about to start an app: from root to the session's user. */
void become_session_user(void);

/* ---- apps installed with Flatpak ---- */
/* What they export (.desktop entries, icons): the image's own installation
 * (Prism) and the disk's (apps installed from Flathub, kept across restarts). */
#define FLATPAK_SYSTEM_EXPORTS "/var/lib/flatpak/exports/share"
#define FLATPAK_DISK_EXPORTS "/var/lib/os-data/flatpak/exports/share"
/* The disk's installation is there (mounted and set up by init). */
#define FLATPAK_DISK_READY "/var/lib/os-data/flatpak/repo/config"
