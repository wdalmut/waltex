#!/usr/bin/env bash
# Costruisce l'immagine di prova del disco per M10.
#
# L'immagine e' il RIFERIMENTO INDIPENDENTE della milestone, nello stesso senso
# in cui l'orologio CMOS lo e' per il timer di M4: un disco non puo' verificare
# se stesso, e un driver che leggesse e riscrivesse solo la propria spazzatura
# passerebbe qualunque controllo interno. Qui il pattern lo genera un programma
# che non e' il kernel, e il kernel deve ritrovarlo identico.
#
#   LBA 0    firma "waltex-disk-v1", poi zeri
#   LBA 1    pattern deterministico          <- il kernel lo rilegge
#   LBA 2    settore di prova, azzerato      <- il kernel ci scrive,
#                                               l'host lo rilegge con od
#   LBA 3+   zeri, 2048 settori in tutto (1 MiB)
#
# Il pattern e' byte[i] = (i * 7 + 3) & 0xFF: deterministico, e NON costante.
# Un memset di un valore solo passerebbe anche con un driver che legge sempre
# lo stesso settore, o che legge mezzo settore e lascia l'altra meta' a caso.
#
# I settori 0-2 stanno nel boot block, che minix non usa: in M11 questa immagine
# sara' fatta con mkfs.minix e i tre settori potranno restare dove sono.
set -euo pipefail

OUT=${1:-build/disk.img}
NSECTORS=2048               # 1 MiB. Il numero e' verificato da un self-check:
                            # cambiarlo qui richiede di cambiarlo anche li'.
FIRMA="waltex-disk-v1"

mkdir -p "$(dirname "$OUT")"

# Si scrive l'immagine INTERA in una volta, non si aggiorna quella esistente:
# un test che scrive nel proprio input non e' ripetibile, e la seconda
# esecuzione passerebbe anche con la scrittura del kernel rotta, perche' il
# settore 2 conterrebbe gia' il risultato atteso della volta prima.
python3 - "$OUT" "$NSECTORS" "$FIRMA" <<'EOF'
import sys

out, nsectors, firma = sys.argv[1], int(sys.argv[2]), sys.argv[3]
SECTOR = 512

img = bytearray(nsectors * SECTOR)

# LBA 0: la firma, il resto zeri.
img[0:len(firma)] = firma.encode('ascii')

# LBA 1: il pattern.
for i in range(SECTOR):
    img[SECTOR + i] = (i * 7 + 3) & 0xFF

# LBA 2 e oltre: gia' zeri.

with open(out, 'wb') as f:
    f.write(img)
EOF

echo "mkdisk: $OUT, $NSECTORS settori ($((NSECTORS / 2)) KB)"
