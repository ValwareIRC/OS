#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "gfx.h"

/* The system tray (tray.c): Facet elements a design can place anywhere
 * (<volume/>, <network/>, <notifications/>, <quicksettings/>, and the
 * calendar behind every <clock>), their popovers, and the notifications
 * that show up as toasts. */

/* A notification from the companion, an app or the system. `app` is an app
 * id ("files", "companion", "network", ...) or NULL; clicking the toast opens
 * that app. Shown as a toast unless Do not disturb is on, and always kept in
 * the notification centre's history. Safe to call at any time. */
void notify(const char *title, const char *body, const char *app);

/* Notifications from other programs: a file dropped in this folder holding
 * "title\nbody\napp\n" becomes a notification (and is removed). */
#define NOTIFY_SPOOL "/run/os/notify"

bool notify_dnd(void);
void notify_set_dnd(bool on);
int notify_unread(void);

/* Opens or closes a tray popover: "volume", "network", "notifications",
 * "calendar", "quicksettings". `anchor` is the element that asked. */
void tray_action(const char *what, rect anchor);
void tray_close(void);

/* Called by the desktop each frame: before anything reads the pointer
 * (the tray's popovers and toasts take it first), and after the design is
 * drawn (they float over everything). */
void tray_frame_begin(void);
void tray_frame_end(void);

/* ---- sound (/dev/mixer), shared with Settings ---- */
bool sound_available(void);
float sound_volume(void); /* 0..1 */
bool sound_muted(void);
void sound_set_volume(float v, bool keep); /* keep: remember it across restarts */
void sound_set_muted(bool m);
void sound_restore(void);  /* at boot: the level they left it at */
void sound_test(void);     /* a short chime, in the background */

/* ---- the network (eth0), shared with Settings and About ---- */
struct net_state {
    bool present;  /* there is a network card */
    bool link;     /* a cable (carrier) */
    bool has_ip;
    char ip[20], mask[20], gw[20], dns[48], mac[24];
    unsigned long rx, tx; /* packets */
};
const struct net_state *net_state(void);
