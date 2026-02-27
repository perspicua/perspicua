.PHONY: clean run-qemu debug-qemu

ASM_SOURCES := $(shell find src -name '*.S')
OBJECTS := $(patsubst src/%.S,build/%.o,$(ASM_SOURCES))

run-qemu: build/kernel8.img
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel build/kernel8.img

debug-qemu: build/kernel8.img
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel build/kernel8.img -s -S

build/kernel8.img: $(OBJECTS) link.ld
	aarch64-linux-gnu-ld -nostdlib $(OBJECTS) -T link.ld -o build/kernel8.elf
	llvm-objcopy -O binary build/kernel8.elf build/kernel8.img

build/%.o: src/%.S
	@mkdir -p $(dir $@)
	@echo "*" > ./build/.gitignore
	aarch64-linux-gnu-as $< -o $@

clean:
	rm -rf build
