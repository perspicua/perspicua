.PHONY: clean run-qemu debug-qemu format test

ASM_SOURCES := $(shell find src -name '*.S')
C_SOURCES := $(shell find src -name '*.c')

OBJECTS := $(patsubst src/%.S,build/%.o,$(ASM_SOURCES))
OBJECTS += $(patsubst src/%.c,build/%.o,$(C_SOURCES))

ifeq ($(shell uname),Darwin)
    CROSS_COMPILE ?= aarch64-elf-
    LLVM_PATH := $(shell brew --prefix llvm)
    OBJCOPY ?= $(LLVM_PATH)/bin/llvm-objcopy
else
    CROSS_COMPILE ?= aarch64-linux-gnu-
    OBJCOPY ?= llvm-objcopy
endif

run-qemu: build/kernel8.img
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel build/kernel8.img

run-qemu-vc: build/kernel8.img
	qemu-system-aarch64 -M raspi4b -serial vc -kernel build/kernel8.img

debug-qemu: build/kernel8.img
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel build/kernel8.img -s -S

build/kernel8.img: $(OBJECTS) link.ld
	$(CROSS_COMPILE)ld -nostdlib $(OBJECTS) -T link.ld -o build/kernel8.elf
	$(OBJCOPY) -O binary build/kernel8.elf build/kernel8.img

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CROSS_COMPILE)gcc -Wall -O2 -ffreestanding -nostdlib -mcpu=cortex-a72+nosimd -mgeneral-regs-only -c $< -o $@

build/%.o: src/%.S
	@mkdir -p $(dir $@)
	@echo "*" > ./build/.gitignore
	$(CROSS_COMPILE)as $< -o $@

format:
	@find src -name '*.c' -exec clang-format -i {} +
	@find src -name '*.h' -exec clang-format -i {} +

clean:
	rm -rf build
