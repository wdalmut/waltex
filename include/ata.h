#ifndef WALTEX_ATA_H
#define WALTEX_ATA_H

#include "types.h"
#include "blockdev.h"

/* Driver ATA PIO in POLLING per il canale primario.

   In polling e non a interrupt, deliberatamente: l'IRQ 14 esiste e non lo
   usiamo. Senza blocking I/O un gestore non avrebbe nessuno da svegliare, e il
   ciclo di attesa dovrebbe comunque esserci — quindi l'interrupt costerebbe
   complessita' senza togliere il ciclo. Con la prelazione di M6b gli altri task
   girano lo stesso mentre questo aspetta.

   Solo LBA28: 28 bit di indirizzo sono 128 GiB, e l'immagine di M11 sara' di
   qualche megabyte. LBA48 aggiunge un secondo giro di scritture sui registri e
   non serve a niente qui.

   Solo il canale primario, 0x1F0-0x1F7 piu' 0x3F6. Il secondario (0x170, 0x376)
   raddoppierebbe la superficie senza aggiungere un concetto. */

/* Cerca i due dischi del canale primario e li identifica.

   NON fallisce, e non fa assert: un canale vuoto e' una configurazione
   legittima — "make run" senza -drive deve continuare a funzionare — e in quel
   caso ata_drive_count() ritorna 0. E' la differenza con device_register, dove
   l'assert e' giusto perche' un driver che non riesce a iscriversi e' un bug
   nostro; qui l'assenza e' una proprieta' dell'ambiente.

   In polling non ha vincoli d'ordine veri: la sua posizione in kmain e' solo la
   convenzione del progetto, ogni sottosistema con la sua *_init() esplicita. */
void ata_init(void);

/* Il disco i, oppure 0 se i e' negativo, oltre l'ultimo, o se ata_init non e'
   stata chiamata. Nessuna inizializzazione implicita: e' un vincolo di
   CLAUDE.md, e vale anche qui.

   Il puntatore e' allo slot vero, non a una copia: chi lo riceve chiamera'
   b->read(b, ...) e b deve essere quello con il priv giusto.

   Esiste per la stessa ragione di device_at in M8 e di task_slot in M6a:
   l'array e' static dentro ata.c, e serve enumerarlo da fuori senza renderlo
   globale. In M11 kmain fara' minixfs_init(ata_drive(0)), che e' lo stesso
   gesto di vfs_init(devfs_root()). */
struct blockdev *ata_drive(int i);

int ata_drive_count(void);

#endif
