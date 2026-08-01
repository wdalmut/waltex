#!/usr/bin/env bash
# Avvia il kernel, digita "echo ciao" nel monitor di QEMU e verifica che la
# shell abbia eseguito il comando.
#
# E' il solo test di M7 che deve girare dentro la VM: l'editor di riga, lo
# splitting e il parsing esadecimale sono coperti dai test host, che sono
# istantanei. Qui si verifica la catena completa — IRQ 1, ring buffer, editor di
# riga, tabella dei comandi, kprintf — che non esiste da nessun'altra parte.
#
# I nomi dei tasti sono verificati contro il monitor di QEMU, non ricordati:
# "ret" per Invio e "spc" per lo spazio sono accettati, "enter" e "space"
# vengono rifiutati in silenzio.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}

# Da M10 il disco va attaccato anche qui, non solo in tests/disk.sh: i
# self-check dell ATA girano a ogni boot, e senza immagine kmain si ferma su
# "N selftest falliti" prima di stampare qualunque marker.
DISK=${2:-build/disk.img}

# Il marker di fine boot, non il prompt: il prompt non ha un ritorno a capo in
# fondo, quindi cercarlo con grep su un file che sta crescendo e' una corsa.
PRONTO="waltex: M7 ok"

# Definito in include/shell.h, non una stringa inventata qui: se il prompt
# cambiasse in un solo posto questo test mentirebbe.
PROMPT="waltex> "

LOG=$(mktemp)
MON=$(mktemp -u)
trap 'rm -f "$LOG" "$MON"' EXIT

qemu-system-i386 -kernel "$KERNEL" -display none -no-reboot \
    -drive file="$DISK",format=raw,if=ide,cache=writethrough \
    -serial "file:$LOG" -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &
QPID=$!

# Non si digita prima che la shell sia pronta a leggere.
for _ in $(seq 1 150); do
    grep -qF "$PRONTO" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

if ! grep -qF "$PRONTO" "$LOG"; then
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    echo "FAIL -- il kernel non ha raggiunto il prompt"
    echo "--- output seriale ---"; cat "$LOG"
    exit 1
fi

python3 tests/sendkeys.py "$MON" e c h o spc c i a o ret

# M8: "devs" elenca il registro dei dispositivi. E' un test di M8 travestito da
# test della shell — prende sia l'enumerazione sia il fatto che i tre driver si
# siano iscritti davvero, cosa che nessun test host puo' verificare perche' li
# iscrivono le *_init dentro la VM.
python3 tests/sendkeys.py "$MON" d e v s ret

# M9b: l'albero. "ls /" deve mostrare la sola voce della radice, "ls /dev" i tre
# dispositivi che i driver hanno iscritto.
python3 tests/sendkeys.py "$MON" l s spc slash ret
python3 tests/sendkeys.py "$MON" l s spc slash d e v ret

# E il controllo che chiude il secondo blocco: cat su un dispositivo.
#
# Si digita il comando, e POI la riga che cat deve leggere. Mentre cat gira, il
# ciclo di shell_task non sta leggendo la tastiera — quindi quei caratteri non
# passano dall'editor di riga e non vengono echeggiati da lui: quello che
# compare lo stampa cat, dopo averlo letto attraverso cinque livelli.
python3 tests/sendkeys.py "$MON" c a t spc slash d e v slash k b d ret
sleep 0.3
python3 tests/sendkeys.py "$MON" p i p p o ret

# La riga di output deve essere esattamente "ciao". Cercare "ciao" e basta
# troverebbe anche l'eco di quello che abbiamo digitato, che sta sulla riga del
# prompt: quella comincia con "waltex> ", questa no.
pulito() { tr -d '\r' < "$LOG"; }

for _ in $(seq 1 40); do
    pulito | grep -qx "ciao" && break
    sleep 0.1
done

kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

FALLITI=0

if pulito | grep -qx "ciao"; then
    echo "ok   -- la shell ha eseguito \"echo ciao\""
else
    echo "FAIL -- \"echo ciao\" non ha prodotto una riga \"ciao\""
    FALLITI=1
fi

# Il prompt deve ricomparire dopo il comando: una volta all'avvio e una dopo.
# E' la prova che il ciclo continua invece di fermarsi al primo comando, ed e'
# indipendente da come sono formulati i messaggi della shell.
PROMPTS=$(pulito | grep -oF "$PROMPT" | wc -l)

