#ifndef WALTEX_DEVIO_H
#define WALTEX_DEVIO_H

#include "types.h"
#include "dev.h"

/* IL PONTE fra i due mondi dei dispositivi e il resto del kernel.

   devio.c e' l'UNICO file dove enum dev_kind si apre in uno switch, e non e' una
   preferenza di stile: e' cio' che permette a dev.c di non conoscere le due
   specie e a devfs.c di non conoscere cosa sia un disco. I due divieti sono la
   ragione per cui questo file esiste invece che spargere i suoi contenuti fra
   quegli altri due.

   Chi include cosa, e cosa NON include:

     dev.c      dev.h, memory.h                      il registry, agnostico
     devio.c    dev.h chardev.h blockdev.h vfs.h     il ponte, sa tutto
     devfs.c    dev.h devio.h vfs.h                  l'albero, solo nomi
     vfs.c      NIENTE di tutto questo               non sa cosa sia un device

   L'ultima riga e' la misura binaria della milestone, la stessa che ha dato senso
   a M11d: git diff --stat kernel/vfs.c dev'essere vuoto. */

/* Dichiarazioni anticipate invece degli #include, e non e' avarizia: questo
   header non usa nessun CAMPO di quelle struct, solo puntatori, quindi non ha
   bisogno della loro definizione. Chi include devio.h e vuole guardarci dentro
   include anche chardev.h o blockdev.h da se'.

   Senza queste tre righe, "struct chardev *" dentro un prototipo dichiara un tipo
   incompleto NUOVO, visibile solo in quel prototipo — e gcc lo dice con "struct
   chardev declared inside parameter list". Il tipo del chiamante e quello del
   prototipo diventano due tipi diversi con lo stesso nome. */
struct chardev;
struct blockdev;
struct inode;

/* Le capacita', come maschera. Serve al comando "devs" per la colonna r-/-w, e
   sta qui invece che in shell.c perche' guardare i puntatori a operazione vuole
   sapere di che specie sia impl — cioe' vuole lo switch, che esiste in un posto
   solo.

   Sul disco la colonna guadagna significato: r- vuol dire read-only, che con
   struct device di M8 non era uno stato esprimibile. */
#define DEVIO_CAN_READ  1
#define DEVIO_CAN_WRITE 2

/* I due wrapper: compilano una struct dev_entry e la passano a dev_register.
   Ritornano quello che ritorna lui — 0 iscritto, -1 rifiutato — piu' un rifiuto
   proprio ciascuno.

   L'ASIMMETRIA E' VOLUTA, e un controllo condiviso non potrebbe esprimerla,
   perche' quello distingue solo "almeno uno":

                        read == 0                          write == 0
     chardev   lecito: console non si legge      lecito: kbd non si scrive
     blockdev  RIFIUTATO: un disco da cui        lecito: e' un disco read-only
               non si legge non ha senso

   E' precisamente il controllo che dev_register ha PERSO diventando agnostico, e
   perderlo e' stato un guadagno: in M8 era un solo "entrambi nulli", e non poteva
   dire che un disco write-only e' un errore del driver mentre un disco read-only
   e' un dispositivo legittimo.

   ATTENZIONE: c e b devono SOPRAVVIVERE alla chiamata. Il registry copia il nome
   e conserva il PUNTATORE all'implementazione, quindi una struct locale di
   funzione e' un puntatore penzolante. Vedi il commento in cima a chardev.h.

   Il nome si copia dentro la dev_entry locale con una scansione limitata a
   DEV_NAME_MAX byte — strncpy non esiste in questo progetto, va scritta — e un
   nome troppo lungo dal chiamante deve produrre un RIFIUTO da parte di
   dev_register, non un array senza terminatore. */
int chardev_register (const char *name, uint16_t major, uint16_t minor,
                      struct chardev  *c);
int blockdev_register(const char *name, uint16_t major, uint16_t minor,
                      struct blockdev *b);

