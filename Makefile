.PHONY: clean run-qemu debug-qemu

ASM_SOURCES := $(shell find src -name '*.S')
OBJECTS := $(patsubst src/%.S,build/%.o,$(ASM_SOURCES))

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

debug-qemu: build/kernel8.img
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel build/kernel8.img -s -S

build/kernel8.img: $(OBJECTS) link.ld
	$(CROSS_COMPILE)ld -nostdlib $(OBJECTS) -T link.ld -o build/kernel8.elf
	$(OBJCOPY) -O binary build/kernel8.elf build/kernel8.img

build/%.o: src/%.S
	@mkdir -p $(dir $@)
	@echo "*" > ./build/.gitignore
	$(CROSS_COMPILE)as $< -o $@

clean:
	rm -rf build
