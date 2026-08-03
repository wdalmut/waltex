#ifndef WALTEX_PROCFS_H
#define WALTEX_PROCFS_H

#include "types.h"
#include "task.h"
#include "vfs.h"

/* Il filesystem di M11d, e il primo che non ha NIENTE sotto: non un disco come
   minix, non un registro come devfs. Il contenuto di /proc/0/status non esiste
   da nessuna parte finche' qualcuno non lo chiede — lo GENERA la read, e smette
   di esistere quando ritorna.

     /proc              directory: le voci sono gli indici dei task ATTIVI
     /proc/<N>          directory: una voce sola, "status"
     /proc/<N>/status   foglia: il testo costruito dalla tabella dei task

   Tre livelli e non due, con /proc/<N> che e' una directory invece di essere
   direttamente il file. Costa due funzioni da dieci righe e le ripaga in M13:
   /proc/<N>/maps e' la ragione piu' forte per avere procfs — sara' per il paging
   quello che peek e' per la memoria — e con la forma piatta bisognerebbe
   cambiare TIPO a /proc/<N>, cioe' rifare tutto cio' che ci si appoggia.

   Di sola lettura: write e create restano a zero nelle inode_ops, quindi
   "mkdir /proc/x" fallisce da se' — la convenzione di M8, per la quarta volta.

   L'INDICE DEL TASK STA IN ino, E NON C'E' NESSUN struct task * DA NESSUNA
   PARTE. In M16 i task escono e i loro slot vengono riusati: un inode che
   tenesse un puntatore riporterebbe in silenzio su un processo diverso. E' la
   conclusione OPPOSTA a quella della tabella di mount in vfs.c, ed e' giusto che
   sia opposta — uno slot della cache di inode non viene mai riciclato, uno slot
   della tabella dei task si'. */

/* Numerazione degli inode, esplicita e in un posto solo, perche' e' un PATTO fra
   lookup, che li assegna, e read, che ci risale.

   La radice e' 1 e i task partono da 2, quindi nessun inode vale zero — che per
   convenzione del progetto significa "nessuno", ed e' il valore con cui una voce
   di directory minix dice "cancellata". */
#define PROC_INO_ROOT           1u
#define PROC_INO_TASK(n)        (2u + (uint32_t)(n))
#define PROC_INO_STATUS(n)      (2u + MAX_TASKS + (uint32_t)(n))

/* E le inverse. Chi le usa deve GIA' sapere che tipo di inode ha in mano:
   applicare quella sbagliata da' un indice plausibile e falso, che e' il genere
   di errore che costa piu' a diagnosticare perche' tutto il resto funziona. */
#define PROC_TASK_DA_DIR(ino)     ((int)((ino) - 2u))
#define PROC_TASK_DA_STATUS(ino)  ((int)((ino) - 2u - MAX_TASKS))

/* Il testo di uno status sta in un buffer di questa dimensione, e il buffer e'
   LOCALE a chi genera — mai statico. Statico costerebbe questi byte una volta
   sola, ma fra il "genero" e il "copio" ci sta un tick del timer, e due
   cat su due status diversi in parallelo si mescolerebbero. Sullo stack non e'
   condiviso con nessuno, e non serve nessuna sezione critica. */
#define PROC_STATUS_MAX  128

/* Riempie gli inode statici — 1 + MAX_TASKS + MAX_TASKS.

   NON legge la tabella dei task, e non e' un dettaglio: riempie TUTTI gli slot,
   attivi o no, perche' sono lookup e readdir a decidere quali far vedere AL
   MOMENTO DELLA DOMANDA. Un procfs_init che leggesse la tabella darebbe un /proc
   congelato all'istante del boot, e "spin" avvierebbe due task senza che /proc
   se ne accorga.

   Da cui: nessun vincolo d'ordine rispetto a task_init. E' l'opposto di
   devfs_init, che DEVE venire dopo tutte le *_init dei driver proprio perche' il
   registro lo legge una volta sola. */
void procfs_init(void);

/* La directory da passare a vfs_mount.

   Si chiama procfs_procdir e non procfs_root per stare accanto a devfs_devdir:
   il nome dice "la directory che si monta", che e' l'unica cosa che il chiamante
   deve sapere. Qui, a differenza di devfs, coincide con la radice del
   filesystem — devfs ha una radice a parte perche' la sua contiene una sola voce
   chiamata "dev", e montare quella darebbe /dev/dev/kbd.

   Ritorna 0 se procfs_init non e' ancora stata chiamata: con una radice fatta di
   zeri il VFS avrebbe type INODE_NONE e ops nullo, quindi vfs_mount rifiuterebbe
   con un -1 che non dice perche'. Stessa scelta di devfs_root() e
   minixfs_root(). */
struct inode *procfs_procdir(void);

#endif
