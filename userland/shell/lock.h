#pragma once
/* The lock screen (lock.c): their clock, their name, their password (if they set one) to open it. It takes the
 * whole screen and every key and click while it is up. Locked by Super+L, by Settings, after the time away they
 * chose (Settings > Users), and at the start when they have a password (the sign-in). The password itself is
 * init's to check (session.inc): the desktop never holds it. */
#include <stdbool.h>

void lock_boot(void);        /* at the start: signed in first if they have a password */
void lock_now(void);         /* Super+L, Settings, the companion */
bool lock_active(void);
bool lock_frame(void);       /* while locked: draws it and takes the input (true); else false */
bool lock_has_password(void);
/* their password changed in Settings: "" old when there is none, "" new to take it away. The answer comes
 * later: lock_password_result. */
void lock_password_change(const char *old_pw, const char *new_pw);
/* 0 while waiting (or nothing asked), 1 done, -1 refused (*why: "wrong", "wait 15") */
int lock_password_result(char *why, int n);
