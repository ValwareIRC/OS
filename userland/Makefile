# The OS user space, built as static musl binaries.
OUT    ?= ../build/userland
PORTS  ?= ../build/ports
CC     := musl-gcc
CFLAGS := -std=gnu17 -O2 -Wall -Wextra -Wno-format-truncation -I$(PORTS)/include -MMD -MP -I$(PORTS)/include/lua
LIBS   := -L$(PORTS)/lib -llua -lmbedtls -lmbedx509 -lmbedcrypto -lm

LIB_SRCS := $(wildcard lib/*.c)
LIB_OBJS := $(patsubst %.c,$(OUT)/obj/%.o,$(LIB_SRCS))

PROGRAMS := $(OUT)/init $(OUT)/os-ai $(OUT)/os-audio $(if $(wildcard shell/*.c),$(OUT)/os-shell)

all: $(PROGRAMS)

$(OUT)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/init: $(OUT)/obj/init/init.o
	$(CC) -static -o $@ $^

$(OUT)/os-audio: $(OUT)/obj/tools/os-audio.o
	$(CC) -static -o $@ $^

$(OUT)/os-ai: $(OUT)/obj/tools/os-ai.o $(LIB_OBJS)
	$(CC) -static -o $@ $^ $(LIBS)

SHELL_OBJS := $(patsubst %.c,$(OUT)/obj/%.o,$(wildcard shell/*.c shell/fm/*.c shell/wl/*.c))
$(OUT)/os-shell: $(SHELL_OBJS) $(LIB_OBJS)
	$(CC) -static -o $@ $^ $(LIBS)

-include $(shell find $(OUT)/obj -name '*.d' 2>/dev/null)
.PHONY: all
