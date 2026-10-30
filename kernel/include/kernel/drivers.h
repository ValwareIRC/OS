#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 16550 UART on COM1: the kernel's first and most reliable log sink. */
bool serial_init(void);
void serial_remap(void); /* once vmm_init has loaded the kernel page tables */
void serial_write(const char *s, uint64_t len);
void serial_rx_init(void);

/* Text console drawn on the bootloader framebuffer. Only used for boot and
 * emergencies; the real desktop is drawn by the compositor. */
void fbcon_init(void);
void fbcon_write(const char *s, uint64_t len);
void fbcon_suspend(bool on); /* stop drawing once a program owns the screen */
void fbdev_init(void);
void input_init(void);
void virtio_input_init(void);
