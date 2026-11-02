#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 16550 UART on COM1: the kernel's first and most reliable log sink. */
bool serial_init(void);
void serial_remap(void); /* once vmm_init has loaded the kernel page tables */
void serial_write(const char *s, uint64_t len);
void serial_rx_init(void);

/* Text console drawn on the bootloader framebuffer: only with "verbose" on the kernel command line; otherwise
 * the boot screen (bootscreen.c) is shown and the messages are kept (/proc/bootlog). */
void fbcon_init(bool verbose);
void fbcon_write(const char *s, uint64_t len);
void fbcon_suspend(bool on); /* stop drawing once a program owns the screen */

/* The boot screen: a splash with the steps of the start behind Esc (drivers/bootscreen.c). */
void bootscreen_init(void);
bool bootscreen_active(void);
void bootscreen_suspend(void);
void bootscreen_start_animation(void); /* once threads can run */
void boot_step_start(const char *id);
void boot_step_done(const char *id, const char *detail);
void boot_step_progress(const char *id, uint64_t done, uint64_t total);
void boot_step_failed(const char *id, const char *detail);
void bootscreen_key_esc(void);
int bootscreen_steps_text(char *buf, unsigned long size);
int bootscreen_steps_write(const char *buf, unsigned long len);
void bootscreen_failed(const char *log, unsigned long len, const char *why);
/* everything the kernel has logged since it started (the oldest dropped past 512 KiB) */
const char *klog_text(unsigned long *len);
void fbdev_init(void);
void input_init(void);
void virtio_input_init(void);
