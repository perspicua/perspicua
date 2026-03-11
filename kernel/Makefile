include ../common.mk

BUILD_DIR ?= build
LIBC_DIR ?= ../libc

# Flags
CFLAGS = $(COMMON_CFLAGS) -Iinclude -Itests -I$(LIBC_DIR)/include -I.. -D__KERNEL__
ASFLAGS = $(ARCH_FLAGS) -Iinclude -Itests -I$(LIBC_DIR)/include -I.. -D__ASSEMBLY__ -MMD -MP
LDFLAGS = -nostdlib -T kernel.ld

# Subsystems
SRCS = $(wildcard mm/*.c fs/*.c sched/*.c core/*.c arch/*.c driver/*.c devicetree/*.c init/*.c tests/*.c)
ASMS = $(wildcard core/*.S arch/*.S)

OBJS = $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS)) $(patsubst %.S, $(BUILD_DIR)/%.o, $(ASMS))

# ensure arch/boot.o is linked first
BOOT_OBJ = $(BUILD_DIR)/arch/boot.o
OBJECTS = $(BOOT_OBJ) $(filter-out $(BOOT_OBJ), $(OBJS))

LIBK = $(LIBC_DIR)/$(BUILD_DIR)/libk.a

TARGET = $(BUILD_DIR)/kernel8.elf
IMAGE = $(BUILD_DIR)/kernel8.img

all: $(IMAGE)

$(IMAGE): $(TARGET)
	$(call print_img,$@)
	$(Q)$(OBJCOPY) -O binary $< $@

$(TARGET): $(OBJECTS) $(LIBK)
	$(call print_ld,$@)
	$(Q)$(LD) $(LDFLAGS) $(OBJECTS) $(LIBK) -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(call print_cc,$<)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(call print_as,$<)
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

clean:
	@printf "  $(COL_MAGENTA)CLEAN$(COL_DEFAULT)   kernel\n"
	$(Q)rm -rf $(BUILD_DIR)

-include $(OBJS:.o=.d)
