#ifndef WALTEX_MINIXFS_H
#define WALTEX_MINIXFS_H

#include "types.h"
#include "vfs.h"
#include "blockdev.h"

/* Il formato su disco — superblocco, inode, voci di directory — sta in
   kernel/minixfs.c e non qui: l'header e' il contratto d'interfaccia, e
   nessuno fuori da minixfs.c guarda quei byte. Nemmeno i test, che passano
   da ops come ci passa il VFS. */

/* minix versione 1, SOLA LETTURA — la figura canonica di un filesystem Unix:
   superblocco, bitmap degli inode, bitmap delle zone, tabella degli inode, dati.
   E' quella di Tanenbaum e quella che Linux 0.01 implementa in fs/.

   La scrittura arriva in M11b, e non e' una limitazione da nascondere: senza
   allocazione sulle bitmap non c'e' niente in cui creare, esattamente come devfs
   in M9b e' di sola lettura per struttura.

   Il layout, VERIFICATO su un'immagine di mkfs.minix e non ricordato:

     blocco 0        boot block, 1024 byte, minix non lo usa
     blocco 1        superblocco
     blocchi 2..     bitmap degli inode      s_imap_blocks blocchi
     poi             bitmap delle zone       s_zmap_blocks blocchi
     poi             tabella degli inode     s_ninodes * 32 byte
     poi             i dati, dal blocco s_firstdatazone

   Il blocco e' 1024 byte e il settore e' 512: DUE SETTORI PER BLOCCO, ed e'
   l'unica aritmetica che struct blockdev non fa da se'. */

/* Quanti inode possono stare in RAM insieme. E' una cache, e in M11a non e'
   un'ottimizzazione ma correttezza: lookup restituisce un struct inode *, e quel
   puntatore deve puntare a qualcosa che sopravvive alla chiamata — quindi non
   puo' essere una locale. In devfs il problema non c'era, perche' gli inode
   erano tutti statici e tutti presenti.

   E c'e' un secondo motivo: due lookup dello stesso path devono dare lo STESSO
   puntatore. Con due copie, la i_size di una puo' divergere dall'altra.

   Nessun refcount: in M11a non si libera niente, e i refcount arrivano in M16
   con fork e dup. Uno slot esaurito e' un fallimento deterministico e visibile,
   che e' il punto della disciplina statica. */
#define MAX_INODES 64

/* Monta il filesystem che sta su dev.

   Ritorna 0, oppure -1 se dev e' nullo, se il superblocco non si legge, se il
   magic non e' 0x137F, o se s_log_zone_size non e' zero.

   Il magic 0x138F — la variante con nomi da 30 caratteri — si RIFIUTA, e non e'
   pigrizia: con nomi da 30 la voce di directory e' lunga 32 byte invece di 16, e
   leggerla come se fosse da 16 non produce nessun errore. Produce nomi finti e
   numeri di inode presi dal mezzo di un nome, cioe' un filesystem che sembra
   funzionare.

   Stessa cosa per s_log_zone_size: vale zero su ogni immagine che mkfs.minix
   produce, e significa "una zona e' un blocco". Se fosse diverso ogni numero di
   zona andrebbe spostato, e ignorarlo darebbe dati sbagliati in silenzio.

   Su -1 minixfs_root() deve continuare a ritornare 0: un mount fallito a meta',
   con il disco impostato e il superblocco a zeri, e' peggio di uno fallito del
   tutto. */
int minixfs_init(struct blockdev *dev);

/* L'inode della radice — l'inode 1, non lo 0 — da passare a vfs_init. Ritorna 0
   se il mount non e' riuscito, per la stessa ragione di devfs_root(): con una
   radice fatta di zeri il VFS avrebbe type INODE_NONE e ops nullo, quindi OGNI
   resolve fallirebbe senza dire perche'. */
struct inode *minixfs_root(void);

/* Non c'e' nessuna minixfs_graft, e l'assenza e' il punto di M11c.

   Fino a M11b c'era: uno slot dentro minixfs.c, consultato da minix_lookup prima
   del disco ed elencato da minix_readdir come una voce in piu'. Funzionava, e il
   difetto non era che fosse rotta — era che per montare qualcosa bisognava
   MODIFICARE IL FILESYSTEM che possedeva il punto di innesto. Un secondo
   filesystem sotto /mnt avrebbe voluto una graft piu' grande, dentro minixfs.

   Adesso la tabella sta in vfs.c e la sostituzione e' una riga in vfs_resolve,
   quindi minixfs non sa che i mount esistano. E' anche il motivo per cui "dev"
   e' una directory VERA sull'immagine invece di un nome inventato dalla lookup:
   in Unix mount COPRE una directory che c'e' gia', e il guadagno e' che
   minix_readdir non deve accordarsi con nessuno — il nome glielo da' il disco. */

#endif
