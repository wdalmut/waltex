# Il polimorfismo di waltex: device, VFS, devfs

Documento di ripasso. Non aggiunge decisioni: ricostruisce, un livello per
volta, come `cat /dev/kbd` riesce a stampare quello che digiti senza che una
sola riga di `cat` nomini la tastiera.

Le tre milestone in gioco:

| | cosa aggiunge | il tipo che introduce |
|---|---|---|
| **M8** | i driver si iscrivono a un registro | `struct device` |
| **M9a** | path, inode, descrittori | `struct inode_ops` |
| **M9b** | un filesystem concreto sopra il registro | i tre insiemi di `devfs.c` |

---

## 1. Il meccanismo, in una frase

Non c'è nessun linguaggio a oggetti e nessuna vtable generata dal compilatore.
C'è **un puntatore a funzione dentro una struct**, e la tabella la scrivi a mano:

```c
static const struct inode_ops ops_chardev = { chardev_read, chardev_write, 0, 0 };
```

Questa riga *è* la vtable. È `const`, quindi sta in `.rodata`; esiste una volta
sola per tipo, non una per oggetto; e ogni inode che si comporta come un
dispositivo a caratteri ne tiene l'indirizzo.

Chi chiama non sa cosa c'è dall'altra parte. Sa solo che c'è qualcosa che accetta
gli argomenti giusti:

```c
rbytes = f->inode->ops->read(f->inode, f->off, buf, n);
```

