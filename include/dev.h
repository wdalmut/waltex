#ifndef WALTEX_DEV_H
#define WALTEX_DEV_H

#include "types.h"

/* Quanti dispositivi ci stanno, e quanto puo' essere lungo un nome. Statici come
   tutto il resto: l'allocatore arriva in M12, e non prima. */
#define DEV_MAX      16
#define DEV_NAME_MAX 16     /* NUL compreso: 15 caratteri utili */

/* DEV_NONE = 0 e non DEV_CHAR = 0, ed e' la convenzione di INODE_NONE in vfs.h e
   di "nessun inode vale zero" di procfs: uno slot mai riempito non deve poter
   passare per un dispositivo a caratteri valido. */
enum dev_kind { DEV_NONE = 0, DEV_CHAR, DEV_BLOCK };

/* Una voce del registry: identita' e presenza, e NIENTE I/O. E' l'intero
   refactor di M11e in una frase.

   Finche' il registro conosceva read e write a byte — struct device di M8 — le
   due proprieta' "essere un dispositivo" e "avere una vista a byte" erano la
   stessa cosa. Un disco ha la prima e non la seconda: la sua granularita' e' il
   settore, e su un disco "ho letto 3 byte su 64" non e' un esito normale ma un
   guasto. Quindi non poteva iscriversi, quindi non esisteva come file, e non e'
   una dimenticanza — era strutturale. Sotto la stessa interfaccia uno dei due
   avrebbe dovuto mentire.

   Chi sa fabbricare la vista a byte e' devio.c, ed e' l'unico posto dove kind si
   apre in uno switch.

   name e' un ARRAY, e la ragione e' quella di M8: chi si iscrive riempie una
   struct e dev_register COPIA il nome, quindi il registry conserva una stringa
   che non appartiene a nessun altro.

   impl e' un PUNTATORE, e qui la convenzione di M8 SI SPEZZA IN DUE:

     name          copiato        come prima
     impl          RIFERITO       nuovo in M11e

   Quindi la struct puntata deve sopravvivere a chi la iscrive: static o globale,
   mai una locale di funzione. Fino a M11d era il contrario, e i tre driver a
   caratteri la riempivano sullo stack — vedi il commento in serial_init, che dice
   esattamente questo al rovescio.

   major e minor sono METADATI: nessun lookup li usa, e sono qui perche' il giorno
   che un inode minix dira' "sono il dispositivo 3:0", dev_by_id e' la funzione
   che risponde. Sono i numeri veri di Linux, verificati con ls -l /dev e non
   ricordati: costa zero e sta nella direzione del vincolo POSIX. */
struct dev_entry {
    char          name[DEV_NAME_MAX];
    enum dev_kind kind;
    uint16_t      major, minor;
    void         *impl;          /* struct chardev * | struct blockdev * */
};

/* Porta il registry a "nessun dispositivo iscritto".

   Va chiamata da kmain PRIMA di ogni *_init() dei driver, perche' sono i driver a
   iscriversi. Attenzione: il registry tiene un contatore static, che al boot vale
   zero perche' sta in .bss — quindi dimenticare questa chiamata non rompe niente
   OGGI, e rompe tutto il giorno che il registry guadagna un campo che non parte
   da zero. Chiamarla dopo i driver e' peggio: azzera il contatore e i
   dispositivi gia' iscritti diventano invisibili pur essendo nell'array. */
void dev_init(void);

