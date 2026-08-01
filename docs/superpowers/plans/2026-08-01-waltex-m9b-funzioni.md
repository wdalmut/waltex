# M9b — Cosa deve fare ogni funzione

Companion di `2026-08-01-waltex-m9b-devfs.md`. Sei voci per scheda, come in M8 e
M9a: **compito**, **ritorna** (ogni caso), **non deve**, **chi la chiama**,
**come si sbaglia**, **test**.

## Indice

**`kernel/devfs.c`** — il filesystem concreto

1. [`devfs_init`](#1-devfs_init)
2. [`devfs_root`](#2-devfs_root)
3. [`root_lookup`](#3-root_lookup) e [`root_readdir`](#4-root_readdir)
4. [`dev_lookup`](#5-dev_lookup) e [`dev_readdir`](#6-dev_readdir)
5. [`chardev_read`](#7-chardev_read) e [`chardev_write`](#8-chardev_write)

**`kernel/shell.c`**

6. [`shell_ls`](#9-shell_ls)
7. [`shell_cat`](#10-shell_cat)

---

## Lo stato di devfs

```text
static struct inode ino_root;                 /* "/"        ino 1 */
static struct inode ino_dev;                  /* "/dev"     ino 2 */
static struct inode ino_devices[MAX_DEVICES]; /* uno per dispositivo, ino 3+ */
static int pronto;                            /* devfs_init e' stata chiamata? */
```

`ino_devices[i]` corrisponde a `device_at(i)`: **stesso indice**. È la
corrispondenza che tiene insieme le due tabelle, ed è il motivo per cui
`dev_lookup` cerca con `device_at` e non con `device_find` — le serve l'indice,
non il dispositivo.

Costo: `(16 + 2) × 24` = **432 byte** di `.bss`.

---

## 1. `devfs_init`

```c
void devfs_init(void);
```

**Compito.** Riempire i tre tipi di inode leggendo il registro dei dispositivi.

```text
ino_root:   ino=1, type=INODE_DIR, ops=&ops_root, size=0
ino_dev:    ino=2, type=INODE_DIR, ops=&ops_dev,  size=0

per ogni i da 0 a device_count()-1:
    d = device_at(i)
    ino_devices[i].ino   = 3 + i
    ino_devices[i].type  = INODE_CHARDEV
    ino_devices[i].ops   = &ops_chardev
    ino_devices[i].priv  = d          ← il ponte: l'inode ricorda quale dispositivo e'
    ino_devices[i].major = d->major
    ino_devices[i].minor = d->minor
    ino_devices[i].size  = 0          ← un dispositivo non ha dimensione

pronto = 1
```

**Ritorna.** Niente.

**Non deve.** Chiamare `vfs_init` — è `kmain` a collegare i due, e tenerli
separati è ciò che permette ai test di M9a di usare un albero finto.

**Chi la chiama.** `kmain`, **dopo** tutte le `*_init()` dei driver e **prima**
di `vfs_init`. I due vincoli insieme incorniciano le inizializzazioni dei driver:
`device_init` prima di tutti, `devfs_init` dopo tutti.

**Come si sbaglia.**

- **chiamarla prima che i driver si iscrivano**: `device_count()` darebbe 0,
  `/dev` sarebbe vuota, e **nessun errore** da nessuna parte. Il self-check sul
  numero di voci esiste per questo;
- **copiare il `struct device` invece del puntatore.** In `priv` va l'indirizzo:
  una copia si sgancerebbe dal registro, e in M10 quando un disco cambia stato
  l'inode mostrerebbe il vecchio;
- **dimenticare `size = 0`.** Gli inode sono `static`, quindi partono già a zero
  — ma scriverlo dice al lettore che è una scelta e non un residuo.

**Test.** I dieci self-check, che girano dopo il boot vero.

---

## 2. `devfs_root`

```c
struct inode *devfs_root(void);
```

**Compito.** Restituire `&ino_root`, oppure `0` se `devfs_init` non è ancora
stata chiamata.

**Ritorna.** Il puntatore, o `0`.

**Perché il controllo.** `vfs_init(devfs_root())` con `devfs_init` dimenticata
darebbe al VFS una radice fatta di zeri: `type` sarebbe `INODE_NONE` e `ops`
nullo, quindi ogni `resolve` fallirebbe **senza dire perché**. Restituendo `0`
l'errore si vede al primo controllo invece che al primo `cat`.

**Chi la chiama.** `kmain`, una volta.

**Come si sbaglia.** Restituendo `&ino_root` incondizionatamente. Funziona finché
l'ordine in `kmain` è giusto, e smette di funzionare in silenzio il giorno che
qualcuno lo cambia.

---

## 3. `root_lookup`

```c
static int root_lookup(struct inode *dir, const char *name, struct inode **out);
```

**Compito.** La radice ha **una sola voce**: `dev`. Se il nome è quello, `*out =
&ino_dev` e ritorna 0; altrimenti −1.

**Ritorna.** `0` con `*out` scritto, oppure `-1` senza toccarlo.

**Non deve.** Cercare da nessuna parte: la radice non ha una tabella. Un
confronto con `strcmp` e basta.

**Chi la chiama.** `vfs_resolve`, sul primo componente di ogni path.

**Come si sbaglia.** Dimenticando `(void)dir` — la firma lo impone e `-Wextra`
protesta. E scrivendo `*out` prima di aver confrontato il nome.

**Test.** `vfs_resolve("/dev")` riesce, `vfs_resolve("/qualcosa")` no.

---

## 4. `root_readdir`

```c
static int root_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);
```

**Compito.** `idx == 0` → il nome è `"dev"`, `*ino_out = 2`, ritorna 1. Qualunque
altro indice → 0.

**Ritorna.** `1` se la voce esiste, `0` se l'indice è oltre. Mai `-1`: la radice
ha sempre un `readdir` valido.

**Non deve** dimenticare il terminatore in `name`.

**Chi la chiama.** `vfs_readdir`, e da lì `ls /`.

**Come si sbaglia.** Restituendo 1 per ogni indice: `ls /` non finirebbe mai.

---

## 5. `dev_lookup`

```c
static int dev_lookup(struct inode *dir, const char *name, struct inode **out);
```

**Compito.** Scorrere il registro cercando un dispositivo con quel nome, e
restituire l'inode **corrispondente per indice**.

```text
per ogni i da 0 a device_count()-1:
    se strcmp(device_at(i)->name, name) == 0:
        *out = &ino_devices[i]
        ritorna 0
ritorna -1
```

**Ritorna.** `0` con `*out`, oppure `-1` senza toccarlo.

**Perché `device_at` e non `device_find`.** Perché serve l'**indice**:
`ino_devices[i]` è l'inode di `device_at(i)`, e `device_find` restituisce il
dispositivo senza dire dov'era. Sono due funzioni che fanno una ricerca simile
con esiti diversi, ed è la volta in cui serve l'altra — se ti sembra una
duplicazione, guarda cosa restituiscono.

**Non deve** costruire l'inode al volo: `*out` è un puntatore, e deve puntare a
qualcosa che sopravvive alla chiamata. Gli inode li ha già fatti `devfs_init`.

**Chi la chiama.** `vfs_resolve`, sul secondo componente di `/dev/<nome>`.

**Come si sbaglia.**

- **restituire l'indirizzo di una variabile locale.** Funzionerebbe quasi
  sempre — il chiamante la usa subito — e sarebbe rotto in modo intermittente;
- **usare `device_find` e poi cercare l'indice** confrontando i puntatori: due
  scansioni al posto di una, e la seconda può fallire senza motivo.

**Test.** `vfs_resolve("/dev/kbd")` e `/dev/console` riescono,
`/dev/nonesiste` no.

---

## 6. `dev_readdir`

```c
static int dev_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);
```

**Compito.** La voce numero `idx` è `device_at(idx)`: copiane il nome, e metti in
`*ino_out` il numero di inode di `ino_devices[idx]`.

**Ritorna.** `1` se `idx < device_count()`, `0` altrimenti.

**Il nome va copiato limitato.** Viene da `device_at(i)->name`, che è un array da
`DEV_NAME_MAX` (16), mentre `name` ne accetta `VFS_NAME_MAX + 1` (15). **I due
limiti sono diversi**, e un nome di dispositivo di 15 caratteri non ci starebbe.
Oggi non capita — i tre nomi sono corti — ma la copia va comunque limitata a
`VFS_NAME_MAX`, altrimenti è un buffer overflow che aspetta un driver con un nome
lungo.

Vale la pena notarlo: è **la stessa geometria** di `device_register` in M8, dove
un nome più lungo del posto disponibile va rifiutato e non troncato. Qui però il
nome esiste già ed è valido nel suo mondo: è il *destinatario* a essere più
piccolo. Troncare è l'unica opzione, e va fatta con il terminatore al posto
giusto.

**Chi la chiama.** `vfs_readdir`, e da lì `ls /dev`.

**Test.** Il self-check che conta tre voci in `/dev`, e `tests/shell.sh` che
cerca i tre nomi nell'output di `ls /dev`.

---

## 7. `chardev_read`

```c
static int chardev_read(struct inode *ino, uint32_t off, void *buf, uint32_t n);
```

**Compito.** Girare la chiamata al dispositivo che sta in `priv`.

```text
d = (struct device *)ino->priv
se d == 0 oppure d->read == 0  →  -1
ritorna d->read(d, buf, n)
```

**Ritorna.** Quello che ritorna il dispositivo: byte trasferiti, `0` per «adesso
niente», `-1` per errore. Non lo reinterpreta.

**Scarta l'offset**, ed è la cosa da vedere: `(void)off;`. Una tastiera **non ha
un byte numero 12**. Il VFS lo passa comunque perché la firma è unica per tutti i
tipi di inode, e il dispositivo lo ignora deliberatamente.

È la prima volta nel progetto che un `inode_ops->read` scarta legittimamente il
suo secondo argomento, e il `(void)off` esplicito serve a dire al lettore che è
una scelta e non una dimenticanza.

**Non deve** far avanzare niente: la posizione la gestisce `vfs_read`, e per un
dispositivo semplicemente non la guarda nessuno.

**Chi la chiama.** `vfs_read`, attraverso `ops`. E dietro di lei ci sono ancora
due livelli: `d->read` è `kbd_dev_read` di M8, che chiama `keyboard_getchar` di
M5, che fa `ring_pop`.

**Come si sbaglia.**

- **controllare `d->read` dopo averlo chiamato**;
- **usare `off` per indicizzare il buffer** invece di ignorarlo.

**Test.** Il self-check di fine catena: `vfs_open("/dev/kbd")` più `vfs_read` a
ring vuoto dà 0 e non tocca il buffer di destinazione. Attraversa tutti e cinque
i livelli.

---

## 8. `chardev_write`

```c
static int chardev_write(struct inode *ino, uint32_t off, const void *buf, uint32_t n);
```

Identica a `chardev_read` nell'altra direzione, con `d->write`. Stesso `(void)off`,
stesso controllo sul puntatore nullo.

**Chi la chiama.** `vfs_write`. In M9b nessun comando della shell la usa — `cat`
legge e basta — ma è il percorso che in **M15** un processo utente userà per
`write(1, "ciao", 4)`, con `/dev/console` dietro il descrittore 1.

---

## 9. `shell_ls`

```c
static void shell_ls(int argc, char **argv);
```

**Compito.** Con un argomento, elencare quel path; senza, `/`.

```text
waltex> ls
  2 dev
waltex> ls /dev
  3 console
  4 ttyS0
  5 kbd
waltex> ls /dev/kbd
  5 kbd    chardev 13:64
```

**Due forme, come `devs`.** Su una directory enumera con `vfs_open` +
`vfs_readdir`; su qualunque altra cosa mostra quella sola voce con il suo tipo —
che è ciò che fa anche `ls` vero quando gli si passa un file.

Per sapere il tipo serve `vfs_resolve`, perché **da un descrittore non si risale
all'inode**: non c'è `fstat`. È il chiamante che in M9a mancava.

**Non deve** usare `read` su una directory: dà −1 per contratto, e in M11
darebbe byte grezzi di strutture minix.

**Il ciclo si ferma quando `vfs_readdir` dà 0**, non `-1`. I due valori sono
distinti apposta: `0` è «le voci sono finite», `-1` è «la domanda non aveva
senso». Un ciclo che si ferma su entrambi sembra funzionare finché non gli passi
un file.

**Come si sbaglia.** Dimenticando `vfs_close` — dopo otto `ls` i descrittori del
task sono finiti, e il sintomo arriva molto dopo la causa.

**Test.** `tests/shell.sh` cerca i tre nomi nell'output di `ls /dev`.

---

## 10. `shell_cat`

```c
static void shell_cat(int argc, char **argv);
```

La funzione che rende vera la frase «tutto è un file».

**Compito.** Aprire il path, leggere a blocchi, stampare. Ma **quando smettere
dipende dal tipo**, e non per convenzione: per natura.

| tipo | come finisce |
|---|---|
| `INODE_FILE` | `read` dà 0: la posizione ha raggiunto `size` |
| `INODE_CHARDEV` | non finisce mai — si smette al primo `'\n'` |
| `INODE_DIR` | errore: «è una directory» |

Su un dispositivo il valore di ritorno **non basta**, perché `0` significa
«adesso niente» e non «finito». Una tastiera non ha una fine, e `cat` che
aspettasse uno zero resterebbe piantato per sempre — senza modo di uscire, perché
non ci sono segnali e quindi nemmeno Ctrl-C.

**Serve quindi `vfs_resolve` prima di `vfs_open`**, per guardare il tipo.

**Il buffer è locale e piccolo** — 64 byte bastano — perché non c'è allocazione e
perché leggere a blocchi è ciò che fa `cat` vero.

**Chi la chiama.** `shell_exec`. In M11 la stessa funzione, senza una riga di
modifica, leggerà file veri da un disco minix — ed è il modo di verificare che
l'astrazione regga.

**Come si sbaglia.**

- **aspettare `read == 0` su un dispositivo**: la shell si pianta;
- **stampare con `%s`** un buffer che `read` non ha terminato. `read` restituisce
  quanti byte, non una stringa: o si termina il buffer a mano, o si stampa un
  carattere per volta;
- **dimenticare `vfs_close`.**

**Test.** `tests/shell.sh` digita `cat /dev/kbd`, poi una riga, e verifica che
esca. È la catena intera: IRQ 1 → ring buffer → `keyboard_getchar` →
`kbd_dev_read` → `chardev_read` → `vfs_read` → `cat`.

---

## Riepilogo

| funzione | righe circa | cosa dimostra |
|---|---|---|
| `devfs_init` | 20 | gli inode si costruiscono una volta, dal registro |
| `devfs_root` | 5 | l'ordine sbagliato si vede subito |
| `root_lookup`, `root_readdir` | 15 | una directory con una voce sola |
| `dev_lookup`, `dev_readdir` | 25 | una directory le cui voci **sono** un'altra tabella |
| `chardev_read`, `chardev_write` | 20 | il ponte, e un `read` che scarta l'offset |
| `shell_ls` | 30 | `readdir`, e i suoi tre valori di ritorno |
| `shell_cat` | 35 | perché il tipo dell'inode esiste |

Circa 150 righe. E alla fine `cat /dev/kbd` stampa quello che digiti, senza che
`cat` sappia cos'è una tastiera — che era il punto di tutto il secondo blocco.