Quella riga sola — [vfs.c:269](../kernel/vfs.c#L269) — è tutto il polimorfismo del
VFS. Quattro dereferenze e una chiamata indiretta.

---

## 2. I due strati, affiancati

Sono la stessa idea a due altezze diverse, e conviene vederli uno sotto l'altro.

```c
/* M8 — include/device.h */
struct device {
    char     name[DEV_NAME_MAX];
    uint16_t major, minor;
    int (*read )(struct device *d, void *buf, uint32_t n);
    int (*write)(struct device *d, const void *buf, uint32_t n);
    void *priv;
};

/* M9a — include/vfs.h */
struct inode_ops {
    int (*read   )(struct inode *ino, uint32_t off, void *buf, uint32_t n);
    int (*write  )(struct inode *ino, uint32_t off, const void *buf, uint32_t n);
    int (*lookup )(struct inode *dir, const char *name, struct inode **out);
    int (*readdir)(struct inode *dir, int idx, char *name, uint32_t *ino_out);
};
```

Le differenze non sono cosmetiche:

| | `struct device` | `struct inode_ops` |
|---|---|---|
| rende intercambiabili | tre **driver** | interi **filesystem** |
| operazioni | 2 | 4 — servono anche i nomi e l'albero |
| tabella e dati | **nella stessa struct** | **separati**: `inode_ops` è condivisa, `inode` è per oggetto |
| posizione | non esiste: un device non ha un byte numero 12 | **offset esplicito**, e non è nella struct |
| istanze | una struct per dispositivo, copiata nel registro | una `inode_ops` per *tipo*, un `inode` per oggetto |

La riga più importante è la terza. In M8 puntatori e dati stanno insieme, perché
i dispositivi sono pochi e ognuno è diverso. In M9a sono separati, perché un
filesystem minix avrà migliaia di inode e sarebbe assurdo copiare quattro
puntatori in ognuno: tutti gli inode di uno stesso tipo condividono la tabella.

La quarta è la decisione di interfaccia che regge tutto M9: **`read` e `write`
ricevono l'offset e non lo tengono**. La posizione vive nella tabella dei file
aperti, quindi due `open` sullo stesso path danno due posizioni e un solo inode.
Se stesse nell'inode, due letture indipendenti se la ruberebbero.

---

## 3. Il percorso completo di `cat /dev/kbd`

Sette livelli. A ogni riga in grassetto la chiamata è **indiretta**: chi la fa
non sa dove va a finire.

```text
shell_exec("cat /dev/kbd")            kernel/shell.c
 └─ table[i].fn(argc, argv)           ← INDIRETTA #1: quale comando
     shell_cat
      ├─ vfs_resolve("/dev/kbd")      kernel/vfs.c
      │   ├─ ops->lookup("dev")       ← INDIRETTA #2: quale directory
      │   │    root_lookup            kernel/devfs.c
      │   └─ ops->lookup("kbd")       ← ancora #2, ma un'ALTRA funzione
      │        dev_lookup             kernel/devfs.c
      ├─ vfs_open("/dev/kbd")         → un fd
      └─ vfs_read(fd, buf, 64)
          └─ ops->read(ino, off, ..)  ← INDIRETTA #3: quale filesystem
              chardev_read            kernel/devfs.c
               └─ d->read(d, buf, n)  ← INDIRETTA #4: quale driver
                   kbd_dev_read       kernel/keyboard.c
                    └─ keyboard_getchar
                        └─ ring_get   kernel/ring.c
                            ← il gestore dell'IRQ 1 ci ha scritto
```

Le due chiamate marcate `#2` sono lo stesso codice sorgente
([vfs.c:120](../kernel/vfs.c#L120)) che raggiunge **due funzioni diverse** in due
giri consecutivi del ciclo, perché fra un giro e l'altro `current` si è spostato.
È il punto in cui il polimorfismo si vede meglio: la funzione chiamata cambia
senza che cambi la riga che la chiama.

### Dove cadono i nomi

| stringa | compare in | non compare in |
|---|---|---|
| `"dev"`, `"kbd"` | `devfs.c` | `vfs.c` |
| `fd`, `files[]`, `off` | `vfs.c` | `devfs.c` |
| porta 0x60, scancode | `keyboard.c` | ovunque sopra |

Ogni volta che la freccia attraversa un confine, passa per un puntatore a
funzione. È il confine.

---

## 4. Lo stesso trucco, cinque volte nel progetto

Non è una tecnica del secondo blocco: c'era già.

| dove | il puntatore | a cosa serve |
|---|---|---|
| `idt.c` | `exc_handlers[vec]`, `irq_handlers[irq]` — [idt.c:52](../kernel/idt.c#L52) | un gestore per vettore, installato a runtime |
| `kprintf.c` | `kvprintf(void (*putc)(char), ...)` | la grammatica del formato separata da **dove finiscono i byte** |
| `lineedit.c` | `le->echo` | il modulo non chiama `kprintf`: così il test può verificare *cosa* è stato echeggiato |
| `shell.c` | `table[i].fn` — [shell.c:528](../kernel/shell.c#L528) | aggiungere un comando è una riga in un array |
| `device.c` / `vfs.c` | `d->read`, `ops->read` | i due strati di questo documento |

I primi tre sono nati per la **testabilità**, non per l'estensibilità — e non è
un caso: un modulo che non può essere provato in isolamento e un modulo che non
può essere esteso hanno la stessa malattia, dipendono da qualcosa che non
dovrebbero nominare.

`vfs_init(struct inode *root)` è lo stesso espediente in forma di dato invece che
di funzione: il VFS **riceve** la radice. Nel kernel gliela passa `devfs`, nei
test un albero finto di sei nodi. Senza quella scelta, provare la risoluzione di
un path richiederebbe un filesystem, cioè un disco.

---

## 5. Le tre convenzioni, condivise dai due strati

Sono ciò che rende i due livelli componibili. Violarne una a un piano rompe il
piano sopra.

### Un puntatore nullo significa «non supportata», non «errore»

```c
struct device dev = { .name = "console", .major = 5, .minor = 1,
                      .write = vga_dev_write };   /* .read resta 0 */
```

[vga.c:41](../kernel/vga.c#L41). `console` non si legge, `kbd` non si scrive. È la
stessa convenzione di `exc_handlers[vec] == 0`, ed è ciò che permette a `devs` di
stampare `-w` e `r-` **senza un campo di capacità**: la capacità *è* la non
nullità del puntatore, quindi non può divergere dalla realtà.

Chi chiama deve controllare prima di chiamare — [vfs.c](../kernel/vfs.c) lo fa in
`vfs_read`, `vfs_write` e `vfs_readdir`.

### Il ritorno è quanti byte davvero, non quanti richiesti

`read` può restituire meno di `n` senza che sia un errore. È il motivo per cui
`vfs_read` avanza la posizione di `rbytes` e non di `n`.

### Zero significa «adesso niente», **non** fine del file

Questa regge M9b intera. Su un file regolare lo zero significa che la posizione
ha raggiunto `size`; su un dispositivo significa che il buffer è vuoto in questo
istante. Sono due cose diverse, e **il valore di ritorno non le distingue**: le
distingue chi chiama, guardando il tipo dell'inode.

Per questo `shell_cat` legge il tipo *prima* di aprire:

```text
INODE_FILE      si smette quando read torna 0
INODE_CHARDEV   non finisce mai: si smette al primo '\n'
```

Un `cat` che aspettasse lo zero su `/dev/kbd` resterebbe piantato per sempre —
non ci sono segnali, quindi nemmeno un Ctrl-C.

---

## 6. `priv`, ovvero «quale dei due dischi»

Ogni operazione riceve come primo argomento **l'oggetto su cui sta lavorando**:
`struct device *d`, `struct inode *ino`. Sembra ridondante, e in metà dei casi lo
è davvero.

Serve esattamente quando **una funzione serve più di un oggetto**:

| funzione | quanti oggetti serve | il primo argomento |
|---|---|---|
| `vga_dev_write` | uno solo: `console` | ignorato, `(void)d` |
| `root_lookup` | una sola directory: `/` | ignorato, `(void)dir` |
| `chardev_read` | **tre** dispositivi | indispensabile: `ino->priv` |
| `ata_read` (M10) | **due** dischi, stessa funzione | indispensabile |
| `minix_lookup` (M11) | **migliaia** di directory | indispensabile: `dir->ino` |

`chardev_read` è il primo caso del progetto in cui serve davvero, ed è tre righe:

```c
struct device *d = (struct device *)ino->priv;

if (d == 0 || d->read == 0)
    return -1;

return d->read(d, buf, n);
```

[devfs.c:85](../kernel/devfs.c#L85). L'intera cerniera fra i due strati è questa.
Sopra c'è un inode, sotto un dispositivo, e `priv` è il filo che li lega — messo
lì da `devfs_init` con una riga, `ino_devices[i].priv = d`.

Quel filo **non è un fatto, è un patto**: niente nei tipi lo impone. Leggere il
device da `ino_devices[i].priv` invece di richiederlo al registro per indice
toglie il problema alla radice, perché nome e inode vengono dallo stesso oggetto
invece che da due array tenuti allineati a mano.

---

## 7. Perché due strati e non uno

Domanda legittima: `devfs` sembra un passacarte. Perché `vfs_read` non chiama
direttamente `d->read`?

Perché in quel caso `/` potrebbe contenere **solo dispositivi**. Il VFS
conoscerebbe `struct device`, e allora:

- non ci sarebbero directory che non siano `/dev`, perché un dispositivo non ha
  figli;
- non ci sarebbe posto per `size`, per gli offset, per i file regolari;
- **minix non potrebbe esistere**: in M11 servirebbe un secondo caso dentro
  `vfs_read`, e da lì in poi ogni filesystem nuovo rimetterebbe in discussione
  `vfs.c`.

Con il taglio dov'è, `vfs.c` **è finito**. Da qui a M17 non si tocca:
`minixfs.c` riempirà le stesse quattro caselle, e lo `shell_cat` scritto in M9b
leggerà file veri da un disco senza una riga di modifica. Quella è la verifica
che l'astrazione era nel punto giusto — e non si può fare adesso, si potrà fare
in M11.

Vale anche al contrario: **un filesystem si inventa la propria forma.** La radice
e `/dev` non vengono dal registro, non sono dispositivi, e nell'hardware non
esiste niente che si chiami `/`. Le scrive `devfs_init` a mano. minix leggerà la
forma dal disco. Il VFS non distinguerà i due casi.

---

## 8. L'albero non è nei dati

Un dettaglio che passa inosservato e che decide tutto il resto: **`struct inode`
non ha puntatori ai figli.**

```c
struct inode {
    uint32_t         ino;
    enum inode_type  type;
    uint32_t         size;
    uint16_t         major, minor;
    const struct inode_ops *ops;
    void            *priv;
};
```

Nessun `parent`, nessun `children[]`. L'albero **è nella funzione `lookup`**: ogni
directory sa rispondere «dammi il figlio che si chiama così», e camminare un path
è una catena di domande.

È ciò che permette a `devfs` di generare le voci di `/dev` dal registro invece di
duplicarle — due copie della stessa verità possono divergere — e a minix di
leggere i figli dal disco **quando servono**, senza tenere il filesystem in RAM.

E la coppia `lookup`/`readdir` è la conseguenza:

```text
lookup    nome  → inode      "dammi il figlio che si chiama dev"
readdir   idx   → nome       "dammi il nome del figlio numero 0"
```

`ls` non può usare `lookup`: per chiedere un nome bisognerebbe già conoscerlo.
`readdir` non restituisce un array perché non c'è allocazione — restituisce una
voce per volta, e il chiamante conta.

Da cui il ritorno a **tre** valori, unico nel VFS: `1` c'è, `0` finito, `-1` la
domanda non aveva senso. Collassando `0` e `-1`, chi enumera non distingue
«directory finita» da «questo fd non è una directory».

---

## 9. Come si rompe

I guasti veri di M8 e M9b, tutti dello stesso genere: **un valore di ritorno che
viola la convenzione dello strato**. Nessuno di loro produce un errore di
compilazione, e quasi nessuno produce un sintomo vicino alla causa.

| guasto | conseguenza |
|---|---|
| `lookup` ritorna `1` invece di `-1` quando non trova | `vfs_resolve` controlla `< 0`, quindi crede di aver trovato e cammina su un puntatore mai inizializzato: comportamento diverso a ogni boot |
| `chardev_read` ritorna `1` con `d->read == 0` | lì `1` significa «un byte trasferito»: `cat` avanza l'offset e stampa un buffer che nessuno ha riempito |
| `kbd_dev_read` ignora `n` | scrive fino a 127 byte nel buffer del chiamante |
| `readdir` non ha il ramo di fine elenco | il ciclo di `ls` non termina |
| `readdir` e `lookup` in disaccordo sui nomi | `ls` mostra un nome che `cat` non riesce ad aprire |
| indici di `ino_devices` e del registro sfasati | `dev_lookup("kbd")` restituisce un inode di tipo directory |

Il controllo che li prende quasi tutti è **negativo**:
`vfs_resolve("/dev/nonesiste")` deve fallire. Nessun controllo positivo può
vedere un ritorno sbagliato sul ramo dell'insuccesso — è il motivo per cui quel
self-check esiste, ed è la lezione che in M14 diventerà la regola: i test veri
del ring 3 sono tutti negativi.

---

## 10. Cosa succede a questi due strati, da qui in poi

| milestone | cosa si iscrive dove | il pezzo nuovo |
|---|---|---|
| **M10** ATA | due dischi, **stessa** `read` | `priv` diventa indispensabile per la prima volta |
| **M11** minix | un secondo `inode_ops` accanto a `devfs` | `vfs.c` non cambia — è la verifica dell'astrazione |
| **M14** ring 3 | `vfs_open/read/write/close/lseek` diventano syscall | le firme **non cambiano**: era quello il punto |
| **M17** newlib | ~15 stub | sono precisamente la forma del VFS di M9 |

La riga di M14 è la scommessa che il secondo blocco ha fatto scrivendo la shell
per prima: il VFS è stato costruito e usato in ring 0 *prima* che esistesse un
confine di privilegi, e in M14 il chiamante passa dall'altra parte del confine
**senza che l'interfaccia cambi**. Se dovesse cambiare, vorrà dire che il confine
non era nel punto giusto.

---

## Riferimenti nel codice

```text
include/device.h    struct device, le tre convenzioni di M8, i vincoli d'ordine
include/vfs.h       inode_ops, i tre livelli, perche' l'offset e' esplicito
include/devfs.h     il vincolo d'ordine a due lati di devfs_init
kernel/vfs.c:120    la lookup indiretta, dentro il ciclo di vfs_resolve
kernel/vfs.c:269    ops->read: tutto il polimorfismo del VFS in una riga
kernel/devfs.c:85   chardev_read: la cerniera fra i due strati
kernel/vga.c:41     un driver che si iscrive, con .read lasciata a zero
```

Letture di accompagnamento: **xv6** del MIT è il riferimento migliore per il VFS
a tre livelli, perché fa le stesse scelte spiegate meglio. Linux 0.01 `fs/`
implementa esattamente il minix v1 di M11.
