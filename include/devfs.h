#ifndef WALTEX_DEVFS_H
#define WALTEX_DEVFS_H

#include "types.h"
#include "vfs.h"

/* Il filesystem concreto di M9b: tre tipi di inode e nient'altro.

     /            directory con UNA voce: "dev"
     /dev         directory le cui voci SONO il registro dei dispositivi di M8
     /dev/<nome>  foglia: le sue read e write chiamano quelle di struct device

   Nessuno dei tre memorizza le proprie voci: le GENERA quando gliele si chiede.
   E' la proprieta' che il VFS ha comprato scegliendo lookup invece di un albero
   di puntatori, e qui si vede a cosa serviva — le voci di /dev sono il registro,
   e duplicarle sarebbe tenere due verita' che possono divergere. */

/* Costruisce gli inode leggendo il registro dei dispositivi.

   VINCOLO D'ORDINE, e ha due lati:

     - va chiamata DOPO tutte le *_init() dei driver, perche' legge il registro
       che loro riempiono. Chiamandola prima, device_count() darebbe 0, /dev
       sarebbe vuota, e non ci sarebbe nessun errore da nessuna parte;
     - e PRIMA di vfs_init, che ha bisogno della radice.

   E' l'opposto esatto di device_init(), che deve venire prima di tutti. I due
   vincoli insieme incorniciano le inizializzazioni dei driver:

       device_init();          <- prima di tutti: i driver ci si iscrivono
       vga_init(); ... keyboard_init();
       devfs_init();           <- dopo tutti: legge cio' che hanno iscritto
       vfs_init(devfs_root());
*/
void devfs_init(void);

/* L'inode della radice, da passare a vfs_init.

   Ritorna 0 se devfs_init non e' ancora stata chiamata, e il controllo non e'
   decorativo: con una radice fatta di zeri il VFS avrebbe type INODE_NONE e ops
   nullo, quindi OGNI resolve fallirebbe senza dire perche'. Restituendo 0
   l'ordine sbagliato si vede al primo controllo invece che al primo cat. */
struct inode *devfs_root(void);

/* L'inode di /dev: la directory le cui voci SONO il registro dei dispositivi.

   Serve da M11a, quando la radice passa a minix e devfs smette di essere il
   filesystem principale per diventare un innesto. Cio' che si innesta sotto il
   nome "dev" e' questa directory, NON devfs_root(): la radice di devfs ha una
   sola voce, che si chiama "dev", quindi innestando quella si otterrebbe un
   /dev/dev/kbd invece di /dev/kbd.

   Da M11a devfs_root() non lo chiama piu' nessuno nel kernel — resta per i
   self-check e per il ripiego di kmain quando il disco non c'e'.

   Ritorna 0 se devfs_init non e' ancora stata chiamata, come devfs_root(). */
struct inode *devfs_devdir(void);

#endif
