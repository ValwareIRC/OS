#pragma once
#include <stdbool.h>

/* The shell's side of the portals: os-portal (the D-Bus backend) asks it
 * to show the file chooser and notifications, and hears the colour scheme
 * (portal.c). */
void portal_init(void);
/* Descriptors to wait on in the main loop, and taking in what arrived. */
int portal_fds(int *out, int max);
void portal_poll(void);
/* A chooser is open: it takes the pointer and keyboard. Call take_input at
 * the start of a desktop frame and draw at its end. */
bool portal_modal(void);
void portal_take_input(void);
void portal_draw(void);
