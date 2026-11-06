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
# NVIDIA's driver (x86-64 only for now): LKPI=1 builds in the Linux-shaped layer (linuxkpi), NVIDIA's own glue
# (every file of its module, from NV_SRC: docs/nvidia/plan.md) and its prebuilt core, nv-kernel.o.
LKPI ?= 0
OBJS_EXTRA :=
ifeq ($(ARCH)-$(LKPI),x86_64-1)
NV_SRC ?= $(HOME)/osbuild/nvidia-open
NV_GLUE := $(NV_SRC)/kernel-open
NV_CORE := $(NV_SRC)/src/nvidia/_out/Linux_x86_64/nv-kernel.o
NV_FILES := $(shell grep -o 'nvidia/[a-z_0-9-]*\.c' $(NV_GLUE)/nvidia/nvidia-sources.Kbuild)
SRCS += $(shell find ./linuxkpi/host ./linuxkpi/src -name '*.c')
OBJS_EXTRA := $(patsubst nvidia/%.c,$(BUILD)/nvidia/%.o,$(NV_FILES)) $(NV_CORE)
CFLAGS += -DLKPI
endif
OBJS := $(patsubst ./%,$(BUILD)/%.o,$(SRCS))

$(OUT): $(OBJS) $(OBJS_EXTRA) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) $(OBJS_EXTRA) -o $@

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

# NVIDIA's glue, as its Kbuild would build it, against the layer's headers (tests/nvidia/glue_compile.sh: the same)
NV_CFLAGS := -std=gnu11 -O2 -g -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-pic -fno-pie \
          -fno-omit-frame-pointer $(ARCH_CFLAGS) -nostdinc -isystem $(shell $(CC) -print-file-name=include) \
          -Ilinuxkpi/include -Ilinuxkpi -I$(NV_GLUE)/common/inc -I$(NV_GLUE)/nvidia -I$(NV_GLUE) \
          -include linuxkpi/include/linux/kconfig.h -include linuxkpi/include/linux/compiler_types.h \
          -D__KERNEL__ -DMODULE -DNVRM -DNV_KERNEL_INTERFACE_LAYER -DNV_UVM_ENABLE -DNDEBUG -DNV_SPECTRE_V2=0 \
          -DNV_FILESYSTEM_ACCESS_AVAILABLE=1 -DNVIDIA_UNDEF_LEGACY_BIT_MACROS -DNV_VERSION_STRING=\"620.70.06\" \
          -DKBUILD_MODNAME=\"nvidia\" -D__KBUILD_MODNAME=nvidia -DLIBSPDM_CONFIG=\"nvspdm_rmconfig.h\" \
          -Wno-unused-function -Wno-unused-variable -MMD -MP
$(BUILD)/nvidia/%.o: $(NV_GLUE)/nvidia/%.c
	@mkdir -p $(dir $@)
	$(CC) $(NV_CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

-include $(OBJS:.o=.d) $(filter $(BUILD)/nvidia/%,$(OBJS_EXTRA:.o=.d))
