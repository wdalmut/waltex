# -fno-builtin e' obbligatorio, non stilistico: senza, gcc espande inline le
# chiamate a memcpy/memset con dimensione costante usando le proprie versioni,
# e il test finisce per misurare il compilatore invece del codice del kernel.
# Il kernel compila gia' con questo flag: i test devono usare la stessa regola.
CFLAGS := -m32 -Wall -Wextra -std=gnu11 -g -fno-builtin -DWALTEX_HOSTED \
          -I../../include

BINS := test_kprintf test_memory test_timer test_ring test_keyboard

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

run: $(BINS)
	@for t in $(BINS); do echo "--- $$t ---"; ./$$t || exit 1; done

clean:
	rm -f $(BINS)

.PHONY: all run clean