/* Lookup TIPIZZATI: rendono l'implementazione se il nome c'e' ED e' della specie
   giusta, altrimenti 0.

   Controllano kind PRIMA di castare impl, e non e' pedanteria: un cast da void *
   che il chiamante non puo' verificare e' il punto dove un errore di
   registrazione diventa un salto in un indirizzo arbitrario. Chiamare
   b->read su un struct chardev interpretato male legge un puntatore a funzione
   dall'offset sbagliato.

   dev_blockdev e' anche il modo in cui kmain ottiene il disco per minixfs, e in
   cui rdsect e wrsect scelgono su quale dei due lavorare. Per NOME e non per
   indice, per la ragione scritta in shell.c: "rdsect hdb 7" si legge, "rdsect 1 7"
   bisogna ricordarselo — e su wrsect la divergenza sarebbe scrivere sul disco
   sbagliato. */
struct chardev  *dev_chardev (const char *name);
struct blockdev *dev_blockdev(const char *name);

/* Gli stessi due, ma a partire dalla VOCE invece che dal nome: rendono l'impl se
   e' della specie giusta, 0 altrimenti — e 0 anche per una voce nulla, cosi' si
   compongono con dev_get senza un controllo in mezzo.

   Esistono per chi ENUMERA. Chi cammina il registry ha gia' la voce in mano, e
   costringerlo a passare da dev_chardev(e->name) vorrebbe dire buttare via
   l'indice per ricercare lo stesso oggetto per nome — ma soprattutto e' cio' che
   spinge chi enumera a scriversi il filtro e il cast a mano:

       if (e->kind != DEV_BLOCK) continue;
       b = (const struct blockdev *)e->impl;

   che e' esattamente il corpo di dev_blockdev riscritto altrove. E' successo in
   shell_lsblk, ed e' il motivo per cui queste due funzioni sono state aggiunte
   dopo: il cast da void * e' l'operazione pericolosa del device layer, e ogni
   posto che lo esegue e' un posto che puo' sbagliarlo. Con queste, il controllo
   su kind vive in UN punto e i due lookup per nome si scrivono in termini loro.

   Leggere e->kind per MOSTRARE cos'e' un dispositivo resta legittimo — shell_devs
   lo fa per stampare 'c' o 'b'. Cio' che non deve accadere fuori da devio.c e'
   leggerlo per decidere come interpretare impl. */
struct chardev  *devio_chardev_of (const struct dev_entry *e);
struct blockdev *devio_blockdev_of(const struct dev_entry *e);

/* Riempie *in come VISTA A FILE del dispositivo e: type, ops, priv, size, major,
   minor. 0 se ci riesce, -1 se e e' nullo o se kind non e' uno dei due — e su -1
   NON tocca *in, che e' la convenzione di lookup e di create in vfs.h.

   NON scrive ino, e la ragione e' importante: quello e' affare di devfs, che lo
   scrive per ULTIMO. Nel suo pool ino == 0 significa "slot non inizializzato",
   quindi scriverlo prima del resto fa consegnare a un altro task un inode con ops
   ancora nullo. Il commento sta in devfs_lookup.

   E' l'UNICO posto che assegna le due vtable di inode_ops, che per questo restano
   static dentro devio.c: nessuno le vede da fuori, e non c'e' modo di raggiungere
   l'adapter se non passando da qui. Anche i test host ci passano, ed e' un
   vantaggio — esercitano il percorso vero.

   Per un dispositivo a blocchi size vale nsectors * SECTOR_SIZE. Il prodotto gira
   in uint32_t a 4 GiB, cioe' 8388608 settori, e LBA28 arriva a 128 GiB — quindi
   e' raggiungibile in principio, e sui nostri dischi da 2048 e 512 settori non si
   vede. Sistemarlo vuole una dimensione a 64 bit in struct inode, che e' un
   problema di struct stat in M14: e' annotato fra i debiti. */
int devio_fill_inode(const struct dev_entry *e, struct inode *in);

/* La maschera delle capacita' di e, oppure 0 per una voce invalida.

   Zero e' anche il valore di un dispositivo che non sa fare niente, che i due
   wrapper rifiutano — quindi in pratica non si presenta, e non serve un terzo
   valore per distinguere i due casi. */
int devio_caps(const struct dev_entry *e);

#endif
