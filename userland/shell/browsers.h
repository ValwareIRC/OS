#pragma once

/* Onboarding's browser question (browsers.c): asked once, while the AI makes
 * their space, when Flathub can be reached. It takes the pointer and keys
 * while it's up: take_input at the start of the frame, draw at its end. */
void browsers_take_input(void);
void browsers_draw(void);
