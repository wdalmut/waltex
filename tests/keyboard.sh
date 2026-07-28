#!/usr/bin/env bash
# Avvia il kernel, digita "walter" attraverso il monitor di QEMU, e verifica
# che i caratteri compaiano in eco sulla seriale.
#
# E' il solo test di M5 che deve girare dentro la VM: la decodifica degli
# scancode e il buffer circolare sono coperti dai test host, che sono
# istantanei. Qui si verifica la catena completa, dall'IRQ 1 alla stampa.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}
ATTESO="walter"

LOG=$(mktemp)
MON=$(mktemp -u)
trap 'rm -f "$LOG" "$MON"' EXIT

qemu-system-i386 -kernel "$KERNEL" -display none -no-reboot \
    -serial "file:$LOG" -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &
QPID=$!

# Non si digita prima che il kernel sia pronto a leggere.
for _ in $(seq 1 150); do
    grep -qF "waltex: eco attiva" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

if ! grep -qF "waltex: eco attiva" "$LOG"; then
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    echo "FAIL -- il kernel non ha raggiunto il ciclo di eco"
    echo "--- output seriale ---"; cat "$LOG"
    exit 1
fi

python3 tests/sendkeys.py "$MON" w a l t e r

for _ in $(seq 1 40); do
    grep -qF "$ATTESO" "$LOG" 2>/dev/null && break
    sleep 0.1
done

kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

if grep -qF "$ATTESO" "$LOG"; then
    echo "ok   -- i tasti digitati compaiono in eco"
    exit 0
fi

echo "FAIL -- eco di \"$ATTESO\" non trovata"
echo "--- output seriale ---"
cat "$LOG"
exit 1
