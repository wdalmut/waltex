#ifndef WALTEX_CHARDEV_H
#define WALTEX_CHARDEV_H

#include "types.h"

/* Un dispositivo a CARATTERI: due operazioni e un puntatore opaco, e nient'altro.

   Nome, major e minor NON stanno piu' qui: da M11e vivono nella voce del
   registry, struct dev_entry in dev.h. La ragione e' che sono le uniche cose che
   un disco e una tastiera hanno in comune — l'identita' — mentre le operazioni
   sono cio' che li rende incompatibili. Tenere l'identita' insieme alle
   operazioni e' cio' che impediva a un disco di iscriversi.

   I dispositivi a BLOCCHI hanno un'interfaccia propria, struct blockdev in
   blockdev.h, perche' la loro granularita' e' il settore e non il byte. La
   tabella in testa a quel file dice esattamente in che cosa differiscono.

   ATTENZIONE: QUESTA STRUCT NON VIENE PIU' COPIATA DAL REGISTRY.

   dev_entry.impl ne conserva l'INDIRIZZO, quindi deve sopravvivere a chi la
   iscrive: static o globale, mai una locale di funzione. Fino a M11d era il
   contrario — chardev_register copiava l'intera struct, e i tre driver la
   riempivano sullo stack, con un commento in serial_init che spiegava perche' era
   sicuro. Adesso quel commento dice l'opposto.

   Il guasto, se qualcuno la mette sullo stack, non si vede provando: il kernel
   boota, ls /dev funziona, e l'assert sull'iscrizione e' verde. Arriva al primo
   read dopo che quello stack e' stato riusato, come un salto in un indirizzo
   arbitrario — lontano dalla causa, come il frame falsificato di task_create e la
   zona non azzerata di M11b. Memoria che ha l'aria di essere giusta.

   Il NOME invece si copia ancora, dentro dev_entry. La convenzione di M8 si e'
   spezzata in due, ed e' scritto in dev.h.

   read e write ricevono il proprio struct chardev come primo argomento, cosi' un
   driver puo' iscrivere due dispositivi con la STESSA funzione e sapere quale dei
   due sta servendo. Per quello esiste anche priv. In M8 non serviva a niente e
   gli adattatori lo ignoravano; il primo a esercitarlo davvero e' stato ata.c in
   M11a, con due dischi e un solo puntatore a read.

   Un puntatore a operazione NULLO significa "questo dispositivo non fa quella
   cosa", non "errore": console non si legge, kbd non si scrive. E' la stessa
   convenzione di exc_handlers[vec] == 0 in idt.c, ed e' cio' che permette a
   devio_caps di descrivere le capacita' senza un campo apposta.

   Ma ATTENZIONE a cosa se ne conclude un piano sopra: chr_inode_ops, davanti a
   un'operazione non supportata, ritorna -1 e NON 0. La convenzione del puntatore
   nullo descrive il DISPOSITIVO; il valore consegnato a chi ha chiesto di leggere
   deve essere -1, perche' uno zero significherebbe "adesso non c'e' niente" e
   nessuno dei due chiamanti puo' distinguerlo da un rifiuto. E' il return 1 di
   chardev_read che in M9b faceva avanzare l'offset a cat su un buffer che nessuno
   aveva riempito. */
struct chardev {
    /* Ritornano quanti byte hanno trasferito DAVVERO, oppure -1 su errore.

       Per read, uno zero significa "adesso non c'e' niente", NON "fine del
       file", e la distinzione regge tutto M9: cat /dev/kbd fa spin proprio su
       quello zero, e se volesse dire "finito" uscirebbe invece di aspettare che
       si digiti.

       E' anche la differenza con un dispositivo a BLOCCHI, dove lo zero significa
       EOF vero: la' c'e' una dimensione, quindi la fine esiste. Per questo
       shell_cat guarda il TIPO dell'inode per decidere la condizione d'uscita, e
       non il valore di ritorno.

       Nessuna delle due deve bloccare: il blocking I/O manca per scelta, sta
       nello spec sotto "fuori scope", e il punto di decisione e' M9. */
    int (*read )(struct chardev *d, void *buf, uint32_t n);
    int (*write)(struct chardev *d, const void *buf, uint32_t n);

    void *priv;
};

/* L'iscrizione NON e' qui: sta in devio.h, come chardev_register(name, major,
   minor, c). E' li' perche' deve conoscere sia questa struct sia il registry, e
   dev.c non conosce nessuna delle due specie — e' cio' che lo rende agnostico.

   Per la stessa ragione non ci sono piu' chardev_find, chardev_by_id,
   chardev_count e chardev_at: le ricerche sul registry sono dev_lookup_index,
   dev_get, dev_by_id e dev_count in dev.h, e per ottenere un struct chardev * da
   un nome c'e' dev_chardev in devio.h, che controlla kind prima di castare. */

#endif
