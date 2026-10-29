#pragma once
/* The companion's senses and hands.
 *
 *   senses.c   what it can perceive: the real PC through the host senses
 *              service (temperatures, fans, RGB lights), this computer
 *              through /proc, and the shell's own state (windows, activity,
 *              the hour). Each sense can be switched off by the person.
 *   alive.c    what it makes of that: a short world model for every prompt,
 *              notable events, speaking up on its own (rarely, kindly), the
 *              orb's mood, ambient lighting, and the "what I can sense" panel.
 *   actions.c  what it can do: [[light:]], [[set:]], [[notify:]], [[run:]]
 *              (always confirmed on screen), [[look:]] (read-only questions
 *              it asks the system mid-reply), [[undo]]; all logged.
 */
#include "app.h"

#include <stdbool.h>
#include <stddef.h>

/* ---- senses.c ---- */

enum sense {
    SENSE_HARDWARE, /* the real PC: temperatures, load, fans */
    SENSE_LIGHTS,   /* the PC's RGB devices */
    SENSE_SYSTEM,   /* this computer: load, memory, busy programs */
    SENSE_WINDOWS,  /* open windows, what is in front, what is on screen */
    SENSE_ACTIVITY, /* whether they are here, typing, or away */
    SENSE_NETWORK,  /* online or not */
    SENSE_TIME,     /* the hour and the date */
    SENSE_FILES,    /* reading a file or folder when asked */
    SENSE_COUNT
};

struct sense_info {
    const char *key;  /* in /etc/os/alive.conf */
    const char *name; /* for the person */
    const char *what; /* what it means, plainly */
};
extern const struct sense_info sense_info[SENSE_COUNT];
bool sense_on(enum sense s);
void sense_set(enum sense s, bool on);

#define HOST_MAX_GPUS 4
#define HOST_MAX_FANS 12
#define HOST_MAX_LIGHTS 24
#define HOST_MAX_MISSING 8
#define TOP_PROCS 5

struct host_light {
    char id[64], name[64], kind[16], vendor[32];
    char color[8]; /* "#rrggbb", or "" if unknown */
    int leds;
    bool controllable;
};

/* Readings: a value below -900 means "not known" (the service sent null). */
#define UNKNOWN (-999.0)

/* The real PC, through the host senses service (http://10.0.2.2:4571). */
struct host_senses {
    bool up;                /* the service answered recently */
    uint64_t seen;          /* when it last answered (ms) */
    char name[64];
    char cpu_model[96];
    double cpu_load, cpu_temp, cpu_mhz;
    struct {
        char name[64];
        double load, temp, fan_pct, power_w, vram_used, vram_total;
    } gpu[HOST_MAX_GPUS];
    int ngpu;
    double mem_used_mb, mem_total_mb;
    struct {
        char name[48];
        double rpm, pct;
    } fan[HOST_MAX_FANS];
    int nfan;
    struct host_light light[HOST_MAX_LIGHTS];
    int nlight;
    char missing[HOST_MAX_MISSING][160];
    int nmissing;
    char why_down[160];     /* why it isn't answering, plainly */
    char light_result[160]; /* how the last change to the lights went */
};

struct senses {
    struct host_senses host;

    /* this computer */
    double vm_cpu;                /* 0..100 over the last sample */
    double vm_mem_used_mb, vm_mem_total_mb;
    struct {
        int pid;
        char name[32];
        double cpu; /* % of one core */
        double rss_mb;
    } top[TOP_PROCS];
    int ntop;
    int nprocs;

    /* them */
    uint64_t last_input, last_key, session_start; /* ms */
    bool net_up, net_known;
};

const struct senses *senses_now(void);
void senses_tick(void); /* every frame; does real work a few times a minute */
/* The compact "what I can sense right now" lines for a prompt. */
void senses_write(char *out, size_t n);
/* Answers a [[look:WHAT]]; returns false if it doesn't know WHAT. */
bool senses_look(const char *what, char *out, size_t n);
/* bind="sense.cpu_temp" etc. for Facet designs. */
bool senses_bind(const char *key, char *out, size_t n);
/* Lights on the real PC: queued, sent by the background poller. */
void senses_lights_post(const char *path, const char *json_body);
/* Gives the lights back to the vendor apps, waiting briefly (for exit). */
void senses_lights_restore_now(void);
/* Notifications, so the companion knows what came in (the tray calls this). */
void senses_note_notification(const char *title, const char *body, const char *app);
int senses_notifications(char out[][200], int max);
bool senses_typing(void); /* a key in the last few seconds */
bool senses_fullscreen(void);

/* ---- alive.c ---- */

void alive_tick(void);  /* every desktop frame, before drawing */
void alive_draw(void);  /* overlays: the senses panel and the run question */
/* What the companion senses, what happened lately and what it can do, for
 * its system prompt. */
const char *alive_prompt(void);
const char *alive_onboarding_note(void);
/* Something notable happened. level 0: worth remembering; 1: worth
 * mentioning; 2: worth mentioning soon; 3: urgent. kind groups cooldowns. */
void alive_event(int level, const char *kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void alive_orb(color *a, color *b, bool *active);
bool alive_host_action(const char *action); /* "senses", "quiet:on", ... from Facet */
void alive_shutdown(void);
/* The senses panel, drawn into r (for Settings and <view app="senses"/>). */
int alive_panel(rect r);
void alive_panel_open(bool open);

struct alive_prefs {
    bool proactive;     /* may speak up on its own */
    bool quiet;         /* "be quiet": nothing unless it's urgent */
    bool lights_follow; /* the PC's RGB takes the desktop's colours */
    bool lights_show;   /* the keyboard pulses while it thinks or talks */
};
extern struct alive_prefs alive_prefs;
void alive_prefs_save(void);

/* ---- actions.c ---- */

/* Acts on the senses-and-hands tags in a fresh reply. Returns true if it
 * asked the model again (a [[look:]]): the caller stops there. May set
 * *from_person when the reply continues something the person asked. */
bool actions_take(struct conversation *c, bool *from_person);
void actions_draw(void);          /* the "run this?" card */
bool actions_confirming(void);
void actions_tick(void);
/* Sets a light the same way [[light:...]] does; returns a short summary. */
const char *actions_light(const char *spec, bool log);
void actions_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int actions_recent(char out[][160], int max);
bool actions_undo(void);

/* ---- desktop.c (small hooks) ---- */

/* The companion says something about a hidden note (no person message). */
void companion_prompt(const char *note, int max_tokens);
/* Opens a folder in Files or a text file in Notes. */
void desktop_open_path(const char *path);

/* ---- other parts of the system, linked if present ----
 * Weak references: when nobody provides them they are NULL, and the
 * companion falls back to what it can do itself. */
void notify(const char *title, const char *body, const char *app) __attribute__((weak));
void theme_set_dark(bool dark) __attribute__((weak));
void settings_set_volume(int pct) __attribute__((weak));
int settings_get_volume(void) __attribute__((weak));
