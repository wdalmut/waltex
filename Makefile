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

# L'estensione resta nel nome dell'oggetto: kernel/gdt.c e kernel/gdt.S
# esistono entrambi, e mappandoli su kernel/gdt.o si sovrascriverebbero a
# vicenda producendo un "multiple definition" al link.
OBJ  := $(patsubst %,$(BUILD)/%.o,$(CSRC) $(SSRC))

QEMU      := qemu-system-i386
DISK      := $(BUILD)/disk.img
MINIXIMG  := $(BUILD)/minix.img

# cache=writethrough non e' pedanteria: senza, QEMU tiene le scritture in un
# buffer e le versa sul file solo alla chiusura pulita. tests/disk.sh legge
# l'immagine da fuori dopo che il kernel ci ha scritto, e senza questo il
# settore resterebbe a zero in modo intermittente — la peggiore delle diagnosi.
# La difesa e' doppia: writethrough qui, e un "quit" dal monitor nel test.
# Due dischi da M11, e non e' una comodita': il settore 2 su cui disk.sh scrive
# e' la prima meta' del superblocco minix, quindi le due immagini NON possono
# stare sullo stesso disco. hda resta quella a pattern di M10, hdb e' il
# filesystem. ata_init prova gia' master e slave, quindi il kernel non cambia
# una riga — ma priv viene esercitato davvero per la prima volta: due struct
# blockdev con lo stesso puntatore a read, e solo priv a distinguerle.
QEMUFLAGS := -kernel $(KERNEL) -no-reboot \
             -drive file=$(DISK),format=raw,if=ide,index=0,cache=writethrough \
             -drive file=$(MINIXIMG),format=raw,if=ide,index=1,cache=writethrough

all: $(KERNEL) $(DISK) $(MINIXIMG)

# L'immagine si rifa' da zero ogni volta che lo script cambia. I test la
# ricostruiscono comunque per conto loro: veder passare due volte di fila un
# test che scrive nel proprio input non dimostra niente.
$(DISK): tools/mkdisk.sh
	./tools/mkdisk.sh $@

# L'immagine minix si COPIA in build/ invece di attaccare direttamente quella in
# tests/data/: quella e' il riferimento committato, e non deve poter essere
# modificata da una VM. M11a e' di sola lettura e non ci scriverebbe comunque,
# ma la difesa costa una riga e vale anche per M11b.
$(MINIXIMG): tests/data/minix.img
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

run: $(KERNEL) $(DISK) $(MINIXIMG)
	$(QEMU) $(QEMUFLAGS) -serial stdio

# -s apre il gdbserver sulla 1234, -S ferma la CPU alla prima istruzione.
# Da un altro terminale, in quest'ordine: i simboli prima della connessione,
# altrimenti gdb si lamenta di non sapere cosa sta debuggando.
#   gdb -q build/waltex.elf -ex 'target remote :1234' -ex 'break kmain' -ex continue
debug: $(KERNEL) $(DISK) $(MINIXIMG)
	$(QEMU) $(QEMUFLAGS) -serial stdio -s -S

host-test:
	$(MAKE) -C tests/host

test: $(KERNEL) $(DISK) $(MINIXIMG) host-test
	./tests/smoke.sh $(KERNEL)
	./tests/keyboard.sh $(KERNEL)
	./tests/shell.sh $(KERNEL)
	./tests/tasks.sh $(KERNEL)
	./tests/disk.sh $(KERNEL)

clean:
	rm -rf $(BUILD)
	$(MAKE) -C tests/host clean

.PHONY: all run debug test host-test clean
