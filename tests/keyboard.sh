#!/usr/bin/env bash
# Avvia il kernel, digita "walter" attraverso il monitor di QEMU, e verifica
# che i caratteri compaiano in eco sulla seriale.
#
# E' il solo test di M5 che deve girare dentro la VM: la decodifica degli
# scancode e il buffer circolare sono coperti dai test host, che sono
# istantanei. Qui si verifica la catena completa, dall'IRQ 1 alla stampa.
#
# Da M7 l'eco non la fa piu' il ciclo di idle di kmain ma l'editor di riga della
# shell — il consumatore del ring buffer si e' SPOSTATO, e questo test continua
# a valere perche' verifica la catena, non chi sta in fondo. Non premiamo Invio:
# ci interessa l'eco dei caratteri, non l'esecuzione di un comando.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}

# Da M10 il disco va attaccato anche qui, non solo in tests/disk.sh: i
# self-check dell ATA girano a ogni boot, e senza immagine kmain si ferma su
# "N selftest falliti" prima di stampare qualunque marker.
DISK=${2:-build/disk.img}
ATTESO="walter"
PRONTO="waltex: M7 ok"

LOG=$(mktemp)
MON=$(mktemp -u)
trap 'rm -f "$LOG" "$MON"' EXIT

qemu-system-i386 -kernel "$KERNEL" -display none -no-reboot \
    -drive file="$DISK",format=raw,if=ide,cache=writethrough \
    -serial "file:$LOG" -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &
QPID=$!

# Non si digita prima che il kernel sia pronto a leggere.
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

python3 tests/sendkeys.py "$MON" w a l t e r

# Fino a M6b i due task di prova stampavano A e B in continuazione e l'eco
# usciva interlacciata — "wABaABlAB..." — quindi qui c'era un tr -d 'AB' per
# togliere il rumore. Da M7 i task partono silenziosi e li accende il comando
# "spin", che questo test non manda: il filtro non serve piu'.
pulito() { tr -d '\r' < "$LOG"; }

for _ in $(seq 1 40); do
    pulito | grep -qF "$ATTESO" && break
    sleep 0.1
done

kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

if pulito | grep -qF "$ATTESO"; then
    echo "ok   -- i tasti digitati compaiono in eco"
    exit 0
fi

echo "FAIL -- eco di \"$ATTESO\" non trovata"
echo "--- output seriale ---"
pulito | tail -20
exit 1
