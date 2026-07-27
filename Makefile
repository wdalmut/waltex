CC  := gcc
LD  := ld

CFLAGS  := -m32 -ffreestanding -nostdlib -fno-pie -fno-stack-protector \
           -fno-builtin -Wall -Wextra -std=gnu11 -g -Iinclude
ASFLAGS := -m32 -g -Iinclude
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

BUILD  := build
KERNEL := $(BUILD)/waltex.elf

CSRC := $(wildcard kernel/*.c)
SSRC := boot/multiboot.S $(wildcard kernel/*.S)
OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(CSRC)) $(patsubst %.S,$(BUILD)/%.o,$(SSRC))

QEMU      := qemu-system-i386
QEMUFLAGS := -kernel $(KERNEL) -no-reboot

all: $(KERNEL)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

run: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -serial stdio

# -s apre il gdbserver sulla 1234, -S ferma la CPU alla prima istruzione:
#   gdb -ex 'target remote :1234' -ex 'symbol-file build/waltex.elf'
debug: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -serial stdio -s -S

host-test:
	$(MAKE) -C tests/host

test: $(KERNEL) host-test
	./tests/smoke.sh $(KERNEL)

clean:
	rm -rf $(BUILD)
	$(MAKE) -C tests/host clean

.PHONY: all run debug test host-test clean
