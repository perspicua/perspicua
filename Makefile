CROSS_COMPILE ?= aarch64-linux-gnu-

CC = $(CROSS_COMPILE)gcc
AS = $(CROSS_COMPILE)as
LD = $(CROSS_COMPILE)ld
OBJCOPY = llvm-objcopy

CFLAGS = -Wall -O2 -ffreestanding -nostdlib -mcpu=cortex-a72+nosimd -mgeneral-regs-only -MMD -MP
ASFLAGS =
LDFLAGS = -nostdlib -T link.ld

SRC_DIR = src
BUILD_DIR = build

C_SOURCES = $(shell find $(SRC_DIR) -name '*.c')
S_SOURCES = $(shell find $(SRC_DIR) -name '*.S')

C_OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
S_OBJECTS = $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(S_SOURCES))

OBJECTS = $(S_OBJECTS) $(C_OBJECTS)
DEPS = $(C_OBJECTS:.o=.d)

TARGET = $(BUILD_DIR)/kernel8.elf
IMAGE = $(BUILD_DIR)/kernel8.img

all: $(IMAGE)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(TARGET): $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@

$(IMAGE): $(TARGET)
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD_DIR)

run: $(IMAGE)
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel $(IMAGE)

-include $(DEPS)

.PHONY: all clean run
