#!/usr/bin/env bash
# Il controllo BIDIREZIONALE di M10, e il solo test del progetto in cui la
# verifica avviene fuori dalla macchina che ha fatto il lavoro.
#
# Gli altri controlli del disco vivono nei self-check, dentro la VM, e provano
# che il kernel CREDE di aver scritto. Questo prova che sul file c'e' davvero:
#
#   1. l'immagine si ricostruisce da zero          (stato noto)
#   2. si verifica che il settore 2 sia a zeri     (il test non parte gia' vinto)
#   3. la VM parte, e i self-check ci scrivono
#   4. si chiude QEMU dal monitor con "quit"       (le scritture si versano)
#   5. l'HOST rilegge il settore 2 con od e confronta
#
# Il passo 2 non e' cerimonia: un test che scrive nel proprio input non e'
# ripetibile, e senza quel controllo la seconda esecuzione passerebbe anche con
# la scrittura completamente rotta, perche' il settore 2 conterrebbe gia' il
# risultato atteso della volta prima.
#
# Il passo 4 e' la parte fragile. tests/shell.sh ammazza QEMU con kill, e qui
# non basterebbe: servono un "quit" dal monitor e un wait sul processo prima di
# leggere il file. Insieme a cache=writethrough e al FLUSH CACHE del driver sono
# tre difese indipendenti contro lo stesso guasto.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}
DISK=${2:-build/disk.img}
MINIXIMG=${3:-build/minix.img}

PRONTO="waltex: M7 ok"

# Il pattern che i self-check scrivono nel settore 2: (i * 11 + 5) & 0xFF.
# NON e' quello del settore 1, che mkdisk.sh genera con (i * 7 + 3) & 0xFF, e la
# differenza e' voluta: se fossero uguali, una write che non facesse niente
# passerebbe grazie a una read che sbaglia settore.
ATTESI="05 10 1b 26 31 3c 47 52 5d 68 73 7e 89 94 9f aa"

MON=$(mktemp -u)
trap 'rm -f "$MON"' EXIT

# 1. Stato noto. Si ricostruisce SEMPRE, anche se make la considera aggiornata.
./tools/mkdisk.sh "$DISK" >/dev/null
cp tests/data/minix.img "$MINIXIMG"

# 2. Il settore 2 deve essere a zeri prima di cominciare.
PRIMA=$(od -An -tx1 -j1024 -N16 "$DISK" | tr -s ' ' | sed 's/^ //;s/ $//')

if [ "$PRIMA" != "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00" ]; then
    echo "FAIL -- il settore 2 non parte a zeri: '$PRIMA'"
    echo "        l'immagine non e' stata ricostruita, il test non misurerebbe niente"
    exit 1
fi

# 3. La VM. I self-check scrivono il settore 2 durante il boot: non serve
#    digitare niente, e cosi' questo test non dipende da nessun comando della
#    shell.
LOG=$(mktemp)
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
    echo "FAIL -- il kernel non ha raggiunto il marker finale"
    echo "--- output seriale ---"; tr -d '\r' < "$LOG" | tail -20
    exit 1
fi

# 4. Chiusura pulita, e si ASPETTA che il processo sia finito prima di leggere.
python3 tests/monitor.py "$MON" quit >/dev/null 2>&1
wait "$QPID" 2>/dev/null

# 5. La rilettura dall'host.
DOPO=$(od -An -tx1 -j1024 -N16 "$DISK" | tr -s ' ' | sed 's/^ //;s/ $//')

FALLITI=0

if [ "$DOPO" = "$ATTESI" ]; then
    echo "ok   -- il settore scritto dal kernel si rilegge dall'host"
else
    echo "FAIL -- il settore 2 sul file non e' quello che il kernel ha scritto"
    echo "        atteso: $ATTESI"
    echo "        trovato: $DOPO"
    FALLITI=1
fi

# E il settore 1 non deve essere cambiato: il kernel lo legge e basta. Se
# risultasse modificato, ata_dev_write starebbe scrivendo al posto sbagliato —
# guasto che nessun controllo dentro la VM puo' vedere, perche' anche la read
# sbaglierebbe nello stesso modo.
S1=$(od -An -tx1 -j512 -N8 "$DISK" | tr -s ' ' | sed 's/^ //;s/ $//')

if [ "$S1" = "03 0a 11 18 1f 26 2d 34" ]; then
    echo "ok   -- il settore di sola lettura e' intatto"
else
    echo "FAIL -- il settore 1 e' stato modificato: '$S1'"
    FALLITI=1
fi

# E il settore 3 deve essere ancora a zeri. E' il controllo che copre il buco
# che nessun altro vede: una write che trasferisce count + 1 settori scrive
# oltre quello richiesto, e dentro la VM tutto torna — il settore 2 riletto e'
# giusto, e nessun self-check guarda il 3. Da fuori si vede.
#
# Vale la pena sapere quali guasti questo test NON aggiunge: un settore
# sbagliato in lettura o in scrittura lo prendono gia' i self-check, perche' i
# tre settori dell'immagine hanno contenuti distinti. E il FLUSH CACHE mancante
# non lo prende nessuno dei due, perche' con cache=writethrough e una chiusura
# pulita QEMU versa comunque — verificato togliendolo. Il flush resta perche' su
# hardware vero e' l'unica garanzia, non perche' un test lo imponga.
S3=$(od -An -tx1 -j1536 -N16 "$DISK" | tr -s ' ' | sed 's/^ //;s/ $//')

if [ "$S3" = "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00" ]; then
    echo "ok   -- la scrittura non ha toccato il settore successivo"
else
    echo "FAIL -- il settore 3 non e' piu' a zeri: '$S3'"
    FALLITI=1
fi

exit "$FALLITI"
