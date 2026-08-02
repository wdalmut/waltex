# -fno-builtin e' obbligatorio, non stilistico: senza, gcc espande inline le
# chiamate a memcpy/memset con dimensione costante usando le proprie versioni,
# e il test finisce per misurare il compilatore invece del codice del kernel.
# Il kernel compila gia' con questo flag: i test devono usare la stessa regola.
CFLAGS := -m32 -Wall -Wextra -std=gnu11 -g -fno-builtin -DWALTEX_HOSTED \
          -I../../include

BINS := test_kprintf test_memory test_timer test_ring test_keyboard test_task \
        test_lineedit test_shell test_device test_vfs test_minixfs

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
# stubbarlo, perche' e' logica pura e non tira dentro nient'altro. vfs.c serve
# da M9b, per ls e cat — stesso criterio, e task_current() lo fornisce gia'
# test_shell.c per il comando ps.
#
# Nessun test host esercita ls o cat: senza una radice il VFS non ha niente da
# risolvere, e costruirne una qui vorrebbe dire riscrivere l'albero finto di
# test_vfs.c. Le due funzioni le prende tests/shell.sh dentro la VM, dove
# l'albero e' quello vero. Qui si linka soltanto — che e' comunque un controllo:
# e' il link freestanding a scoprire i buchi, e questo gli somiglia.
# ata.c si aggiunge da M10, per blk/rdsect/wrsect. Contiene istruzioni
# privilegiate — in e out — che in user space darebbero SIGSEGV, ed e' innocuo
# solo perche' nessun test host esegue quei tre comandi: senza un disco non
# avrebbero niente da leggere. Vale la stessa nota di vfs.c qui sopra, con in
# piu' che qui eseguirlo non e' inutile, e' letale.
test_shell: test_shell.c ../../kernel/shell.c ../../kernel/lineedit.c \
            ../../kernel/memory.c ../../kernel/device.c ../../kernel/vfs.c \
            ../../kernel/ata.c
	$(CC) $(CFLAGS) -o $@ $^

# memory.c serve per strcmp, che device_find usa per il confronto esatto dei
# nomi. Nessuno stub: il registro e' logica pura.
test_device: test_device.c ../../kernel/device.c ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^

# memory.c per ogni funzione di stringa che vfs.c dovesse usare; nessuno stub,
# perche' l'unica dipendenza esterna del VFS e' task_current(), che test_vfs.c
# fornisce da se' per poter verificare che i descrittori siano PER TASK.
test_vfs: test_vfs.c ../../kernel/vfs.c ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^

# memory.c per le funzioni di stringa. Nessuno stub: l'unica dipendenza esterna
# di minixfs e' struct blockdev, e test_minixfs.c ne fornisce uno backed da
# fread/fseek sull'immagine di riferimento. E' il motivo per cui il grosso di
# M11a si prova in millisecondi invece che dentro QEMU.
#
# Il percorso dell'immagine e' ASSOLUTO, costruito qui con $(CURDIR): un
# relativo funzionerebbe solo lanciando il binario da questa directory, e la
# prima cosa che si fa e' lanciarlo dalla radice del repo.
test_minixfs: test_minixfs.c ../../kernel/minixfs.c ../../kernel/memory.c
	$(CC) $(CFLAGS) -DMINIX_IMG='"$(CURDIR)/../data/minix.img"' \
	                -DMINIX_WORK='"$(CURDIR)/../data/minix-lavoro.img"' -o $@ $^

run: $(BINS)
	@for t in $(BINS); do echo "--- $$t ---"; ./$$t || exit 1; done

clean:
	rm -f $(BINS)
	rm -f ../data/minix-lavoro.img

.PHONY: all run clean
