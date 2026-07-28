#!/usr/bin/env bash
# Verifica che due task si ALTERNINO davvero.
#
# Non basta che sulla seriale compaiano sia A sia B: due task che stampano
# venti A e poi venti B non si sono alternati, si sono succeduti. Quindi
# estraiamo la sequenza delle sole A e B e controlliamo che i caratteri
# adiacenti siano sempre diversi.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}
ALTERNANZE_MINIME=20

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

qemu-system-i386 -kernel "$KERNEL" -display none -no-reboot \
    -serial "file:$LOG" >/dev/null 2>&1 &
QPID=$!

for _ in $(seq 1 150); do
    grep -qF "waltex: M6a ok" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

# un attimo perche' i task girino
sleep 1

kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

# La sequenza dei task: solo le A e le B emesse dopo il marker.
SEQ=$(sed -n '/waltex: M6a ok/,$p' "$LOG" | tr -cd 'AB')

if [ -z "$SEQ" ]; then
    echo "FAIL -- nessuna A o B sulla seriale: i task non hanno girato"
    echo "--- output seriale ---"; tail -20 "$LOG"
    exit 1
fi

# Conta quante volte il carattere cambia rispetto al precedente.
ALTERNANZE=$(python3 -c "
s = '''$SEQ'''
print(sum(1 for a, b in zip(s, s[1:]) if a != b))
")

echo "sequenza di $(printf %s "$SEQ" | wc -c) caratteri, $ALTERNANZE alternanze"
echo "inizio: $(printf %s "$SEQ" | head -c 40)"

if [ "$ALTERNANZE" -ge "$ALTERNANZE_MINIME" ]; then
    echo "ok   -- i due task si alternano ($ALTERNANZE >= $ALTERNANZE_MINIME)"
    exit 0
fi

echo "FAIL -- alternanze insufficienti: $ALTERNANZE < $ALTERNANZE_MINIME"
echo "        una sequenza tipo AAAA...BBBB significa che il controllo e'"
echo "        passato una volta sola invece di andare e tornare"
exit 1
