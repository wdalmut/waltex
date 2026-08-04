#ifndef WALTEX_BLOCKDEV_H
#define WALTEX_BLOCKDEV_H

#include "types.h"
#include "dev.h"

/* Un dispositivo a BLOCCHI, che e' una cosa diversa da struct chardev.

   struct chardev ha read(d, buf, n): una posizione non c'e', perche' un
   dispositivo a caratteri non ne ha una. Un disco si', e la sua unita' non e'
   il byte:

                    struct chardev            struct blockdev
     unita'         il byte                   il SETTORE da 512 byte
     indirizzo      non esiste                lba, esplicito in ogni chiamata
     letture corte  normali                   NON esistono: o tutto o errore
     chi consuma    devio, che ne fabbrica    devio per la vista a byte,
                    la vista a byte           e MINIXFS in LBA, senza
                                              passare da li'

   La terza riga e' la differenza di sostanza. Su una tastiera "ho letto 3 byte
   su 64" e' un esito normale; su un disco e' un guasto. Mettere i due sotto la
   stessa interfaccia costringerebbe uno dei due a mentire — ed e' esattamente il
   motivo per cui il registro di M8 non poteva accogliere un disco, e per cui da
   M11e conosce solo l'identita'.

   La quarta riga e' la cosa da non perdere di vista in M11e: minixfs parla a
   QUESTA interfaccia, in LBA, e non passa dall'adapter. Se un giorno passasse,
   ogni lettura di un blocco minix costerebbe una traduzione byte->LBA per tornare
   al settore da cui era partita. L'adapter serve a chi vede il disco come un
   file, non a chi ci vede un filesystem. */

#define SECTOR_SIZE   512

struct blockdev {
   /* DEV_NAME_MAX da dev.h, e non un BLK_NAME_MAX proprio: da M11e il registry e'
      uno, quindi il limite del nome e' uno. Due costanti con lo stesso valore
      sono due verita' che possono divergere, ed e' lo stesso ragionamento per cui
      il conteggio del registry e' tenuto invece di ricalcolato. */
   char     name[DEV_NAME_MAX];   /* "hda", "hdb" */
   uint32_t nsectors;             /* la capacita', da IDENTIFY */

   /* Ritornano quanti SETTORI hanno trasferito, oppure -1. Non byte: la
      granularita' e' il settore, e un trasferimento parziale di settore non
      esiste.

      Il primo argomento e' il proprio struct blockdev, per la stessa ragione
      di struct chardev in M8 — e qui, a differenza di M8, serve subito: ata.c
      iscrive due dischi con la STESSA funzione read, e priv e' cio' che le
      dice quale dei due sta leggendo.

      Nessuna delle due avanza una posizione, perche' una posizione non c'e':
      l'indirizzo arriva a ogni chiamata. E' lo stesso motivo per cui
      inode_ops->read riceve l'offset esplicito invece di tenerlo — la
      posizione appartiene a chi legge, non a cosa si legge. */
   int (*read )(struct blockdev *b, uint32_t lba, void *buf,       uint32_t count);
   int (*write)(struct blockdev *b, uint32_t lba, const void *buf, uint32_t count);

   void *priv;
};

#endif
