#pragma once
#include <kernel/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Text sent by the host (clipboard sharing in VMs), read without blocking. */
bool platform_hostlink_init(void);
int64_t platform_hostlink_read(void *buf, uint64_t len);

void platform_input_init(void);
int64_t platform_rtc_read(void);
void platform_reboot(void);
void platform_poweroff(void);
void platform_test_exit(uint8_t code);
uint32_t platform_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void platform_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
/* Legacy PCI I/O BAR access, `size` bytes (1, 2 or 4) at `port`. */
uint32_t platform_pci_io_read(uint16_t port, unsigned size);
void platform_pci_io_write(uint16_t port, unsigned size, uint32_t v);
/* Would w x h at 32 bits per pixel work on the display? */
bool platform_fb_mode_fits(uint32_t w, uint32_t h);
/* Switches to w x h, 32 bits per pixel, pitch w * 4; returns the framebuffer address and memory size. */
bool platform_fb_set_mode(uint32_t w, uint32_t h, uint64_t *phys, uint64_t *vram_bytes);

/* QEMU fw_cfg. Reading a file returns its byte count, finding one its size and key; both are negative when it does not exist. */
int fwcfg_read_file(const char *name, char *buf, size_t size);
int fwcfg_find_file(const char *name, uint16_t *key);
/* The registers: select an item, then read its bytes in order, all under fwcfg_lock. */
extern spinlock_t fwcfg_lock;
void fwcfg_select(uint16_t key);
void fwcfg_read(void *buf, size_t len);
