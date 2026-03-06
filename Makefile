# toolchain
ifeq ($(shell uname),Darwin)
    CROSS_COMPILE ?= aarch64-elf-
    LLVM_PATH     := $(shell brew --prefix llvm 2>/dev/null)
    OBJCOPY       ?= $(LLVM_PATH)/bin/llvm-objcopy
else
    CROSS_COMPILE ?= aarch64-linux-gnu-
    OBJCOPY       ?= llvm-objcopy
endif

CC      = $(CROSS_COMPILE)gcc
LD      = $(CROSS_COMPILE)ld
OBJDUMP = $(CROSS_COMPILE)objdump
NM      = $(CROSS_COMPILE)nm
SIZE    = $(CROSS_COMPILE)size

# parallel build by default
MAKEFLAGS += -j$(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)

# directories
SRC_DIR   = src
BUILD_DIR = build
LINK_SCRIPT = link.ld

# flags
ARCH_FLAGS = -mcpu=cortex-a72+nosimd -mgeneral-regs-only

WARNINGS = -Wall -Wextra -Wshadow -Wdouble-promotion \
           -Wredundant-decls -Wconversion -Wno-sign-conversion \
           -Wundef -Wstrict-prototypes -Wmissing-prototypes

CFLAGS  = $(WARNINGS) -ffreestanding -nostdlib $(ARCH_FLAGS) \
          -I$(SRC_DIR) -std=gnu11 -MMD -MP \
          -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
ASFLAGS = $(ARCH_FLAGS) -I$(SRC_DIR) -D__ASSEMBLY__ -MMD -MP
LDFLAGS = -nostdlib -T $(LINK_SCRIPT)

# debug vs release
ifeq ($(DEBUG),1)
    CFLAGS  += -O0 -g3 -DDEBUG
    ASFLAGS += -g3
else
    CFLAGS  += -O2 -DNDEBUG
endif

# sources and objects (recursive wildcard, deterministic order)
rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))
C_SOURCES = $(sort $(call rwildcard,$(SRC_DIR),*.c))
S_SOURCES = $(sort $(call rwildcard,$(SRC_DIR),*.S))

C_OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
S_OBJECTS = $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(S_SOURCES))

# boot object linked first so .text.boot lands at entry
BOOT_OBJ  = $(BUILD_DIR)/arch/boot.o
OBJECTS   = $(BOOT_OBJ) $(filter-out $(BOOT_OBJ), $(S_OBJECTS) $(C_OBJECTS))
DEPS      = $(C_OBJECTS:.o=.d) $(S_OBJECTS:.o=.d)

TARGET = $(BUILD_DIR)/kernel8.elf
IMAGE  = $(BUILD_DIR)/kernel8.img

# quiet by default, V=1 for verbose
ifneq ($(V),1)
    Q = @
    msg = @printf '  %-7s %s\n' $(1) $(2)
else
    Q =
    msg =
endif

# cflags change detection — forces rebuild on flag changes
$(shell mkdir -p $(BUILD_DIR))
CFLAGS_FILE = $(BUILD_DIR)/.cflags
$(shell echo "$(CFLAGS)" | cmp -s - $(CFLAGS_FILE) || echo "$(CFLAGS)" > $(CFLAGS_FILE))

# build targets
.DELETE_ON_ERROR:

all: $(IMAGE)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(CFLAGS_FILE)
	$(call msg,"CC",$<)
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

# use CC for .S to get preprocessor and dependency tracking
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	$(call msg,"AS",$<)
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS) $(LINK_SCRIPT)
	$(call msg,"LD",$@)
	$(Q)$(LD) $(LDFLAGS) $(OBJECTS) -o $@

$(IMAGE): $(TARGET)
	$(call msg,"BIN",$@)
	$(Q)$(OBJCOPY) -O binary $< $@
	@printf '  %-7s %s bytes\n' "SIZE" "$$(wc -c < $@ | tr -d ' ')"

# run and debug
QEMU       = qemu-system-aarch64
QEMU_FLAGS     = -M raspi4b -serial stdio -display none -kernel $(IMAGE)
QEMU_FLAGS_GUI = -M raspi4b -serial vc -kernel $(IMAGE)

run: $(IMAGE)
	$(QEMU) $(QEMU_FLAGS)

run-gui: $(IMAGE)
	$(QEMU) $(QEMU_FLAGS_GUI)

# start QEMU paused, waiting for GDB on :1234
debug: $(IMAGE)
	@echo "Waiting for GDB on :1234 … (use 'make gdb' in another terminal)"
	$(QEMU) $(QEMU_FLAGS) -s -S

gdb: $(TARGET)
	$(CROSS_COMPILE)gdb -ex 'target remote :1234' -ex 'layout split' $<

# inspection
disasm: $(TARGET)
	$(OBJDUMP) -d -S $<

size: $(TARGET)
	$(SIZE) $<

symbols: $(TARGET)
	$(NM) -n $< | grep -v '\(compiled\)\|\.o$$'

# generate compile_commands.json for clangd (needs bear or compiledb)
compile_commands.json:
	@if command -v bear >/dev/null 2>&1; then \
		bear --append -- $(MAKE) all; \
	elif command -v compiledb >/dev/null 2>&1; then \
		compiledb $(MAKE) all; \
	else \
		echo "Install 'bear' or 'compiledb' to generate compile_commands.json"; \
		exit 1; \
	fi

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)

.PHONY: all clean run run-gui debug gdb disasm size symbols compile_commands.json
