#!/usr/bin/env bash
# Il controllo che chiude M11b: il kernel crea, e un'implementazione che non e'
# la nostra dice se ci ha creduto.
#
# In M11a il verso era l'opposto — mkfs.minix scriveva e il nostro parser
# leggeva. Qui scrive il kernel, e a giudicare sono fsck.minix e mount.
#
#   1. build/minix.img si rifa' dal riferimento committato   (stato noto)
#   2. si verifica che /nuovo NON ci sia gia'                 (non parte vinto)
#   3. la VM parte, e dal prompt si digita mkdir e write
#   4. si chiude QEMU dal monitor con "quit"
#   5. FSCK.MINIX sull'host                                   <- l'oracolo vero
#   6. mount + cat, se c'e' sudo                              <- il contenuto
#
# Il passo 5 e' il punto, e mount da solo NON basterebbe. Misurato: spegnendo un
# bit nella bitmap degli inode dell'immagine buona, mount la accetta e ls
# funziona benissimo, mentre fsck dice
#
#     Inode 2 marked unused, but used for file '/hello.txt'
#
# ed esce con 4. Un filesystem incoerente si legge: il danno esce alla PROSSIMA
# allocazione, quando quell'inode viene riusato e due file finiscono sopra lo
# stesso. E' il genere di guasto che compare tre operazioni dopo la causa.
#
# fsck NON vuole sudo, perche' lavora su un file. mount si', quindi il passo 6 si
# salta con un avviso quando sudo non c'e': make test non puo' volerlo.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}
DISK=${2:-build/disk.img}
MINIXIMG=${3:-build/minix.img}

PRONTO="waltex: M7 ok"
RIFERIMENTO=tests/data/minix.img

ATTESO="salve dal kernel"

MON=$(mktemp -u)
LOG=$(mktemp)
trap 'rm -f "$LOG" "$MON"' EXIT

# 1. Stato noto: SEMPRE dal riferimento, mai riusando l'immagine di prima. Un
#    test che scrive nel proprio input non e' ripetibile — la seconda esecuzione
#    passerebbe anche con la scrittura rotta, perche' il file ci sarebbe gia'.
cp "$RIFERIMENTO" "$MINIXIMG"

# 2. E il riferimento non deve contenere gia' quello che cerchiamo. Il controllo
#    e' grezzo apposta: si cerca il nome nei byte dell'immagine, senza montarla.
if grep -qa "ciao.txt" "$MINIXIMG"; then
    echo "FAIL -- il riferimento contiene gia' ciao.txt: il test non misurerebbe niente"
    exit 1
fi

# 3. La VM.
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

# mkdir /nuovo
python3 tests/sendkeys.py "$MON" m k d i r spc slash n u o v o ret
sleep 0.3

# write /nuovo/ciao.txt salve dal kernel
python3 tests/sendkeys.py "$MON" w r i t e spc slash n u o v o slash c i a o \
    dot t x t spc s a l v e spc d a l spc k e r n e l ret
sleep 0.5

# 4. Chiusura pulita: le scritture ancora in volo si versano, e si ASPETTA che
#    il processo sia finito prima di leggere il file.
python3 tests/monitor.py "$MON" quit >/dev/null 2>&1
wait "$QPID" 2>/dev/null

FALLITI=0
pulito() { tr -d '\r' < "$LOG"; }

# I due comandi devono essere esistiti. Senza questo, un "comando non trovato"
# produrrebbe un filesystem intatto — che fsck approva.
if pulito | grep -q "comando non trovato"; then
    echo "FAIL -- la shell non conosce mkdir o write"
    pulito | grep "comando non trovato" | sed 's/^/        /'
    FALLITI=1
fi

# 5. L'ORACOLO. -f forza il controllo anche su un filesystem marcato pulito.
if OUT=$(fsck.minix -f "$MINIXIMG" 2>&1); then
    echo "ok   -- fsck.minix dice che il filesystem e' coerente"
else
    echo "FAIL -- fsck.minix ha trovato incoerenze (exit $?)"
    echo "$OUT" | sed 's/^/        /'
    FALLITI=1
fi

# 6. Il contenuto. Vuole sudo, quindi si salta invece di fallire: fsck ha gia'
#    detto la cosa piu' difficile.
if sudo -n true 2>/dev/null; then
    MNT=$(mktemp -d)
    trap 'sudo umount "$MNT" 2>/dev/null; rmdir "$MNT" 2>/dev/null; rm -f "$LOG" "$MON"' EXIT

    if sudo mount -o loop -t minix "$MINIXIMG" "$MNT" 2>/dev/null; then
        if [ -d "$MNT/nuovo" ]; then
            echo "ok   -- la directory creata dal kernel esiste sull'host"
        else
            echo "FAIL -- /nuovo non c'e' sull'immagine"
            FALLITI=1
        fi

        # Il contenuto, e non solo l'esistenza: un file creato e vuoto
        # passerebbe il controllo di sopra e fsck insieme.
        if [ -f "$MNT/nuovo/ciao.txt" ] &&
           grep -q "$ATTESO" "$MNT/nuovo/ciao.txt" 2>/dev/null; then
            echo "ok   -- e il file contiene quello che il kernel ha scritto"
        else
            echo "FAIL -- /nuovo/ciao.txt manca o ha il contenuto sbagliato"
            [ -f "$MNT/nuovo/ciao.txt" ] &&
                echo "        trovato: '$(cat "$MNT/nuovo/ciao.txt")'"
            FALLITI=1
        fi

        sudo umount "$MNT"
    else
        echo "FAIL -- mount rifiuta l'immagine scritta dal kernel"
        FALLITI=1
    fi

    rmdir "$MNT" 2>/dev/null
else
    echo "     -- salto mount e cat: serve sudo. fsck ha gia' controllato la"
    echo "        coerenza, che e' la parte difficile."
fi

exit "$FALLITI"
