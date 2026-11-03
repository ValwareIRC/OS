#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Date and time (clock.c): their time zone, set by hand or found from where the computer is on the internet;
 * the clock set from the network (by init, time.inc); 12- or 24-hour. Kept in /etc/os/time.conf. */

/* at the start: their choices read, TZ set */
void clock_init(void);
/* once a frame: the automatic time zone's answer, when it comes */
void clock_frame(void);

const char *clock_zone(void); /* "Europe/London" */
bool clock_set_zone(const char *zone); /* by hand (turns the automatic zone off) */
bool clock_auto_zone(void);
void clock_set_auto_zone(bool on); /* on: looked up now */
bool clock_auto_time(void);
void clock_set_auto_time(bool on);
bool clock_12h(void);
void clock_set_12h(bool on);
/* the network's time now (init sets the clock); the answer through clock_sync_result */
void clock_sync_now(void);
/* 0 while waiting or never asked; 1 done, -1 failed; what happened in msg */
int clock_sync_result(char *msg, size_t n);
/* what the automatic time zone last found ("" none yet), and when (0 never) */
const char *clock_auto_status(void);

/* The zones of the world, for picking: the tz database's zone1970.tab, with country names. */
struct clock_zone_entry {
    char name[48];     /* Europe/London */
    char where[64];    /* United Kingdom */
};
int clock_zones(const struct clock_zone_entry **out);
