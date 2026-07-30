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

if [ "$FALLITI" -ne 0 ]; then
    echo "--- output seriale ---"
    pulito | tail -20
    exit 1
fi

exit 0
