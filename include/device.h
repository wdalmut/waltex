#ifndef WALTEX_DEVICE_H
#define WALTEX_DEVICE_H

#include "types.h"

/* Quanti dispositivi ci stanno, e quanto puo' essere lungo un nome. Statici
   come tutto il resto: l'allocatore arriva in M12, e non prima. */
#define MAX_DEVICES  16
#define DEV_NAME_MAX 16     /* NUL compreso: 15 caratteri utili */

/* Un dispositivo a caratteri: un nome, una coppia di numeri, e due operazioni.
   I dispositivi a blocchi arrivano in M10 con un'interfaccia propria, perche' la
   loro granularita' e' il settore e non il byte.

   name e' un ARRAY e non un const char *, e la differenza e' il punto: chi si
   iscrive puo' riempire questa struct sullo stack, e quando la sua funzione
   ritorna quella memoria non e' piu' sua. Con un puntatore il registro
   conserverebbe l'indirizzo di una stringa che appartiene a qualcun altro;
   l'array costringe device_register a copiare, ed e' la copia che rende il
   registro indipendente da chi lo popola.

   read e write ricevono il proprio struct device come primo argomento. In M8
   non serve a niente — gli adattatori lo ignorano — e serve a tutto in M10,
   quando il driver ATA iscrivera' due dischi con la STESSA funzione read e
   dovra' sapere quale dei due sta leggendo. Per quello esiste anche priv, che
   in M8 nessuno usa.

   Un puntatore a operazione NULLO significa "questo dispositivo non fa quella
   cosa", non "errore": console non si legge, kbd non si scrive. E' la stessa
   convenzione di exc_handlers[vec] == 0 in idt.c, ed e' cio' che permette a chi
   enumera di descrivere le capacita' senza un campo apposta. */
struct device {
    char     name[DEV_NAME_MAX];
    uint16_t major, minor;

    /* Ritornano quanti byte hanno trasferito DAVVERO, oppure -1 su errore.

       Per read, uno zero significa "adesso non c'e' niente", NON "fine del
       file", e la distinzione regge tutto M9: cat /dev/kbd fara' spin proprio
       su quello zero, e se volesse dire "finito" uscirebbe invece di aspettare
       che si digiti.

       Nessuna delle due deve bloccare: il blocking I/O manca per scelta, sta
       nello spec sotto "fuori scope", e il punto di decisione e' M9. */
    int (*read )(struct device *d, void *buf, uint32_t n);
    int (*write)(struct device *d, const void *buf, uint32_t n);

    void *priv;
};

/* Porta il registro a "nessun dispositivo iscritto".

   Va chiamata da kmain PRIMA di ogni *_init() dei driver, perche' sono i driver
   a iscriversi. Attenzione: il registro tiene un contatore static, che al boot
   vale zero perche' sta in .bss — quindi dimenticare questa chiamata non rompe
   niente OGGI, e rompe tutto il giorno che il registro guadagna un campo che non
   parte da zero. Chiamarla dopo i driver e' peggio: azzera il contatore e i
   dispositivi gia' iscritti diventano invisibili pur essendo nell'array. */
void device_init(void);

/* Copia il descrittore nel primo slot libero. Ritorna 0 se iscritto, -1 se
   rifiutato, e i motivi del rifiuto sono cinque:

     - il registro e' pieno;
     - name non contiene un NUL nei primi DEV_NAME_MAX byte. Attenzione al
       modo in cui questo caso si presenta: name e' un array da DEV_NAME_MAX,
       quindi un chiamante NON PUO' passare un nome di DEV_NAME_MAX caratteri
       terminato — non c'e' posto per il terminatore. Il caso reale e' quindi
       "nome non terminato", e va rilevato scandendo AL MASSIMO DEV_NAME_MAX
       byte. Una strlen normale su una stringa non terminata cammina fuori
       dall'array;
     - name e' vuoto: device_find("") non ha significato, e nel /dev di M9
       sarebbe un file senza nome;
     - un dispositivo con quel nome c'e' gia': la ricerca diventerebbe ambigua
       e vincerebbe il primo in silenzio;
     - read e write sono entrambi nulli: un dispositivo che non sa fare niente
       non e' utilizzabile, e iscriverlo nasconde un driver che ha dimenticato
       di riempire la struct.

   In tutti e cinque i casi si RIFIUTA, non si aggiusta. Troncare un nome troppo
   lungo farebbe collidere "console-primaria" e "console-secondaria", cioe'
   trasformerebbe un errore del driver in un bug di ricerca che si manifesta
   da un'altra parte.

   Non modifica *d: il const e' una promessa al chiamante. */
int device_register(const struct device *d);

/* Il dispositivo con quel nome, o 0 se non c'e'.

   La corrispondenza e' ESATTA: "cons" non trova "console". Il puntatore e' allo
   slot dentro il registro, non a una copia, cosi' chi lo riceve puo' chiamare
   d->write(d, ...) passando il d giusto. */
struct device *device_find(const char *name);

/* Il dispositivo con quella coppia di numeri, o 0 se non c'e'.

   In M8 non la chiama nessuno, e vale la pena saperlo. Il primo chiamante e' il
   VFS di M9: l'inode di un file di dispositivo memorizza major e minor e non il
   nome, esattamente come su Unix, quindi la chiave che il VFS avra' in mano
   sara' 13:0 e non "kbd". */
struct device *device_by_id(uint16_t major, uint16_t minor);

/* Quanti dispositivi sono iscritti. Il numero e' tenuto, non ricalcolato:
   contare scorrendo l'array introdurrebbe una seconda verita' che puo'
   divergere dalla prima. */
int device_count(void);

/* Lo slot i, oppure 0 se i e' negativo o >= device_count().

   Esiste per la stessa ragione di task_slot in M6a: l'array e' static dentro
   device.c, e serve un modo di enumerarlo da fuori senza renderlo globale.
   Il controllo sul negativo non e' pedanteria — devs[-1] legge i byte prima
   dell'array, che in .bss sono un'altra variabile. */
struct device *device_at(int i);

#endif