/* Copia la voce nel primo slot libero. 0 se iscritta, -1 se rifiutata, e i
   motivi del rifiuto sono SEI:

     - il registry e' pieno;

     - kind non e' DEV_CHAR ne' DEV_BLOCK. DEV_NONE compreso: uno slot che non
       dichiara di che specie sia non si puo' servire, perche' devio non saprebbe
       come interpretare impl — e interpretarlo male e' un salto in un indirizzo
       arbitrario;

     - name non contiene un NUL nei primi DEV_NAME_MAX byte. Attenzione al modo
       in cui questo caso si presenta: name e' un array da DEV_NAME_MAX, quindi un
       chiamante NON PUO' passare un nome di DEV_NAME_MAX caratteri terminato —
       non c'e' posto per il terminatore. Il caso reale e' quindi "nome non
       terminato", e va rilevato scandendo AL MASSIMO DEV_NAME_MAX byte.

       QUESTO CONTROLLO ERA ROTTO IN M8, ed e' il bug che M11e chiude. C'era:

           int p = strpos(d->name, '\0');
           if (p > DEV_NAME_MAX) { return 1; }
           size_t lname = strlen(d->name);

       strpos ha un ramo esplicito "else if (a == '\0') r = -1" in memory.c,
       quindi cercando il terminatore ritorna SEMPRE -1: la guardia era codice
       morto, perche' -1 > 16 e' falso. E se fosse stata raggiungibile avrebbe
       ritornato 1, violando il contratto "0 oppure -1" dichiarato qui sopra.
       Cio' che proteggeva davvero era la strlen sotto, cioe' precisamente la
       scansione illimitata contro cui il commento metteva in guardia. Funzionava
       per accidente di layout — major stava subito dopo name — e in dev_entry al
       suo posto c'e' kind, che e' un int;

     - name e' vuoto: dev_lookup_index("") non ha significato, e in /dev sarebbe
       un file senza nome;

     - un dispositivo con quel nome c'e' gia': la ricerca diventerebbe ambigua e
       vincerebbe il primo in silenzio. Vale anche fra specie diverse — /dev ha un
       solo namespace, come su Unix, dove non possono coesistere un hda a
       caratteri e un hda a blocchi;

     - la coppia (major, minor) e' gia' presa. Si prova con dev_by_id, cosi' la
       scansione per coppia esiste una volta sola: la ricerca per coppia e' ben
       definita PERCHE' la coppia e' unica per costruzione, e la coppia e' unica
       PERCHE' qualcuno cerca per coppia.

   In tutti e sei si RIFIUTA, non si aggiusta. Troncare un nome troppo lungo
   farebbe collidere "console-primaria" e "console-secondaria", cioe'
   trasformerebbe un errore del driver in un bug di ricerca che si manifesta da
   un'altra parte.

   NON controlla i puntatori a operazione, perche' non li conosce — e perdere quel
   controllo e' un GUADAGNO, non un costo: nei due wrapper di devio.h diventa
   asimmetrico come deve essere. Un controllo condiviso distingue solo "almeno
   uno", e non potrebbe esprimere che un disco da cui non si legge non ha senso
   mentre un disco su cui non si scrive e' un read-only legittimo.

   Non modifica *e: il const e' una promessa al chiamante. */
int dev_register(const struct dev_entry *e);

/* L'INDICE della voce con quel nome, oppure -1.

   Un indice e non un puntatore, e non e' un dettaglio: devfs tiene un pool di
   inode indicizzato 1:1 col registry, e questo numero e' cio' che gli sceglie lo
   slot. Chi vuole la voce chiama dev_get sull'indice.

   La corrispondenza e' ESATTA: "cons" non trova "console". In /dev sarebbe la
   differenza fra aprire un file e aprirne un altro. */
int dev_lookup_index(const char *name);

/* La voce i, oppure 0 se i e' negativo o >= dev_count().

   Esiste per la stessa ragione di task_slot in M6a: l'array e' static dentro
   dev.c, e serve un modo di enumerarlo da fuori senza renderlo globale. Il
   controllo sul negativo non e' pedanteria — entries[-1] legge i byte prima
   dell'array, che in .bss sono un'altra variabile.

   Ritorna const, cosa che chardev_find non poteva: allora il puntatore doveva
   essere mutabile perche' chi lo riceveva chiamava d->write(d, ...). Ora
   l'oggetto mutabile e' impl, che la voce RIFERISCE e non possiede, quindi il
   const sulla voce diventa possibile — ed e' la promessa che il registry non si
   modifica dall'esterno. */
const struct dev_entry *dev_get(int i);

/* La voce con quella coppia di numeri, oppure 0.

   E' anche l'implementazione del sesto rifiuto di dev_register, e i due si
   giustificano a vicenda: vedi il commento la'.

   In M8 questa funzione non la chiamava nessuno, e l'header prometteva che il
   primo chiamante sarebbe stato il VFS di M9. Non e' successo, in quattro
   milestone — di solito e' il momento di tagliare. Qui no, perche' il nuovo
   rifiuto ha bisogno esattamente di questa scansione. Il chiamante promesso
   arrivera' con un nodo di dispositivo su minix, che memorizza major e minor e
   non il nome: la chiave sara' 13:64 e non "kbd". */
const struct dev_entry *dev_by_id(uint16_t major, uint16_t minor);

/* Quanti dispositivi sono iscritti. Il numero e' TENUTO, non ricalcolato:
   contare scorrendo l'array introdurrebbe una seconda verita' che puo' divergere
   dalla prima. */
int dev_count(void);

#endif
