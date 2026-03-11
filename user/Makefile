include ../common.mk

BUILD_DIR ?= build
LIBC_DIR ?= ../libc

# Flags
CFLAGS = $(COMMON_CFLAGS) -Iinclude -I$(LIBC_DIR)/include -I..
ASFLAGS = $(ARCH_FLAGS) -Iinclude -I$(LIBC_DIR)/include -I.. -D__ASSEMBLY__ -MMD -MP
LDFLAGS = -nostdlib -T user.ld

LIBC = $(LIBC_DIR)/$(BUILD_DIR)/libc.a
CRT0 = $(BUILD_DIR)/crt0.o

APP_SRCS = $(wildcard apps/*.c)
APP_OBJS = $(patsubst apps/%.c, $(BUILD_DIR)/apps/%.o, $(APP_SRCS))
APP_ELFS = $(patsubst $(BUILD_DIR)/apps/%.o, $(BUILD_DIR)/%.elf, $(APP_OBJS))

all: $(APP_ELFS)

$(BUILD_DIR)/%.elf: $(BUILD_DIR)/apps/%.o $(CRT0) $(LIBC)
	$(call print_ld,$@)
	$(Q)$(LD) $(LDFLAGS) $(CRT0) $< $(LIBC) -o $@

$(BUILD_DIR)/apps/%.o: apps/%.c
	@mkdir -p $(dir $@)
	$(call print_cc,$<)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(CRT0): crt0.S
	@mkdir -p $(dir $@)
	$(call print_as,$<)
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

clean:
	@printf "  $(COL_MAGENTA)CLEAN$(COL_DEFAULT)   user\n"
	$(Q)rm -rf $(BUILD_DIR)

-include $(APP_OBJS:.o=.d)
-include $(CRT0:.o=.d)
