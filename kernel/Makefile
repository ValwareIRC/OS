# Freestanding kernel; ARCH selects arch/$(ARCH). The host gcc is fine for now because we pass
# -ffreestanding and our own linker script; a dedicated cross toolchain arrives
# with the musl userland (milestone 2).

BUILD ?= build
OUT   ?= $(BUILD)/kernel.elf
CC    ?= gcc
LD    ?= ld
ARCH  ?= x86_64

include arch/$(ARCH)/arch.mk

CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
          -fno-pic -fno-pie -fno-omit-frame-pointer \
          $(ARCH_CFLAGS) -Iinclude -Iarch/$(ARCH)/include -Ithird_party -MMD -MP
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T linker.ld

SRCS := $(shell find . \( -name '*.c' -o -name '*.S' \) -not -path './arch/*' -not -path './platform/*' \
                -not -path './third_party/*' -not -path './linuxkpi/*') \
        $(shell find arch/$(ARCH) $(ARCH_DIRS) \( -name '*.c' -o -name '*.S' \) | sed 's|^|./|')
# The Linux-shaped layer for NVIDIA's driver (x86-64 only for now), while it is being written: LKPI=1 builds it in.
LKPI ?= 0
ifeq ($(ARCH)-$(LKPI),x86_64-1)
SRCS += $(shell find ./linuxkpi/host ./linuxkpi/src -name '*.c')
endif
OBJS := $(patsubst ./%,$(BUILD)/%.o,$(SRCS))

$(OUT): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The Linux-shaped layer for NVIDIA's driver (linuxkpi/src: docs/nvidia/plan.md) is written against the layer's own
# headers, not ours (each has its own spinlock_t, struct pci_dev...); it reaches our kernel only through
# linuxkpi/host.h, which linuxkpi/host/*.c implements against ours.
LKPI_CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter \
          -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-pic -fno-pie -fno-omit-frame-pointer \
          $(ARCH_CFLAGS) -nostdinc -isystem $(shell $(CC) -print-file-name=include) -Ilinuxkpi/include -Ilinuxkpi \
          -include linuxkpi/include/linux/kconfig.h -include linuxkpi/include/linux/compiler_types.h \
          -D__KERNEL__ -MMD -MP
$(BUILD)/linuxkpi/src/%.c.o: linuxkpi/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LKPI_CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)
