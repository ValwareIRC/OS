#pragma once
#include <stdbool.h>
#include <time.h>

/* Their conversations with the companion, as in every AI app (chats.c): the one open, and the ones before it.
 * "+ New chat" keeps the one open and starts another; the list opens any of them again or deletes it. What the
 * companion remembers about them is the same in all of them. */

struct chat_entry {
    char id[40];     /* "" for the one open, not kept yet */
    char title[96];  /* its first words (theirs) */
    time_t when;     /* its last words */
    bool open;       /* the one open now */
};

/* Newest first, the open one among them; at most `max`. */
int chats_list(struct chat_entry *out, int max);
/* Not while the companion is answering or at work: its answer belongs to the chat it was asked in. */
bool chats_can_switch(void);
/* Keeps the one open (if anything was said in it) and starts an empty one. */
void chats_new(void);
/* Keeps the one open and opens that one again. */
bool chats_open(const char *id);
/* That one, for good ("" or the open one's id: the one open, and an empty one starts). */
void chats_delete(const char *id);
void chats_delete_all(void);
/* The one open, written where the list reads it too (after anything new in it). */
void chats_saved(void);
/* "Today 21:05", "Yesterday", "Mon", "5 Nov" */
void chats_when(time_t t, char *out, int n);
