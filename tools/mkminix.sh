#!/usr/bin/env bash
# Costruisce l'immagine di riferimento minix v1 per M11.
#
# E' il riferimento INDIPENDENTE della milestone, come l'orologio CMOS lo e' per
# il timer di M4 e come tools/mkdisk.sh lo e' per il driver ATA — ma qui e' piu'
# forte di entrambi: il filesystem lo scrive mkfs.minix di util-linux, e il
# contenuto ce lo mette il modulo minix del kernel Linux attraverso mount. Sono
# due implementazioni vere, e nessuna delle due e' la nostra.
#
# VUOLE sudo, perche' montare e' privilegiato. Per questo l'immagine prodotta si
# COMMITTA in tests/data/minix.img e questo script serve solo a rigenerarla:
# "make test" non puo' volere sudo.
#
# Committarla non indebolisce niente e in piu' la rende stabile: i test
# confrontano sempre contro gli stessi byte, invece che contro l'umore della
# versione di util-linux installata sulla macchina di turno.
#
#   -1      minix versione 1, quella di Tanenbaum e di fs/ in Linux 0.01
#   -n 14   nomi da 14 caratteri, magic 0x137F. La variante da 30 (0x138F) ha
#           voci di directory da 32 byte invece di 16, e minixfs.c la rifiuta.
set -euo pipefail

OUT=${1:-tests/data/minix.img}
KB=256                      # 96 inode, prima zona dati 7. Piccola apposta:
                            # finisce in git, ed e' quasi tutta zeri.

if ! command -v mkfs.minix >/dev/null; then
    echo "mkminix: manca mkfs.minix (sudo apt install util-linux)" >&2
    exit 1
fi

if ! sudo -n true 2>/dev/null; then
    echo "mkminix: serve sudo per montare l'immagine." >&2
    echo "         L'immagine committata in $OUT va gia' bene per i test:" >&2
    echo "         questo script serve solo a rigenerarla." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

# Si riparte SEMPRE da zero. Un'immagine aggiornata invece che ricostruita
# porterebbe con se' lo stato della volta prima, ed e' lo stesso motivo per cui
# tests/disk.sh rifa' la sua.
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1024 count=$KB status=none
mkfs.minix -1 -n 14 "$OUT" >/dev/null

MNT=$(mktemp -d)
# La trap smonta anche se qualcosa fallisce a meta': un loop device appeso e'
# una di quelle cose che poi si scoprono tre giorni dopo.
trap 'sudo umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT

sudo mount -o loop -t minix "$OUT" "$MNT"

# Ogni file prova una cosa diversa, e insieme coprono i rami di zona_di:
#
#   hello.txt    26 byte      una zona sola, il caso base
#   etc/motd     piccolo      una lookup a DUE livelli, ed e' quello che
#                             cat leggera' dentro la VM
#   grande.txt   5000 byte    cinque zone DIRETTE
#   enorme.txt   20000 byte   sfonda le 7 dirette: esercita l'INDIRETTO
#   vuoto.txt    0 byte       size 0, e zone[0] a zero
#   dev/         directory VUOTA. Non e' un file di prova: e' il PUNTO DI MOUNT
#                di devfs, e da M11c esiste sul disco perche' e' cosi' che
#                funziona mount in Unix — si COPRE una directory che c'e' gia',
#                non si aggiunge un nome. Il guadagno e' che minix_readdir non
#                deve sapere niente dei mount: il nome "dev" glielo da' il disco.
#                Va creata per ULTIMA, cosi' i numeri di inode dei file di prova
#                non si spostano e hello.txt resta l'inode 2, su cui c'e' un
#                controllo host.
#
# enorme.txt e' quello che conta: senza un file oltre i 7168 byte, la mappatura
# delle zone si prova solo nel ramo facile, e il bug dei puntatori letti come
# uint32 invece che uint16 non lo vedrebbe nessuno.
sudo sh -c "
    set -e
    printf 'ciao dal filesystem minix\n'            > '$MNT/hello.txt'
    mkdir -p '$MNT/etc'
    printf 'waltex M11: minix v1, sola lettura\n'   > '$MNT/etc/motd'
    head -c 5000  /dev/zero | tr '\\0' 'G'          > '$MNT/grande.txt'
    head -c 20000 /dev/zero | tr '\\0' 'Z'          > '$MNT/enorme.txt'
    : > '$MNT/vuoto.txt'
    mkdir -p '$MNT/dev'
"

sudo ls -la "$MNT" "$MNT/etc"
sudo umount "$MNT"
trap - EXIT
rmdir "$MNT"

echo
echo "mkminix: $OUT, $KB KB"
echo "         magic:  $(od -An -tx2 -j1040 -N2 "$OUT" | tr -d ' ')   (atteso 137f)"
echo "         radice: $(od -An -tu2 -j4096 -N2 "$OUT" | tr -d ' ')  come i_mode"
