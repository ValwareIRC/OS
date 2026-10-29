#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 16550 UART on COM1: the kernel's first and most reliable log sink. */
bool serial_init(void);
bool com2_init(void);
int64_t com2_read(void *buf, uint64_t len);
void serial_write(const char *s, uint64_t len);

/* Text console drawn on the bootloader framebuffer. Only used for boot and
 * emergencies; the real desktop is drawn by the compositor. */
void fbcon_init(void);
void fbcon_write(const char *s, uint64_t len);
void fbcon_suspend(bool on); /* stop drawing once a program owns the screen */
void fbdev_init(void);
void input_init(void);