if [ "$PROMPTS" -ge 2 ]; then
    echo "ok   -- il prompt ricompare dopo il comando ($PROMPTS volte)"
else
    echo "FAIL -- il prompt e' comparso $PROMPTS volte, attese almeno 2"
    FALLITI=1
fi

# Si cerca SOLO nella parte di log che segue il comando devs, e la restrizione
# non e' prudenza: i self-check stampano righe come
#   selftest: ok   -- console ha i numeri 5:1
# che contengono gli stessi nomi e gli stessi numeri. Cercando in tutto il log,
# questi tre controlli passavano con devs inesistente — verificato.
dopo_devs() { pulito | sed -n '/waltex> devs/,$p'; }

# Si cerca il nome insieme ai suoi numeri, non l'intera riga formattata:
# l'incolonnamento e' una scelta di chi scrive il comando, e un test che lo
# inchiodasse si romperebbe a ogni ritocco estetico senza dire niente di utile.
for atteso in "console.*5:1" "ttyS0.*4:64" "kbd.*13:64"; do
    nome=${atteso%%.*}

    if dopo_devs | grep -qE "$atteso"; then
        echo "ok   -- devs elenca $nome con i suoi numeri"
    else
        echo "FAIL -- devs non ha elencato $nome (cercato: $atteso)"
        FALLITI=1
    fi
done

# Le capacita', che sono il guadagno visibile della convenzione "puntatore nullo
# uguale operazione non supportata": console scrive e non legge, kbd il
# contrario. Nessuno dei due nomi contiene una 'r' o una 'w', quindi cercarle
# nella riga e' un'asserzione vera e non un artefatto.
RIGA_CONSOLE=$(dopo_devs | grep -E "console.*5:1" | head -1)
RIGA_KBD=$(dopo_devs | grep -E "kbd.*13:64" | head -1)

if [ -n "$RIGA_CONSOLE" ] && [ -n "$RIGA_KBD" ] &&
   printf %s "$RIGA_CONSOLE" | grep -q "w" &&
   ! printf %s "$RIGA_CONSOLE" | grep -q "r" &&
   printf %s "$RIGA_KBD" | grep -q "r" &&
   ! printf %s "$RIGA_KBD" | grep -q "w"; then
    echo "ok   -- devs distingue chi legge da chi scrive"
else
    echo "FAIL -- devs non mostra le capacita' corrette"
    echo "        console: '$RIGA_CONSOLE'  (atteso: scrive, non legge)"
    echo "        kbd:     '$RIGA_KBD'  (atteso: legge, non scrive)"
    FALLITI=1
fi

# M9b. Ogni controllo guarda SOLO le righe fra il proprio comando e il prompt
# successivo, e la restrizione e' la lezione di M8: cercare in tutto il log
# troverebbe l'eco del comando digitato — "waltex> ls /dev" contiene "dev" — e i
# controlli passerebbero con i comandi inesistenti.
fra_prompt() {
    pulito | awk -v m="waltex> $1" '
        index($0, m) { f = 1; next }
        f && index($0, "waltex>") { exit }
        f { print }'
}

if fra_prompt "ls /" | grep -qE "(^| )dev$"; then
    echo "ok   -- ls / elenca la voce dev"
else
    echo "FAIL -- ls / non ha elencato dev"
    FALLITI=1
fi

for nome in console ttyS0 kbd; do
    if fra_prompt "ls /dev" | grep -qE "(^| )$nome$"; then
        echo "ok   -- ls /dev elenca $nome"
    else
        echo "FAIL -- ls /dev non ha elencato $nome"
        FALLITI=1
    fi
done

# La catena intera: IRQ 1 → ring buffer → keyboard_getchar → kbd_dev_read →
# chardev_read → vfs_read → cat. Sette livelli, e "pippo" esce dall'altra parte.
if fra_prompt "cat /dev/kbd" | grep -q "pippo"; then
    echo "ok   -- cat /dev/kbd legge la tastiera attraverso il VFS"
else
    echo "FAIL -- cat /dev/kbd non ha restituito la riga digitata"
    FALLITI=1
fi

if [ "$FALLITI" -ne 0 ]; then
    echo "--- output seriale ---"
    pulito | tail -30
    exit 1
fi

exit 0
