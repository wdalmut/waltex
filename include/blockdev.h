#ifndef WALTEX_BLOCKDEV_H
#define WALTEX_BLOCKDEV_H

#include "types.h"

/* Un dispositivo a BLOCCHI, che e' una cosa diversa da struct device.

   struct device di M8 ha read(d, buf, n): una posizione non c'e', perche' un
   dispositivo a caratteri non ne ha una. Un disco si', e la sua unita' non e'
   il byte:

                    struct device (M8)        struct blockdev (M10)
     unita'         il byte                   il SETTORE da 512 byte
     indirizzo      non esiste                lba, esplicito in ogni chiamata
     letture corte  normali                   NON esistono: o tutto o errore
     chi consuma    devfs, cioe' il VFS       minixfs, in M11

   La terza riga e' la differenza di sostanza. Su una tastiera "ho letto 3 byte
   su 64" e' un esito normale; su un disco e' un guasto. Mettere i due sotto la
   stessa interfaccia costringerebbe uno dei due a mentire. */

#define SECTOR_SIZE   512
#define BLK_NAME_MAX  16

struct blockdev {
   char     name[BLK_NAME_MAX];   /* "hda", "hdb" */
   uint32_t nsectors;             /* la capacita', da IDENTIFY */

   /* Ritornano quanti SETTORI hanno trasferito, oppure -1. Non byte: la
      granularita' e' il settore, e un trasferimento parziale di settore non
      esiste.

      Il primo argomento e' il proprio struct blockdev, per la stessa ragione
      di struct device in M8 — e qui, a differenza di M8, serve subito: ata.c
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
