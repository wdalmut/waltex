# -fno-builtin e' obbligatorio, non stilistico: senza, gcc espande inline le
# chiamate a memcpy/memset con dimensione costante usando le proprie versioni,
# e il test finisce per misurare il compilatore invece del codice del kernel.
# Il kernel compila gia' con questo flag: i test devono usare la stessa regola.
CFLAGS := -m32 -Wall -Wextra -std=gnu11 -g -fno-builtin -DWALTEX_HOSTED \
          -I../../include

BINS := test_kprintf test_memory test_timer test_ring test_keyboard test_task \
        test_lineedit test_shell test_device

all: run

test_kprintf: test_kprintf.c ../../kernel/kprintf.c
	$(CC) $(CFLAGS) -o $@ $^

test_memory: test_memory.c ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^

test_timer: test_timer.c ../../kernel/timer.c
	$(CC) $(CFLAGS) -o $@ $^

test_ring: test_ring.c ../../kernel/ring.c
	$(CC) $(CFLAGS) -o $@ $^

test_keyboard: test_keyboard.c ../../kernel/keyboard.c ../../kernel/ring.c
	$(CC) $(CFLAGS) -o $@ $^

test_task: test_task.c ../../kernel/task.c
	$(CC) $(CFLAGS) -o $@ $^

test_lineedit: test_lineedit.c ../../kernel/lineedit.c
	$(CC) $(CFLAGS) -o $@ $^

# memory.c serve per strcmp, che la ricerca nella tabella dei comandi usa e che
# in freestanding non esiste. lineedit.c serve perche' shell_task ci si appoggia.
# device.c serve da M8, per il comando devs: si linka quello vero invece di
# stubbarlo, perche' e' logica pura e non tira dentro nient'altro.
test_shell: test_shell.c ../../kernel/shell.c ../../kernel/lineedit.c \
            ../../kernel/memory.c ../../kernel/device.c
	$(CC) $(CFLAGS) -o $@ $^

# memory.c serve per strcmp, che device_find usa per il confronto esatto dei
# nomi. Nessuno stub: il registro e' logica pura.
test_device: test_device.c ../../kernel/device.c ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^

run: $(BINS)
	@for t in $(BINS); do echo "--- $$t ---"; ./$$t || exit 1; done

clean:
	rm -f $(BINS)

.PHONY: all run clean
