# Freestanding x86-64 kernel. The host gcc is fine for now because we pass
# -ffreestanding and our own linker script; a dedicated cross toolchain arrives
# with the musl userland (milestone 2).

BUILD ?= build
OUT   ?= $(BUILD)/kernel.elf
CC    ?= gcc
LD    ?= ld

CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
          -fno-pic -fno-pie -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse \
          -mno-sse2 -mno-red-zone -mcmodel=kernel -fno-omit-frame-pointer \
          -Iinclude -Ithird_party -MMD -MP
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T linker.ld

SRCS := $(shell find . -name '*.c' -not -path './third_party/*') \
        $(shell find . -name '*.S')
OBJS := $(patsubst ./%,$(BUILD)/%.o,$(SRCS))

$(OUT): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)
