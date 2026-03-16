include common.mk

BUILD_DIR = build
LIBC_DIR = libc
KERNEL_DIR = kernel
USER_DIR = user
PI4_BOOT_DIR = pi4-boot

INITRD = $(BUILD_DIR)/initrd.cpio

all: kernel

.PHONY: libc user kernel clean run run-gui run-sd debug gdb disasm size symbols compile_commands format check-format

libc:
	@printf "  $(COL_YELLOW)MAKE$(COL_DEFAULT)    libc\n"
	$(Q)$(MAKE) $(S) -C $(LIBC_DIR) BUILD_DIR=build V=$(V) COLOR=$(COLOR)

user: libc
	@printf "  $(COL_YELLOW)MAKE$(COL_DEFAULT)    user\n"
	$(Q)$(MAKE) $(S) -C $(USER_DIR) BUILD_DIR=build V=$(V) COLOR=$(COLOR)

$(INITRD): user
	$(call print_img,$@)
	@mkdir -p $(BUILD_DIR)/root
	@cp $(USER_DIR)/build/*.elf $(BUILD_DIR)/root/
	$(Q)cd $(BUILD_DIR)/root && find . -maxdepth 1 -not -path "." | sed 's|^\./||' | cpio -o -H newc > ../initrd.cpio 2>/dev/null

kernel: libc $(INITRD)
	@printf "  $(COL_YELLOW)MAKE$(COL_DEFAULT)    kernel\n"
	$(Q)$(MAKE) $(S) -C $(KERNEL_DIR) BUILD_DIR=build V=$(V) COLOR=$(COLOR)
	$(Q)cp $(KERNEL_DIR)/build/kernel8.img $(PI4_BOOT_DIR)/kernel8.img
	@printf "$(COL_GREEN)Build complete: $(PI4_BOOT_DIR)/kernel8.img$(COL_DEFAULT)\n"

clean:
	@printf "  $(COL_MAGENTA)CLEAN$(COL_DEFAULT)   all\n"
	$(Q)$(MAKE) $(S) -C $(LIBC_DIR) BUILD_DIR=build clean COLOR=$(COLOR)
	$(Q)$(MAKE) $(S) -C $(USER_DIR) BUILD_DIR=build clean COLOR=$(COLOR)
	$(Q)$(MAKE) $(S) -C $(KERNEL_DIR) BUILD_DIR=build clean COLOR=$(COLOR)
	$(Q)rm -rf $(BUILD_DIR) $(SD_IMAGE)

# QEMU helper targets
QEMU = qemu-system-aarch64
IMAGE = $(PI4_BOOT_DIR)/kernel8.img
SD_IMAGE = sdcard.img
QEMU_FLAGS = -M raspi4b -serial stdio -display none -dtb pi4-boot/bcm2711-rpi-4-b.dtb -kernel $(IMAGE)
QEMU_FLAGS_GUI = -M raspi4b -serial vc -dtb pi4-boot/bcm2711-rpi-4-b.dtb -kernel $(IMAGE)
QEMU_SD_FLAGS = -drive file=$(SD_IMAGE),format=raw,if=sd

$(SD_IMAGE): $(INITRD)
	@printf "  $(COL_CYAN)GEN$(COL_DEFAULT)      $(SD_IMAGE)\n"
	$(Q)dd if=/dev/zero of=$(SD_IMAGE) bs=1M count=32 status=none
	$(Q)dd if=$(INITRD) of=$(SD_IMAGE) conv=notrunc status=none

run: kernel $(SD_IMAGE)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_SD_FLAGS)

run-gui: kernel $(SD_IMAGE)
	$(QEMU) $(QEMU_FLAGS_GUI) $(QEMU_SD_FLAGS)

debug: kernel $(SD_IMAGE)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_SD_FLAGS) -s -S

gdb:
	$(CROSS_COMPILE)gdb -ex 'target remote :1234' -ex 'layout split' $(KERNEL_DIR)/build/kernel8.elf

disasm:
	$(OBJDUMP) -d -S $(KERNEL_DIR)/build/kernel8.elf

size:
	$(SIZE) $(KERNEL_DIR)/build/kernel8.elf

symbols:
	$(NM) -n $(KERNEL_DIR)/build/kernel8.elf | grep -v '\(compiled\)\|\.o$$'

compile_commands: clean
	@printf "  $(COL_CYAN)BEAR$(COL_DEFAULT)     compile_commands.json\n"
	$(Q)bear -- $(MAKE) V=1 all

FORMAT_FILES = $(shell find kernel libc uapi user -name "*.c" -o -name "*.h")

format:
	@printf "  $(COL_CYAN)FORMAT$(COL_DEFAULT)   all C and H files\n"
	$(Q)clang-format -i $(FORMAT_FILES)

check-format:
	@printf "  $(COL_CYAN)CHECK$(COL_DEFAULT)    format style\n"
	$(Q)clang-format --dry-run --Werror $(FORMAT_FILES)

