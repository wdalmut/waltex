#!/usr/bin/env bash
# Avvia il kernel in QEMU headless, cattura COM1 su file e cerca i marker
# attesi. Appena l'ultimo marker compare, QEMU viene terminato: il test non
# aspetta il timeout se il kernel ha già detto tutto quello che doveva.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}
LAST_MARKER="waltex: M3 ok"
MARKERS=("waltex: booting" "waltex: multiboot ok" "waltex: gdt caricata" "waltex: idt e pic pronti" "$LAST_MARKER")

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

qemu-system-i386 -kernel "$KERNEL" -display none -no-reboot \
    -serial "file:$LOG" >/dev/null 2>&1 &
QPID=$!

# fino a 5 secondi, controllando ogni 100 ms
for _ in $(seq 1 50); do
    grep -qF "$LAST_MARKER" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

fail=0
for marker in "${MARKERS[@]}"; do
    if grep -qF "$marker" "$LOG"; then
        echo "ok   -- $marker"
    else
        echo "FAIL -- marker mancante: $marker"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "--- output seriale completo ---"
    cat "$LOG"
    echo "--- fine output ---"
fi

exit "$fail"
