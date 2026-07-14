CROSS   := aarch64-linux-musl-
CC      := $(CROSS)gcc
AS      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

KERNEL  := /tmp/FreeRTOS-Kernel
PORT    := $(KERNEL)/portable/GCC/ARM_AARCH64
HEAP    := $(KERNEL)/portable/MemMang

CFLAGS  := -mcpu=cortex-a53 -nostdlib -nostartfiles -ffreestanding \
           -O2 -Wall -Wextra -Wno-unused-parameter \
           -DGUEST -DQEMU \
           -I src \
           -I $(KERNEL)/include \
           -I $(PORT)

AFLAGS  := $(CFLAGS) -D__ASSEMBLY__

# FreeRTOS kernel sources
K_SRCS  := $(KERNEL)/tasks.c \
           $(KERNEL)/queue.c \
           $(KERNEL)/list.c \
           $(KERNEL)/timers.c \
           $(KERNEL)/event_groups.c \
           $(HEAP)/heap_4.c \
           $(PORT)/port.c

K_ASM   := $(PORT)/portASM.S

# BSP sources
B_SRCS  := src/main.c src/uart.c src/gic.c src/timer.c src/string.c
B_ASM   := src/startup.S src/vectors.S

OBJS    := $(patsubst %.c,build/%.o,$(notdir $(K_SRCS) $(B_SRCS))) \
           $(patsubst %.S,build/%.o,$(notdir $(K_ASM) $(B_ASM)))

TARGET  := freertos

all: $(TARGET).bin

build:
	mkdir -p build

# C compilation rules
build/tasks.o: $(KERNEL)/tasks.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/queue.o: $(KERNEL)/queue.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/list.o: $(KERNEL)/list.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/timers.o: $(KERNEL)/timers.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/event_groups.o: $(KERNEL)/event_groups.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/heap_4.o: $(HEAP)/heap_4.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/port.o: $(PORT)/port.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/portASM.o: $(PORT)/portASM.S | build
	$(AS) $(AFLAGS) -c $< -o $@

build/main.o: src/main.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/uart.o: src/uart.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/gic.o: src/gic.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/timer.o: src/timer.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/string.o: src/string.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/startup.o: src/startup.S | build
	$(AS) $(AFLAGS) -c $< -o $@
build/vectors.o: src/vectors.S | build
	$(AS) $(AFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(LD) -T freertos.lds -o $@ $(OBJS)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "=== Built: $@ ($$(stat -c%s $@) bytes) ==="

clean:
	rm -rf build $(TARGET).elf $(TARGET).bin

.PHONY: all clean
