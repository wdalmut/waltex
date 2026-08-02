#!/usr/bin/env bash
# Verifica che i task si alternino, e che il cambio sia INVOLONTARIO.
#
# Due proprieta' distinte, e servono entrambe.
#
# 1. Le transizioni. Non basta che compaiano sia A sia B: venti A seguite da
#    venti B sono una successione, non un'alternanza. Contiamo quante volte il
#    carattere cambia rispetto al precedente.
#
# 2. La lunghezza delle corse, e questa distingue M6b da M6a. In M6a ogni
#    carattere era seguito da una task_yield, quindi le corse erano tutte di
#    lunghezza 1 e la sequenza era ABABAB. In M6b nessuno cede: un task stampa
#    per tutto il suo quanto, quindi le corse sono lunghe. Corse di lunghezza 1
#    significherebbero che i task stanno ancora cedendo volontariamente, cioe'
#    che la prelazione non c'e' e stiamo guardando il comportamento di prima.
#
# Da M7 i due task partono silenziosi e li accende il comando "spin", digitato
# qui sotto attraverso il monitor di QEMU. Il test ne esce MIGLIORE: fino a M6b
# si appoggiava a un effetto collaterale del kernel — due task che stampavano
# sempre — e adesso provoca da se' la condizione che misura.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}

# Da M10 il disco va attaccato anche qui, non solo in tests/disk.sh: i
# self-check dell ATA girano a ogni boot, e senza immagine kmain si ferma su
# "N selftest falliti" prima di stampare qualunque marker.
DISK=${2:-build/disk.img}
MINIXIMG=${3:-build/minix.img}
TRANSIZIONI_MINIME=20
CORSA_MINIMA=10

# Il marker di fine boot, non il prompt: il prompt non ha un ritorno a capo in
# fondo, quindi cercarlo con grep su un file che cresce e' una corsa.
PRONTO="waltex: M7 ok"

LOG=$(mktemp)
MON=$(mktemp -u)
trap 'rm -f "$LOG" "$MON"' EXIT

qemu-system-i386 -kernel "$KERNEL" -display none -no-reboot \
    -drive file="$DISK",format=raw,if=ide,index=0,cache=writethrough \
    -drive file="$MINIXIMG",format=raw,if=ide,index=1,cache=writethrough \
    -serial "file:$LOG" -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &
QPID=$!

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

# Accende i due task di prova. "ret" e' il nome che sendkey vuole per Invio:
# "enter" viene rifiutato dal monitor.
python3 tests/sendkeys.py "$MON" s p i n ret

# un secondo perche' il timer commuti un centinaio di volte
sleep 1

kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

# Si parte dal messaggio di "spin", non dal marker di boot: prima di quello non
# c'e' niente da contare. Ne' il comando digitato ne' il suo messaggio
# contengono A o B, quindi tr non ha nulla da confondere.
SEQ=$(sed -n '/cominciano a stampare/,$p' "$LOG" | tr -cd 'AB')

if [ -z "$SEQ" ]; then
    echo "FAIL -- nessuna A o B sulla seriale: i task non hanno girato"
    echo "        (il comando spin e' arrivato al prompt?)"
    echo "--- output seriale ---"; tail -20 "$LOG"
    exit 1
fi

read -r TRANSIZIONI CORSA_MAX CORSA_MEDIA <<EOF
$(python3 -c "
s = '''$SEQ'''
trans = sum(1 for a, b in zip(s, s[1:]) if a != b)
corse, n = [], 1
for a, b in zip(s, s[1:]):
    if a == b:
        n += 1
    else:
        corse.append(n); n = 1
corse.append(n)
print(trans, max(corse), sum(corse) // len(corse))
")
EOF

echo "caratteri: $(printf %s "$SEQ" | wc -c)   transizioni: $TRANSIZIONI"
echo "corse: massima $CORSA_MAX, media $CORSA_MEDIA"

if [ "$TRANSIZIONI" -lt "$TRANSIZIONI_MINIME" ]; then
    echo "FAIL -- transizioni insufficienti: $TRANSIZIONI < $TRANSIZIONI_MINIME"
    echo "        il controllo e' passato troppo poche volte"
    exit 1
fi

if [ "$CORSA_MAX" -lt "$CORSA_MINIMA" ]; then
    echo "FAIL -- corse troppo corte: massima $CORSA_MAX < $CORSA_MINIMA"
    echo "        corse di lunghezza 1 significano che i task stanno cedendo"
    echo "        volontariamente: e' M6a, non prelazione"
    exit 1
fi

echo "ok   -- i task si alternano e il cambio e' involontario"
exit 0
