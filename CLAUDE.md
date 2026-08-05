# waltex

Kernel monolitico didattico per x86 32-bit, scritto da zero, avviato in QEMU.
Obiettivo: arrivare a un sistema di forma Unix — device, filesystem, shell come
processo utente in ring 3 — capendo ogni pezzo. Non è software da riutilizzare,
è un progetto per imparare.

Design completo e motivazioni delle scelte, in due spec:

- primo blocco (M1-M6b, kernel):
  `docs/superpowers/specs/2026-07-26-waltex-kernel-design.md`
- secondo blocco (M7-M17, userland):
  `docs/superpowers/specs/2026-07-29-waltex-userland-design.md`

Leggili prima di proporre modifiche architetturali.

Rispondi in italiano.

## Stato corrente

Stato: **primo blocco chiuso, M7, M8, M9, M10 e M11 (a-e) chiuse.** M12 (memoria) è la
prossima.

M1 chiusa: boot Multiboot, VGA text mode con scroll, cursore hardware e colore
corrente, seriale COM1, `kprintf`, `memcpy`/`memset`/`memset16`.
M2 chiusa: GDT propria a tre descrittori piatti ring 0, caricata con `lgdt` e
far jump, verificata rileggendo la tabella con `sgdt`.
M3 chiusa: IDT a 256 gate, 48 stub in assembly, PIC rimappato a 32-47,
`panic` con dump dei registri. Un'eccezione ora produce nome, `EIP` e registri
invece di una tripla fault muta.

M4 chiusa: PIT a 100 Hz sull'IRQ 0, prima `sti` del progetto, `kmain` che non
ritorna piu' ma dorme in `hlt`. Frequenza verificata contro l'orologio CMOS,
che e' un riferimento indipendente: 100 tick misurati in un secondo reale.

M5 chiusa: driver tastiera sull'IRQ 1, decodifica dello scancode set 1, e un
ring buffer a produttore singolo fra il gestore e il codice normale. `kmain`
fa l'eco di quello che si digita. La catena completa e' verificata iniettando
tasti nel monitor di QEMU con `tests/keyboard.sh`.

M6a chiusa: multitasking cooperativo. Tre flussi di esecuzione — `kmain`,
`task_a`, `task_b` — su tre stack separati, che si passano il controllo con
`task_yield()`.
M6b chiusa: multitasking preemptive. Il gestore del timer chiama `schedule()`,
i task non cedono piu' niente e il controllo viene tolto cento volte al
secondo. `tests/tasks.sh` verifica due proprieta' distinte: che le transizioni
siano molte, e che le corse siano LUNGHE — corse di lunghezza 1 vorrebbero dire
che i task stanno ancora cedendo volontariamente, cioe' che la prelazione non
c'e'.

M7 chiusa: una shell. `lineedit.c` trasforma i tasti in righe — accumula,
applica il backspace, dice quando la riga e' finita — e `shell.c` la spezza in
parole e cerca la prima in una tabella di otto comandi. Il `peek` e' lo
strumento di debug delle milestone che seguono: da M13 servira' a camminare le
tabelle delle pagine mentre le si scrive.

Il sink di eco di `lineedit` e' un puntatore a funzione per la stessa ragione
dei due sink di `kprintf`: senza, il modulo dovrebbe chiamare `kprintf` e il
test non potrebbe verificare **cosa** e' stato echeggiato, compresa la sequenza
`\b` spazio `\b` con cui si cancella.

`vga_putc` ha imparato `'\b'` in M7. Serviva: `'\b'` vale 8, quindi non passava
ne' per il ramo `c >= 32` ne' per quello del newline, e non faceva niente. Sulla
seriale il backspace funziona comunque perche' lo interpreta il terminale
all'altro capo; sul framebuffer no. La guardia sullo zero non e' pedanteria —
`cursor` e' un `uint16_t`, decrementarlo a zero da' 65535, il controllo di
scroll subito sotto scatta, e lo schermo scorre al primo backspace di troppo.

M8 chiusa: device layer. Un registro a capacita' fissa, e i tre driver che
esistevano gia' — VGA, seriale, tastiera — che si iscrivono come `console` 5:1,
`ttyS0` 4:64 e `kbd` 13:64, senza cambiare la loro logica. I numeri sono quelli
veri di Linux, verificati con `ls -l /dev/...`: costa zero e sta nella stessa
direzione del vincolo POSIX di M14.

Le tre convenzioni di M8, che M9 erediterà:

- **un puntatore a operazione nullo significa «non supportata»**, non «errore»:
  `console` non si legge, `kbd` non si scrive. E' la stessa convenzione di
  `exc_handlers[vec] == 0` in `idt.c`, ed e' cio' che permette a `devs` di
  stampare `-w` e `r-` senza un campo di capacita';
- **il registro copia, non punta.** Chi si iscrive passa una struct sullo stack,
  e quando la sua `*_init` ritorna quella memoria non e' piu' sua. Per questo
  `name` dentro `struct device` e' un array e non un `const char *`;
- **`read` ritorna quanti byte ha copiato davvero, e zero significa «adesso non
  c'e' niente», NON fine del file.** In M9 `cat /dev/kbd` fara' spin proprio su
  quello zero.

`device_init()` va chiamata da `kmain` **prima** di ogni `*_init()` dei driver,
perche' sono loro a iscriversi. Il tranello e' che funzionerebbe anche
dimenticandola, perche' `ndev` sta in `.bss` e al boot vale gia' zero: il
self-check sul conteggio esiste per quello.

Due bug di M8 trovati **leggendo e non eseguendo**, entrambi in `kbd_dev_read`,
ed entrambi non catturabili da nessun test: i self-check girano con il ring
vuoto, e i test host non possono iniettarci niente perche' `r` e' `static` in
`keyboard.c` e ci scrive solo il gestore dell'IRQ 1. Il primo ignorava `n` e
scriveva fino a 127 byte nel buffer del chiamante; il secondo, con `n == 0`,
consumava un carattere prima di controllare. E' la classe di guasto per cui
esiste la checklist di review qui sotto.

M9a chiusa: il nucleo del VFS. Due tabelle e un risolutore, e da
`vfs_open("/dev/kbd", O_RDONLY)` esce un numero piccolo da cui si legge senza
sapere cosa ci sia dall'altra parte.

**M9 e' stata divisa in due**, come M6a/M6b: M9a il nucleo, M9b `devfs` piu' `ls`
e `cat`. Il taglio separa cio' che si prova sull'host da cio' che esiste solo
davanti all'hardware — M9a e' la prima milestone del progetto **interamente fuori
da QEMU**, 75 controlli host e zero self-check.

Cio' che la rende testabile e' una scelta di interfaccia: **`vfs_init` RICEVE
l'inode della radice**. Nel kernel glielo passa `devfs`, nei test un albero finto
di sei nodi. E' lo stesso espediente del sink di eco di `lineedit`.

M9b chiusa: `devfs`, e `ls`/`cat` nella shell. Tre tipi di inode — la radice,
`/dev`, e una foglia per dispositivo — e `cat /dev/kbd` che attraversa sette
livelli.

Le tre cose di M9b che valgono per M11, dove la stessa struttura si ripete con
minix al posto di devfs:

- **l'albero sta FUORI da `vfs.c`.** `minixfs.c` riempie le stesse caselle di
  `inode_ops`, e il `cat` scritto in M9b legge file veri da disco senza una
  modifica. Se l'albero fosse stato dentro `vfs.c`, aggiungere il secondo
  filesystem vorrebbe dire riscriverlo.

  *(Qui avevo scritto «`vfs.c` e' finito, da qui a M17 non si tocca», ed era
  sbagliato: in M11b ha guadagnato la quinta casella `create`, `O_CREAT` e
  `vfs_mkdir`. Cio' che regge e' il taglio, non l'immutabilita'.)*;
- **un filesystem si inventa la propria forma.** La radice e `/dev` non vengono
  dal registro dei dispositivi — non sono dispositivi, e nell'hardware non esiste
  niente che si chiami `/`. Le scrive `devfs_init` a mano. Solo le foglie
  vengono dal registro. minix leggera' la forma dal disco, e il VFS non
  distinguera' i due casi;
- **`dir` e `ino` nelle `inode_ops` sembrano inutili e non lo sono.** In `devfs`
  ogni directory ha la sua funzione, quindi `dir` e' ignorato; `minix_lookup`
  sara' una sola funzione per migliaia di directory e `dir->ino` sara' l'unica
  cosa che le distingue. E' lo stesso parametro che `struct device *d` e' in M8:
  inutile finche' una funzione serve un oggetto solo.

Il collegamento fra `ino_devices[i]` e `device_at(i)` **non e' un fatto, e' un
patto** che `devfs_init` stabilisce con una riga (`priv = d`) e che ogni altra
funzione deve rispettare. E' il pattern degli array paralleli, ed e' la struttura
in cui gli indici scivolano: leggere il device da `ino_devices[i].priv` invece di
richiederlo al registro toglie il problema, perche' nome e inode vengono dallo
stesso oggetto.

Tre bug di M9b, tutti trovati **rileggendo** e tutti dello stesso genere — un
valore di ritorno che viola una convenzione:

- `root_lookup` ritornava `1` invece di `-1` quando non trovava. `vfs_resolve`
  controlla `< 0`, quindi credeva di aver trovato e camminava su `prossimo`, mai
  inizializzato. Nessun sintomo stabile: una cosa diversa a ogni boot;
- `chardev_read`/`chardev_write` ritornavano `1` con l'operazione non
  supportata. Li' `1` significa «ho trasferito un byte», quindi `cat` avanzava
  l'offset e stampava un buffer che nessuno aveva riempito;
- `devfs_init` ciclava fino a `device_count()-1`, e poi scriveva la radice
  nello slot rimasto libero: `dev_lookup("kbd")` restituiva un inode di tipo
  directory.

Il self-check `vfs_resolve("/dev/nonesiste") fallisce` esiste per il primo: e'
l'unico dei quattordici che lo prende, perche' nessun controllo positivo puo'
vedere un valore di ritorno sbagliato sull'insuccesso.

I livelli, e perche' sono separati:

```text
fds[t][i]  ──>  files[i]  ──>  struct inode
 per task      la posizione     l'identita', e vive nel FILESYSTEM
```

Due `open` sullo stesso path danno due `struct file` — due posizioni — e un solo
inode. Collassandoli, due letture indipendenti si ruberebbero la posizione. Per
questo `read` e `write` dentro `inode_ops` ricevono un **offset esplicito**.

L'albero **non e' nei dati, e' nella funzione `lookup`**: `struct inode` non ha
puntatori ai figli. Ogni directory sa rispondere «dammi il figlio che si chiama
cosi'», e camminare un path e' una catena di domande. E' cio' che rende possibile
a `devfs` di generare i figli dal registro dei dispositivi e a minix, in M11, di
leggerli dal disco senza tenerlo tutto in RAM.

Due emendamenti allo spec, dichiarati: la **cache di inode** e i **refcount** non
esistono in M9a. La cache serve in M11, quando gli inode vengono dal disco; i
refcount in M16, con `fork` e `dup`. Di conseguenza `struct inode` e
`struct file` non hanno il campo `refs`.

Nota su `irq.h`: da M9a ha un ramo `WALTEX_HOSTED` in cui `irq_save`/`irq_restore`
non fanno niente. Serve perche' `cli` e' privilegiata e in user space e' una
violazione di protezione — un test host che ci passasse sopra muore di SIGSEGV,
verificato. Senza quel ramo nessun test host potrebbe esercitare codice con una
sezione critica, e `file_alloc` ne ha una attraversata da ogni `vfs_open`.

M10 chiusa: driver ATA PIO in polling, LBA28, canale primario. `ata.c` l'ha
scritto Claude — nel protocollo non c'e' un concetto di sistemi operativi, e lo
spec l'aveva assegnato per questo. Il valore della milestone e' l'interfaccia che
esporta, `struct blockdev`, che M11 consumera' senza sapere che esista una porta
`0x1F0`.

**`struct blockdev` non e' `struct device`**, e la differenza non e' l'LBA: su un
dispositivo a caratteri «ho letto 3 byte su 64» e' normale, su un disco e' un
guasto. `read` e `write` ritornano **settori**, e un trasferimento parziale di
settore non esiste. Sotto la stessa interfaccia uno dei due dovrebbe mentire.

Niente registro, a differenza di M8: i dischi sono al massimo due e si prendono
per indice da `ata_drive(i)`. `include/blockdev.h` **non ha un `.c`** — e' una
pura interfaccia. Il registro si aggiunge il giorno che servissero due dischi
montati insieme, non prima.

I tranelli di M10, in ordine di quanto costano:

- **ogni attesa vuole un tetto.** Su un canale vuoto il bus fluttuante legge
  `0xFF`, cioe' BSY acceso: un `while (status & BSY)` senza contatore non
  ritorna mai, e il kernel si ferma dopo «timer a 100 Hz» senza un messaggio —
  sintomo identico a una tripla fault, causa completamente diversa;
- **BSY prima di tutto.** Finche' e' acceso gli altri sette bit non significano
  niente. Per questo `ata_wait` prende una MASCHERA: `BSY|DRQ` si chiede
  insieme, e cosi' non si possono guardare nell'ordine sbagliato. Leggere DRQ
  con BSY alto funziona su QEMU e muore su hardware vero;
- **ERR si controlla DENTRO il ciclo d'attesa**, non dopo: su un comando
  rifiutato BSY si spegne e DRQ non arriva mai, quindi un'attesa che guarda solo
  i bit richiesti consuma il tetto e riporta «scaduto» invece di «errore»;
- **l'attesa di DRQ sta dentro il ciclo dei settori**, una per settore. Il disco
  alza DRQ, consegna, riabbassa, poi prepara il prossimo;
- **`lba + count > nsectors` e' il controllo di limite SBAGLIATO**: con `lba`
  vicino a 2³² la somma gira. Si confronta per sottrazione;
- **le word 60-61 di IDENTIFY sono UN numero a 32 bit**, non due. Leggerne una
  sola tronca la capacita' a 65535 settori, che su un'immagine da 2048 non si
  vede;
- **`insw`/`outsw` contano WORD, non byte**, e vogliono `cld`: senza, il buffer
  si riempie all'indietro — leggibile, ordinato, sbagliato.

E il tranello che si e' presentato davvero, tre volte in `shell.c`: **il quarto
argomento di `read`/`write` e' un conteggio di SETTORI.** Trattarlo come byte
significa chiedere 16 settori — 8192 byte — dentro un buffer da 512 su uno stack
da 4096. Il valore di ritorno e' della stessa specie: sono settori, quindi si
confronta con `1` e non si usa come limite di un ciclo di stampa.

**La verifica del disco e' bidirezionale, ed e' la disciplina dell'orologio CMOS
di M4.** `tools/mkdisk.sh` scrive un pattern nel settore 1 prima che la VM parta,
e il kernel lo rilegge; il kernel scrive nel settore 2, e `tests/disk.sh` lo
rilegge da fuori con `od`. Un disco non puo' verificare se stesso.

Cosa hanno mostrato tre sabotaggi deliberati, e vale la pena tenerlo scritto:

| sabotaggio | chi lo prende |
|---|---|
| `write` nel settore sbagliato | i **self-check** — i tre settori dell'immagine hanno contenuti distinti |
| **`FLUSH CACHE` tolto** | **nessuno**: con `cache=writethrough` e una chiusura pulita QEMU versa comunque |
| `write` che trasferisce un settore in piu' | **solo `disk.sh`**, dopo aver aggiunto il controllo sul settore successivo |

Il flush resta perche' su hardware vero e' l'unica garanzia, **non** perche' un
test lo imponga: sta scritto in `disk.sh` accanto al controllo, cosi' non lo si
toglie credendolo coperto.

Tre note operative di M10:

- **`cache=writethrough` nella riga di QEMU** e' la seconda difesa: senza, le
  scritture restano in un buffer fino alla chiusura pulita, e un test che
  rilegge il file diventa intermittente;
- **tutti e cinque gli script attaccano il disco**, non solo `disk.sh`. I
  self-check dell'ATA girano a ogni boot, e senza immagine `kmain` si ferma su
  «N selftest falliti» prima di qualunque marker;
- **`tests/monitor.py`** manda comandi arbitrari al monitor. `sendkeys.py` manda
  solo tasti, e `quit` non e' un tasto.

Un errore da non ripetere, che e' costato una diagnosi sbagliata: **non lanciare
QEMU a mano mentre `mkdisk.sh` ricostruisce l'immagine.** Un processo staccato
che versa il proprio buffer sopra un file appena rifatto produce un self-check
rosso che non si riproduce, e sembra un bug del kernel.

M11a chiusa: minix v1 in **sola lettura**, e la radice del sistema viene dal
disco. `kernel/minixfs.c` l'ha scritto Claude su richiesta esplicita di Walter.

**Il riferimento e' `mkfs.minix` piu' `mount`**, cioe' due implementazioni che
non sono la nostra. `tools/mkminix.sh` costruisce l'immagine — vuole `sudo`,
perche' montare e' privilegiato — e il risultato si **committa** in
`tests/data/minix.img`: `make test` non puo' volere sudo, e committarla rende il
riferimento anche stabile invece che dipendente dalla versione di util-linux
installata. 256 KB grezzi che in git diventano 597 byte.

**53 controlli host, e sono la verifica piu' forte del progetto.** Funzionano
perche' `struct blockdev` ha due puntatori a funzione, e sull'host diventano
`fread` e `fseek` su un file: `minixfs.c` non si accorge che sotto c'e' un file
invece di un disco. E' il sink di `kprintf` e l'albero finto di `test_vfs.c`, la
terza volta, e questa paga piu' delle altre.

Il formato, VERIFICATO con `od` e non ricordato:

```text
blocco 0   boot   |  blocco 1   superblocco  |  poi imap, zmap, tabella inode
inode      32 byte, la RADICE E' L'INODE 1 (lo 0 significa "nessuno")
           32 inode per blocco (1024/32), NON 16
dirent     16 byte: uint16 ino + 14 char, NON terminato se lungo 14
zone       i_zone[9]: 7 dirette, [7] indiretta, [8] doppia. uint16, non uint32
```

Controllo di coerenza che vale la pena rifare a mano su ogni immagine nuova:
`2 + imap + zmap` e' il primo blocco degli inode, `ninodes * 32` arrotondato a
blocco e' la loro lunghezza, e la somma deve dare `s_firstdatazone`. Sulla nostra
immagine: `2+1+1 = 4`, `96*32 = 3` blocchi, `4+3 = 7 = s_firstdatazone`. E' il
controllo che ha smascherato il mio «16 inode per blocco», che era sbagliato.

Le cose di M11a che varranno anche in M11b:

- **la cache degli inode e' correttezza, non velocita'.** `lookup` restituisce un
  puntatore che deve sopravvivere alla chiamata, quindi non puo' essere una
  locale — e due `lookup` dello stesso file devono dare lo STESSO puntatore,
  altrimenti due `size` possono divergere. E' l'emendamento di M9a che scade qui;
- **`struct inode` non ha posto per le zone, e non deve averlo.** «Zona» e' una
  parola di minix. Da cui l'involucro `struct minode { struct inode vfs;
  uint16_t zone[9]; }` e `priv` che punta alle zone. L'alternativa — un array
  parallelo `zone[MAX_INODES][9]` — e' il pattern in cui gli indici scivolano,
  lo stesso che Walter aveva giustamente contestato in M9b;
- **`i_size` e' l'unica cosa che dice dove finisce un file.** Senza il
  troncamento, un file da 26 byte ne restituisce 1024, e i 998 in piu' non sono
  spazzatura casuale: sono dati veri di qualcun altro, gia' sul disco, quindi
  hanno l'aria di essere giusti;
- **una zona a zero e' un BUCO, non la fine.** La fine la dice `size`. E un
  puntatore indiretto nullo non si segue: sarebbe il blocco 0, cioe' il boot
  block letto come tabella di puntatori;
- **una directory e' un file normale.** `lookup` e `readdir` non leggono il disco
  da se': scorrono la directory con la stessa `minix_read` che serve i file;
- **`ino == 0` in una voce di directory significa CANCELLATA, non fine
  dell'elenco.** `lookup` la salta, `readdir` la consegna com'e' — perche' `idx`
  e' una posizione, e saltare renderebbe gli indici instabili. In M11a non ce ne
  sono; dopo il primo `unlink` di M11b si'.

**Due dischi da M11a**, e non e' una comodita': il settore 2 su cui `disk.sh`
scrive e' la prima meta' del superblocco minix, quindi le due immagini non
possono stare sullo stesso disco. `hda` e' quella a pattern di M10, `hdb` il
filesystem. `ata_init` provava gia' master e slave, quindi il kernel non e'
cambiato — ma **`priv` viene esercitato per la prima volta**: due `struct
blockdev` con lo stesso puntatore a `read`, e solo `priv` a distinguerle.

**L'innesto, che e' tutto cio' che c'e' di un mount.** La radice viene da minix e
`/dev` si aggancia con `minixfs_graft("dev", devfs_devdir())`: uno slot, non una
tabella — quella e' fuori scope nello spec. `minixfs_graft` riceve un
`struct inode *` e non sa da dove viene, quindi `minixfs.c` non include
`devfs.h`: il filesystem su disco non sa che esistano i dispositivi.

*(**`minixfs_graft` non esiste piu' da M11c**, e la frase qui sopra e' rimasta
perche' la scelta era ragionata e vale la pena vedere dove sbagliava. La seconda
delle due note qui sotto — «`lookup` e `readdir` devono essere d'accordo» — non
era una trappola: era il sintomo di una responsabilita' nel posto sbagliato.
La prima invece regge, ed e' l'errore che si rifarebbe con qualunque
meccanismo.)*

Due cose da non sbagliare, entrambe scoperte provando:

- **si monta `devfs_devdir()`, NON `devfs_root()`.** La radice di devfs ha una
  sola voce e si chiama `dev`: innestando quella si ottiene `/dev/dev/kbd`.
  Quattro self-check l'hanno preso;
- **`lookup` e `readdir` devono essere d'accordo sull'innesto.** Se compare solo
  nella prima, `cat /dev/kbd` funziona e `ls /` non mostra `dev`.

Nota su `ls /`: `dev` e `hello.txt` compaiono entrambi con il numero 2, e non e'
un bug. **I numeri di inode sono unici dentro un filesystem, non fra
filesystem** — e' la ragione per cui `struct stat` di Unix riporta anche un
device id accanto a `st_ino`.

Un errore di metodo da non ripetere, che e' costato una diagnosi sbagliata: **non
lanciare QEMU a mano mentre uno script ricostruisce l'immagine.** Un processo
staccato che versa il proprio buffer sopra un file appena rifatto produce un
self-check rosso che non si riproduce, e sembra un bug del kernel.

M11b chiusa: minix in **scrittura**. Bitmap, allocazione di inode e zone,
`create`, `mkdir`, crescita dei file e delle directory. `minixfs.c` e le
aggiunte a `vfs.c` le ha scritte Claude su richiesta esplicita di Walter.

**Il riferimento cambia verso, ed e' la novita' della milestone.** Fino a M11a
`mkfs.minix` scriveva e il nostro parser leggeva; qui scrive il kernel e
`fsck.minix` giudica.

**`mount` NON basta come oracolo**, ed e' misurato: spegnendo un bit nella bitmap
di un'immagine sana, `mount` la accetta e `ls` funziona, mentre `fsck` dice
`Inode 2 marked unused, but used for file '/hello.txt'` ed esce con 4. Un
filesystem incoerente si legge benissimo — il danno esce alla PROSSIMA
allocazione, quando quell'inode viene riusato.

Ha ripagato subito. `fsck` ha trovato un inode allocato e mai collegato, con
tutti gli 89 controlli host verdi:

```text
create(dir, "quindici_caratt")   il nome supera i 14 caratteri
  lookup            non c'e'
  inode_alloca      ALLOCA l'inode 11 e accende il bit
  dirent_inserisci  rifiuta il nome
  return -1                        e l'inode resta li'
```

Da cui la regola: **non si tocca il disco finche' non si sa che l'operazione
puo' riuscire.** La validazione del nome sta prima dell'allocazione, e
`bitmap_spegni` annulla un'allocazione fallita — che NON e' la `free` di
`unlink`, perche' gira solo sul percorso d'errore.

**`unlink` e' fuori di proposito**, per una ragione diagnostica: con la sola
allocazione le bitmap possono solo crescere, quindi un bit di troppo ha un
colpevole solo. Aggiungendo la liberazione, ogni disaccordo di `fsck` diventa
ambiguo — alloc che accende troppo, o free che spegne poco?

I TRE INDICI, che non si calcolano allo stesso modo, e sono la trappola numero
uno della milestone:

```text
tabella degli inode:  inode i  ->  offset (i - 1) * 32
bitmap degli inode:   inode i  ->  bit i                     NIENTE meno uno
bitmap delle zone:    zona z   ->  bit z - firstdatazone + 1
```

Verificato sui byte: il primo byte della imap e' `ff` con sette inode in uso —
con l'indice `i-1` sarebbe `7f`. La zmap ha `ff ff ff 7f`, cioe' il bit 0
riservato piu' trenta zone, e le zone usate vanno dalla 7 alla 36: quindi la
zona 7 e' il bit **1**.

Il resto di M11b che vale la pena ricordare:

- **il bit 0 e' riservato in entrambe le bitmap** e vale sempre 1. E' il motivo
  per cui l'inode 0 non esiste, e per cui lo zero puo' fare da «non trovato»
  nei due allocatori;
- **una zona appena allocata si AZZERA.** Se diventa un blocco di dati, il file
  ha in coda i resti di un altro; se diventa un blocco indiretto, quei resti
  vengono letti come puntatori a zone e il danno si sposta su file che non
  c'entrano;
- **`i_nlinks` e' il primo numero che `fsck` controlla.** Una directory nasce a
  2 — `.` piu' la voce nel genitore — e il genitore ne guadagna uno per via di
  `..`. Dimenticare l'ultimo da' `Inode 1 has 3 links, counted 4`;
- **`inode_scrivi` e' la funzione che si dimentica di chiamare**, e il sintomo e'
  che tutto funziona finche' il filesystem resta montato. Ogni controllo di
  scrittura nei test host passa da un RIMONTAGGIO per questo;
- **i campi che il VFS non ha si preservano rileggendo**: `i_uid`, `i_gid`,
  `i_time`. Azzerarli non fa protestare `fsck`, ma su `ls -l` i file cambiano
  proprietario appena il kernel li tocca;
- **il doppio indiretto si rifiuta in scrittura.** Sull'immagine da 256 KB non ci
  si arriva — servirebbero file oltre 519 KB — quindi sarebbe codice mai
  eseguito. L'asimmetria con la lettura e' voluta.

**`vfs.c` non era finito**, e la nota di M9b era sbagliata: `inode_ops` ha
guadagnato una quinta casella, `create`, perche' `vfs_open` con `O_CREAT` deve
poter chiamare qualcosa. `devfs` la lascia a zero, quindi `mkdir /dev/x`
fallisce da se' — la convenzione di M8, per la terza volta.

Due note su `vfs.c`: **`O_CREAT` e' un BIT**, quindi si prova con `&` e non con
`==`, perche' `flags` vale `O_WRONLY|O_CREAT`. E il genitore di un path si
ottiene copiando e troncando in `spezza_path`, non aggiungendo un parametro a
`vfs_resolve`: quella funzione ha 75 test addosso e non c'era ragione di
toccarla. Il caso da non sbagliare e' `/f.txt`, dove il genitore dopo il
troncamento sarebbe la stringa vuota invece di `/`.

**Le tabelle `inode_ops` usano inizializzatori designati** da M11b. Con la forma
posizionale, ogni tabella incompleta produce `missing initializer for field
'create'` a ogni build — un avviso permanente e giusto, cioe' un avviso che si
smette di leggere.

**Il Makefile non tracciava le dipendenze dagli header**, scoperto qui: cambiare
`vfs.h` non ricompilava niente, e si linkavano oggetti costruiti contro una
struct diversa. Adesso c'e' `-MMD -MP` con l'`-include` dei `.d`.

**I self-check di M11b CREANO file sull'immagine**, quindi ogni script di test
ricopia `tests/data/minix.img` in `build/minix.img` prima di partire. Senza,
`mkdir crea una directory nuova` fallisce al secondo script — verificato.

*(E in M11c si e' scoperto che la regola `$(MINIXIMG)` del Makefile **non
bastava**: confronta i timestamp, e dopo il primo boot la copia e' piu' recente
del riferimento, quindi make la considera aggiornata per sempre. `make test`
restava verde perche' ogni script fa la `cp` da se'; `make run` e `make debug`
no, e il secondo `make run` trovava il lavoro del primo. Adesso passano da un
bersaglio `.PHONY minix-fresh`. Il difetto era li' da M11b: si e' visto la prima
volta che qualcuno ha lanciato `make run` due volte.)*

M11c chiusa: il **mount vero**. La tabella sta in `vfs.c`, la sostituzione e' una
riga in `vfs_resolve`, e `minixfs_graft` non esiste piu'.

**Il difetto che ha chiuso, e non era che la graft fosse rotta:** fino a M11b,
per montare qualcosa bisognava *modificare il filesystem che possedeva il punto
di innesto*. Un `walterfs` sotto `/mnt` avrebbe voluto una `minixfs_graft` piu'
grande, dentro minixfs. E' al contrario, e la nota di M11a su `lookup` e
`readdir` che devono restare d'accordo era il sintomo.

- **il punto di mount ESISTE sul disco.** In Unix `mount` non aggiunge un nome,
  ne **copre** uno — `mount /x` con `/x` inesistente da' `ENOENT`. Da cui
  `mkdir dev` in `tools/mkminix.sh`, e il guadagno grosso: **`minix_readdir` non
  sa piu' niente dei mount**, perche' il nome `dev` glielo da' il disco. Meta'
  del problema e' sparita invece di spostarsi di un livello. Per la stessa
  ragione non si crea il mountpoint se manca: uno che appare dal nulla nasconde
  un errore di battitura;
- **la chiave della tabella e' il PUNTATORE, non `ino`.** E' la nota di M11a che
  presenta il conto: `dev` e `hello.txt` hanno entrambi il numero 2. Una tabella
  per numero monterebbe devfs anche sopra un file regolare. Il self-check «il
  mount non ha coperto `/etc`» esiste solo per quello;
- **una chiave TESTUALE non regge**, ed e' la prima che viene in mente: `/dev`,
  `//dev` e `/dev/` sono tre stringhe e un solo inode, e `vfs_resolve` le accetta
  tutte e tre di proposito. Inoltre la risoluzione cammina un componente alla
  volta e la stringa del punto raggiunto non esiste — provata, dava `/dev` che si
  risolve e `/dev/kbd` che no;
- **non si puo' «scambiare l'inode»**, che e' l'altra idea naturale. Quell'inode
  vive nella cache di minixfs e viene riletto dal disco. Lo scambio non e' nei
  dati, e' nel risolutore — come l'albero non e' nei dati ma nella `lookup`;
- **`risolvi_mount` ha un ciclo esterno CON UN TETTO.** Serve all'impilamento, e
  il tetto perche' montare A su B e B su A costruisce un ciclo. E' la regola di
  M10 («ogni attesa vuole un tetto») applicata a un ciclo invece che a
  un'attesa. `MAX_MOUNTS` giri e' il tetto esatto: la catena piu' lunga ha un
  anello per slot. **Non lo prende nessun test**, come il `FLUSH CACHE` di M10;
- **l'ordine in `kmain` si e' ROVESCIATO**, per due ragioni indipendenti:
  `vfs_init` azzera la tabella, e `vfs_mount` risolve un path. Fino a M11b la
  graft veniva prima di `vfs_init`;
- **un mount fallito non fa piu' ripiegare su devfs.** La radice su disco resta
  buona e `/dev` resta la directory vuota che e' sull'immagine. Perdere il
  filesystem intero perche' un mount non e' andato sarebbe sproporzionato — ed e'
  possibile solo adesso, perche' il punto di mount esiste comunque.

**Il debito della chiave a puntatore, da saldare in M16.** I due puntatori devono
restare validi *e continuare a significare lo stesso file* per tutta la vita del
mount, e la tabella non ha modo di accorgersi se cambiano sotto. Oggi regge, ed
e' verificato invece che sperato: la cache di minixfs **non sfratta** — prende il
primo slot con `ino == 0` e ritorna 0 quando sono esauriti, quindi una cache
piena da' un `resolve` fallito e non una corruzione. Lo romperebbero un
`minixfs_init` a tabella piena (irraggiungibile: `kmain` monta una volta sola) e
una cache a sfratto, che e' la cosa naturale da fare quando in M12 arriva
`kmalloc`. La cura e' una sola per entrambi — un refcount sull'inode del punto di
mount — ed e' lo stesso che serve a `umount`.

**E non e' `(st_dev, st_ino)`**, che e' la prima alternativa che viene in mente.
Quella coppia serve — arriva in **M14**, perche' `struct stat` la vuole, e c'e'
una nota in `include/vfs.h` accanto a `struct inode` — ma risolve un problema
diverso: sopravvive allo sfratto *come chiave*, mentre `mounts[].root` resta
comunque un puntatore, e le `struct file` pure. **La sicurezza dallo sfratto non
e' un problema della tabella di mount, e' un problema del VFS**, ed e' per questo
che Linux inchioda la dentry del mountpoint dentro `struct vfsmount` invece di
ricostruirla per chiave. Da non riusare per quello: `major`/`minor` dentro
`struct inode` dicono *quale dispositivo l'inode E'*, non *su quale filesystem
vive* — e con la domanda sbagliata tutti i file regolari collidono, perche' i
loro `major`/`minor` sono zero.

Il controllo che da' senso alla milestone **non e' automatico**: si monta
l'immagine sull'host e si guarda che `/dev` sia **vuota**. Montare non scrive
niente sul filesystem montante, e questa e' tutta la differenza fra montare e
creare.

E il controllo migliore e' quello che **non e' cambiato**: `tests/shell.sh` cerca
`dev` in `ls /` e i tre dispositivi in `ls /dev` esattamente come prima. Le due
righe passano provando cose diverse — la prima adesso legge il disco, la seconda
attraversa la tabella di mount — ed e' la conferma che il taglio e' nel punto
giusto.

Un controllo host e' caduto e non era stato previsto: **«con un numero di inode
non ancora usato» si aspettava l'8**, che se l'e' preso `dev`. E' l'unico
controllo dell'intera suite che guarda un numero di inode assoluto invece di un
nome, e per questo l'unico che se n'e' accorto.

M11d chiusa: **procfs**, e il vero scopo non era `/proc`.

**Era la prova che mancava a M11c.** Fino a qui la tabella di mount aveva un
cliente solo, ed era lo stesso di prima: devfs, che era gia' agganciato con la
graft. `procfs` e' il primo filesystem scritto DOPO il meccanismo, e la misura e'
binaria — `git diff --stat kernel/vfs.c kernel/minixfs.c` dev'essere vuoto. Lo e'.

- **il contenuto NON ESISTE finche' qualcuno non lo chiede.** minixfs serve byte
  che stanno sul disco, devfs byte che arrivano da un driver, procfs li
  **genera** — e smettono di esistere quando `read` ritorna. Da cui: `read`
  prende un offset, quindi si genera il testo intero e se ne consegna una fetta.
  Linux ha inventato `seq_file` per questo problema, e vale la pena guardarlo
  dopo averci sbattuto contro;
- **il buffer di generazione e' LOCALE, non statico.** Statico costerebbe 128 byte
  una volta sola, ma fra il «genero» e il «copio» ci sta un tick del timer: due
  `cat` su status diversi in parallelo si mescolerebbero. Sullo stack non e'
  condiviso con nessuno, e non serve nessuna sezione critica;
- **NESSUNO STATO fra una `read` e l'altra.** Un flag statico che alterni «prima
  riga» e «finito» funziona per `cat` e si rompe per tutto il resto: una `lseek`
  all'indietro da' zero, e due letture intrecciate si spengono a vicenda perche'
  il flag e' uno per tutto il filesystem. E' il difetto che la prima versione
  aveva, e non lo vedeva nessun `cat`;
- **`size` vale 0, ed e' corretto.** La dimensione non si conosce prima di
  generare. `vfs_read` non consulta `size` — verificato — e `shell_cat` esce
  quando `read` ritorna 0. E' anche quello che fa Linux: `ls -l /proc/1/status`
  mostra 0 byte;
- **un inode per task, non uno condiviso.** Un solo `struct inode` riempito a
  ogni `lookup` fa distruggere alla seconda ricerca il risultato della prima, e
  due `open` su file diversi finiscono sullo stesso oggetto. E' la lezione di
  M11a — `lookup` restituisce un puntatore che deve sopravvivere alla chiamata —
  e `cat` non la vede perche' apre, legge e chiude prima che qualcuno interferisca;
- **l'indice del task sta in `ino`, e non c'e' nessun `struct task *`.** Terza
  volta che `dir->ino` distingue directory servite dalla stessa funzione — la
  prima nota sta nelle `inode_ops` da M9b. E il divieto del puntatore e' la
  conclusione **opposta** a quella della tabella di mount, ed e' giusto che sia
  opposta: uno slot della cache di inode non viene mai riciclato, uno slot della
  tabella dei task si', e in M16 comincia a succedere;
- **nessun inode vale zero.** Radice 1, task da 2, status da `2 + MAX_TASKS`. Lo
  zero significa «nessun inode» — e' il valore con cui una voce di directory
  minix dice «cancellata» — quindi usarlo per un file vero mente a chiunque lo
  controlli;
- **niente `\t`.** Linux allinea `/proc/N/status` con i tab; `'\t'` vale 9, e
  `vga_putc` gestisce solo `>= 32` piu' `'\n'` e `'\b'`. Un tab funzionerebbe
  sulla seriale e sparirebbe sul framebuffer — il bug del backspace di M7,
  rifatto per la seconda volta;
- **`idx` in `readdir` e' una POSIZIONE, non un indice di task.** Con i task 0 e 3
  attivi, `idx == 1` da' `"3"`. Confonderli fa fermare `ls /proc` al primo slot
  libero, e il sintomo e' il peggiore che ci sia — una lista **plausibile e
  incompleta**. Oggi non si vedrebbe, perche' al boot i task sono contigui: c'e'
  un controllo host che costruisce il buco a mano con `task_slot`.

**`snprintf` e' nato qui, sopra `kvprintf`**, e vale piu' della milestone: riusa
il formattatore invece di duplicarlo, e il `va_copy` che si e' portato dietro ha
chiuso il debito di M1 — `kprintf` formattava due volte riusando lo stesso
`va_list`, legale solo su i386. Resta un debito piu' piccolo al suo posto: lo
stato del sink e' una **globale**, perche' `kvprintf` non ha un parametro di
contesto. Il save/restore dentro `vsnprintf` rende sicuro l'annidamento ma **non
l'intreccio**: se un task viene prelazionato a meta', l'altro gli scrive nel
buffer. Oggi il chiamante e' uno. La cura e' un `void *ctx` nel sink.

**I self-check girano PRIMA di `task_init`**, e la scoperta si e' girata in un
controllo: in quell'istante la tabella dei task e' tutta `TASK_FREE`, quindi
`/proc` dev'essere **vuota**, mentre `tests/shell.sh` al prompt la pretende
**piena**. La stessa domanda in due istanti diversi, ed e' il controllo piu' forte
che si possa fare su procfs — un `procfs_init` che si fosse memorizzato la
tabella passerebbe ogni altra verifica e cadrebbe su questo.

Due cose che **non prende nessun test**: il buffer di generazione statico, e il
campo `State` che dice `R (running)` per il task corrente e `R (ready)` per gli
altri — sull'host il task corrente e' sempre lo 0, quindi i due rami danno lo
stesso risultato. La seconda si guarda dal prompt: `cat /proc/1/status` dice
`running`, perche' e' la shell stessa a leggerlo.

E una cosa che nel piano avevo scritto **sbagliata**: «dopo `spin` compaiono due
directory in piu'». No — `demo_tasks_init()` crea i due task di prova **al boot**,
silenziosi; `spin` accende solo la loro stampa. `ls /proc` mostra quattro voci sia
prima sia dopo.

M11e chiusa: il **registry dei dispositivi**, e il polimorfismo che si sposta dal
device layer al VFS. `kernel/dev.c` e `kernel/devio.c` li ha scritti Walter con
Claude, `blk_inode_ops` Claude su richiesta esplicita.

**Il difetto che chiude era strutturale, non una dimenticanza.** `struct device` di
M8 portava `read`/`write` a byte, quindi «essere un dispositivo» e «avere una vista
a byte» erano la stessa proprietà. Un disco ha la prima e non la seconda — la sua
granularità è il settore, e «ho letto 3 byte su 64» su un disco è un guasto — quindi
non poteva iscriversi, quindi non esisteva in `/dev`. Sotto la stessa interfaccia
uno dei due avrebbe dovuto mentire.

I tre file, e nessuno include ciò che non gli serve:

```text
dev.c      dev.h, memory.h                    il registry, AGNOSTICO
devio.c    dev.h chardev.h blockdev.h vfs.h   il ponte, unico switch su kind
devfs.c    dev.h devio.h vfs.h                l'albero, solo nomi
vfs.c      NIENTE di tutto questo             non sa cosa sia un dispositivo
```

L'ultima riga è la misura binaria, la stessa di M11d: `git diff --stat kernel/vfs.c`
vuoto. Lo è — `INODE_BLOCKDEV` in `vfs.h` è l'unica modifica al VFS.

**La trappola numero uno, e si è presentata davvero:** `dev_entry.impl` è un
PUNTATORE mentre `device_register` COPIAVA. Le `struct chardev` dei tre driver
erano locali di funzione. Il guasto non si vede provando — l'`assert`
sull'iscrizione passa, il kernel boota, `ls /dev` mostra cinque dispositivi — e
arriva alla prima CHIAMATA, dopo che quello stack è stato riusato: **General
Protection sul vettore 13** dentro il self-check che fa `d->write(d, "AB", 2)`, con
nei registri i resti di `vga_clear`. Ed è esattamente il caso che il vecchio
commento di `serial_init` descriveva come ipotetico: «se il registro conservasse il
puntatore invece di copiare, il guasto si manifesterebbe esattamente qui».

La convenzione di M8 **si è spezzata in due**, e va tenuto scritto: il **nome** si
copia ancora (`dev_entry.name` è un array), le **operazioni** si riferiscono. Da cui
`static` obbligatorio nei driver.

Il resto di M11e che vale la pena ricordare:

- **il bug che la milestone raccoglie**: la guardia sul nome non terminato in
  `device_register` era **codice morto**. `strpos` ha un ramo esplicito
  `else if (a == '\0') r = -1`, quindi cercando il terminatore ritorna sempre -1 e
  `-1 > 16` è falso. E il suo `return 1` avrebbe violato il contratto «0 oppure -1».
  Ciò che proteggeva davvero era la `strlen` sotto, cioè **precisamente la scansione
  illimitata contro cui l'header metteva in guardia per venticinque righe**.
  Funzionava per accidente di layout — `major` stava subito dopo `name` — e in
  `dev_entry` al suo posto c'è `kind`;
- **`&&` e non `||`** nel controllo di `kind`: un valore non può essere uguale a due
  cose diverse, quindi con `||` la condizione è sempre vera e **nessun dispositivo
  si iscrive**. È De Morgan, e il compilatore non può dirlo — è codice legale che
  significa qualcos'altro;
- **lo `switch` su `enum dev_kind` esiste in UN posto solo**, `devio.c`. `devfs` non
  include `chardev.h` né `blockdev.h`: chiede `devio_fill_inode` e riceve un inode
  riempito. Il secondo cliente della stessa regola è `devio_caps`, che serve alla
  colonna `r-`/`-w` di `devs` e risparmia a `shell.c` di guardare i puntatori a
  operazione;
- **il salto lo decide il valore di ritorno, non un test su `kind`.** `devfs` chiede
  «sai servirmi questo?» e non gli importa perché no — per questo l'arrivo di
  `blk_inode_ops` **non ha cambiato `devfs.c` di una riga**;
- **`ino` si scrive per ULTIMO** nel riempimento pigro. Due task prelazionati sullo
  stesso slot scrivono valori identici e la corsa è benigna; con `ino` prima, il
  secondo riceve un inode con `ops` ancora nullo. Nessuna sezione critica serve, ed
  è la disciplina del ring buffer di M5;
- **`prepara()` la chiamano sia `lookup` sia `readdir`**, e la seconda non è un
  dettaglio: con il riempimento pigro un dispositivo mai cercato ha lo slot vuoto,
  quindi un `readdir` che si fidasse del solo marcatore lo salterebbe. Passando
  entrambe da lì, l'accordo fra le due è per COSTRUZIONE — la nota di M11a risolta
  alla radice invece che raccomandata;
- **il vincolo d'ordine di `kmain` è SPARITO**, e la cosa è verificata invece che
  dichiarata: spostando `devfs_init` prima di *ogni* `*_init()` dei driver, `ls /dev`
  elenca ancora tutto e i self-check passano. Resta il vincolo opposto, `dev_init`
  prima di tutti;
- **`b->read` in UN punto solo, sempre `count == 1`.** È la riga in cui la buffer
  cache si infilerà in M12. Una via rapida per le letture allineate darebbe due punti
  da sostituire e non si eserciterebbe mai — `cat` legge a blocchi di 64 byte.
  `test_devio.c` conta le chiamate proprio per proteggere quella cucitura;
- **il clamp si fa per SOTTRAZIONE**, `n > size - off`. `off + n > size` gira, ed è
  la regola di M10 applicata ai byte invece che ai settori;
- **una scrittura parziale è READ-MODIFY-WRITE.** Senza la lettura, i 502 byte
  intorno finiscono con quello che c'era nel bounce buffer — dati veri di un altro
  settore, quindi con l'aria di essere giusti. È la zona non azzerata di M11b;
- **`vfs_read` dereferenzia `ops` senza controllarlo**, quindi un inode con `ops`
  nullo non fallisce: fa una tripla fault. «Un inode ha sempre `ops`» è un
  invariante del VFS, e `devfs` non consegna ciò che non può servire;
- **il bounce buffer è LOCALE**, 512 byte, un ottavo dello stack di un task. Statico
  farebbe mescolare due letture prelazionate a metà — la lezione di procfs.

**La verifica migliore è bidirezionale, e ripete la disciplina dell'orologio CMOS di
M4:** gli stessi byte letti attraverso il VFS (`open`, `lseek(500)`, `read(100)`) e
in LBA da `ata_drive(0)`, ricuciti a mano. L'intervallo attraversa il confine di
settore, quindi esercita in un colpo l'offset non allineato, la doppia iterazione e
lo skip che torna a zero al secondo giro. Una delle due strade non passa
dall'adapter, quindi non può sbagliare come lui.

**E la misura della milestone è un numero che riconverge:** dopo il passo 2
`dev_count()` valeva 5 e `/dev` ne mostrava 3, perché `devio` rifiutava i dischi.
Quella discrepanza era una prova — diceva che registry e albero sono cose separate,
che fino a M11d non era osservabile perché coincidevano sempre. Adesso tornano
uguali.

**E la lezione che vale piu' della milestone, trovata facendo l'analisi
dell'incapsulamento a cose finite.** La regola «lo `switch` su `kind` esiste in un
posto solo» sta scritta in `devio.h`, ed era **gia' violata da `shell_lsblk`** —
scritto poche ore dopo, dalla stessa mano, sapendola. Non per distrazione: perche'
i due lookup tipizzati prendevano un NOME, e chi enumera ha gia' la voce in mano.
Passare da `dev_blockdev(e->name)` vorrebbe dire buttare l'indice per ricercare lo
stesso oggetto, quindi la scorciatoia — un test su `kind` piu' un cast a mano — era
la via piu' corta.

La cura non e' stata ricordarsene meglio: sono state tre righe,
`devio_blockdev_of(e)`, che prendono la voce e fanno filtro e cast insieme. Da li'
anche `devio_caps` e `devio_fill_inode` ci passano, e i cast di `impl` sono scesi da
cinque posti a **due funzioni**.

**Una convenzione che il codice non rende conveniente non regge**, nemmeno per chi
l'ha appena scritta. E' la stessa specie del `return 1` di M9b e della guardia morta
di `strpos`: il rimedio non e' la disciplina, e' rendere la via giusta anche la piu'
corta. Da cui il controllo che vale la pena rifare a ogni milestone che tocchi il
device layer:

```sh
grep -rn "(struct chardev \*)e->impl\|(struct blockdev \*)e->impl" kernel/
```

Deve dare **due righe**, entrambe in `devio.c`. Leggere `kind` per MOSTRARE cos'e'
un dispositivo resta legittimo — `devs` stampa `c` o `b`; leggerlo per decidere come
interpretare `impl` no.

Tre cose scoperte facendo, che nessuno aveva previsto:

- **`test_dev.c` crashava invece di fallire**, per una guardia sul nullo mancante.
  Un test deve FALLIRE: con il codice sotto rotto, l'output sparisce proprio nel
  momento in cui serve leggerlo;
- **`devs` non può stampare la parola «caratteri»**: contiene una `r`, e
  `tests/shell.sh` verifica le capacità cercando `r` e `w` nella riga *dichiarando*
  quella precondizione. Il test l'ha preso. Si usano `c` e `b`, che sono le lettere
  di `ls -l`;
- **`cat /dev/hda 15` avvelena il log.** La firma è lunga 14, quindi il
  quindicesimo byte è il NUL del padding — e **un solo NUL rende il file binario per
  `grep`**, che smette di contare e risponde «Binary file matches». Il numero deve
  fermarsi esattamente dove finisce il testo. E l'asserzione non può usare
  `fra_prompt`, perché `cat` non stampa un newline finale e i byte finiscono sulla
  riga del prompt successivo.

Stato dei test: 617 host, 134 self-check in QEMU, 10 marker, 6 script dentro la VM
(`smoke.sh`, `keyboard.sh`, `shell.sh`, `tasks.sh`, `disk.sh`,
`minixwrite.sh`). Numeri
**misurati**, non
ricordati: `make -C tests/host -s run | grep -cE "ok +--"` e la stessa cosa sul
log seriale.

Da M9b `tests/host/Makefile` linka `vfs.c` dentro `test_shell`, perche' `ls` e
`cat` lo chiamano. Nessun test host li esercita — senza una radice il VFS non ha
niente da risolvere — ma il link e' comunque un controllo, ed e' lo stesso genere
di controllo che in M7 ha scoperto lo `strcmp` mancante.

Nota: da M7 i due task di prova stanno in `kernel/demo.c` e partono
**silenziosi** — la loro stampa continua rendeva il prompt illeggibile. Li
accende il comando `spin`, e `tests/tasks.sh` se lo manda da se' con
`sendkeys.py`: il test provoca la condizione che misura invece di appoggiarsi a
un effetto collaterale del kernel. E' caduto il filtro `tr -d 'AB'` di
`keyboard.sh`.

I nomi dei tasti che il monitor di QEMU accetta, verificati: `ret` per Invio,
`spc` per lo spazio, `backspace`. `enter` e `space` vengono **rifiutati**.

Trappola dei test host, scoperta in M7: girano in ambiente **hosted**, quindi il
linker riempie di nascosto i buchi con la libc di sistema. `strcmp` mancava in
`memory.c` e i test passavano contro quella di glibc — l'unico posto dove il
buco e' venuto fuori e' il link freestanding del kernel. I test host verificano
la logica, non la completezza.

Le tre regole del context switch, da non violare:

- in `task_yield` l'indice di chi esce e quello di chi entra sono due cose
  distinte: `task_switch(&tasks[prev].esp, tasks[next].esp)`. Collassarli in
  uno fa salvare il contesto uscente nella casella di quello entrante, e il
  conto si presenta due switch dopo;
- il primo argomento e' un **posto** dove salvare, il secondo un **valore** da
  caricare. L'asimmetria e' voluta;
- il frame falsificato da `task_create` serve solo per il primo ingresso: la
  prima chiamata del task lo calpesta usando lo stack normalmente;
- `eflags` fa parte del contesto e `switch.S` lo salva con `pushfl`/`popfl`.
  Senza, un task appena creato erediterebbe gli interrupt spenti dal gate del
  timer e non verrebbe mai piu' interrotto: il kernel stamperebbe la lettera
  del primo task all'infinito. Verificato togliendolo;
- in `isr_handler` l'EOI va mandato **prima** di chiamare il gestore, perche'
  il gestore del timer commuta e non tornerebbe mai all'EOI. E' sicuro perche'
  i gate sono interrupt gate. Verificato invertendo l'ordine: 45000 caratteri
  della stessa lettera, zero transizioni;
- le sezioni critiche si chiudono con `irq_restore`, non con `sti`:
  `vga_putc` viene chiamata anche da `panic_regs`, dove gli interrupt sono
  spenti deliberatamente. E restano corte — `set_cursor` sta fuori, perche'
  quattro `outb` per carattere con gli interrupt spenti farebbero perdere tick
  al timer.

La regola del ring buffer, da non violare: `head` lo scrive solo il produttore
(il gestore), `tail` solo il consumatore (`kmain`). Ognuno legge l'indice
dell'altro e non lo tocca. Per questo non serve nessun `cli`, e per questo non
c'e' un contatore degli elementi: sarebbe l'unica variabile scritta da
entrambi. Non chiamare `keyboard_getchar` da un interrupt handler: il buffer
ammette un solo consumatore.

Applicato in M7: il consumatore e' stato **spostato**, non aggiunto. Era il
ciclo di idle di `kmain`, adesso e' `shell_task`, e `kmain` ha smesso di
leggere. Leggere da due posti farebbe sparire caratteri a caso: digitando
`echo`, la shell ne vedrebbe `eh` e l'idle stamperebbe `co`.

Il debito di concorrenza del secondo blocco, da tenere presente da M9: la
tabella dei descrittori e' **per task**, ma la tabella dei file aperti e la
cache di inode sono **condivise** fra task prelazionati cento volte al secondo.
`refs++` e' read-modify-write, cioe' lo stesso `count++` che nel ring buffer si
e' evitato con la struttura — e qui la struttura non salva, perche' tutti i task
aprono file per definizione. Servono `irq_save`/`irq_restore` intorno ai
refcount e alla ricerca di uno slot libero, corti come in `vga_putc` e per la
stessa ragione: una sezione critica lunga fa perdere tick, e il self-check di M4
sulla frequenza lo nota.

Lo smoke test ora concede 15 secondi invece di 5, perche' la misura della
frequenza costa due secondi di tempo reale. L'uscita anticipata resta, quindi
in pratica termina in poco piu' di due secondi.

Nota sul provare le eccezioni a mano: non usare `int $N` su un vettore che ha
un codice d'errore (8, 10-14, 17). L'`int` software non ne fa impilare uno,
ma lo stub corrispondente e' generato con `ISR_ERR` e assume che ci sia:
lo stack risulta sfalsato di quattro byte e il dump mente. Usa un vettore
senza codice d'errore, o provoca una fault vera.

Le milestone del primo blocco sono M1 boot+VGA, M2 GDT, M3 IDT+exception+PIC,
M4 timer PIT, M5 tastiera, M6a multitasking cooperativo, M6b preemptive.

Il secondo blocco, in ordine — la forma Unix prima, l'isolamento dopo:

```text
M7   shell            editor di riga + tabella comandi          CHIUSA
M8   device layer     struct device, registro, i driver si iscrivono  CHIUSA
M9a  VFS, il nucleo   path, inode, tabella fd, open/read/write   CHIUSA
M9b  devfs            /dev sopra il registro, ls e cat nella shell  CHIUSA
M10  ATA PIO          driver disco in polling + strato a blocchi  CHIUSA
M11a minix v1         superblocco, inode, zone — LETTURA               CHIUSA
M11b minix v1         bitmap, allocazione, creazione — SCRITTURA     CHIUSA
M11c mount            tabella di mount nel VFS, minixfs_graft rimossa  CHIUSA
M11d procfs           /proc sopra la tabella dei task, secondo mount   CHIUSA
M11e registry         il polimorfismo si sposta nel VFS, hda in /dev   CHIUSA
M12  memoria          mmap Multiboot, allocatore di pagine, kmalloc
M13  paging           page directory, spazi di indirizzamento per processo
M14  TSS + ring 3     int 0x80, ABI Linux i386, validazione puntatori utente
M15  ELF + exec       loader, build user-space, crt0, stub delle syscall
M16  fork/wait        init come PID 1, /bin/sh in ring 3
M17  newlib           opzionale: printf e malloc veri
```

Aggiorna questa sezione quando una milestone viene chiusa.

## Debiti tecnici

**Questo elenco e' un INDICE, non la spiegazione.** Il perche' di ogni voce sta
dove serve — nel commento accanto al codice, o nella sezione della milestone che
l'ha creato — e qui c'e' solo una riga piu' il puntatore. Due copie della stessa
motivazione divergono, ed e' l'errore che questo progetto ha gia' fatto due volte
(«`vfs.c` e' finito», «il refcount arriva in M11»).

Ordinati per **quando mordono**, non per anzianita'.

### Mordono in M12-M13

1. **`mounts[].punto` e' un puntatore in una cache che oggi non sfratta** —
   `kernel/vfs.c`, commento sopra `struct mountpoint`. Regge perche'
   `inode_carica` ritorna 0 a cache piena invece di sfrattare. **M12 porta
   `kmalloc`, e lo sfratto e' la cosa naturale da fare quando ce l'hai**: da quel
   giorno la tabella di mount tiene puntatori penzolanti. La cura e' il 2.
2. **`struct inode` non ha un refcount** — `include/vfs.h`, commento sopra la
   struct. Serve a **tre** cose ed e' un lavoro solo: `fork`/`dup` in M16,
   inchiodare l'inode del mountpoint (il debito 1), e dire a `umount` se c'e'
   qualcosa aperto sotto. La `struct file` ha gia' la sua sezione critica.

   E il cliente piu' vicino nel tempo e' `dup`, non `fork`: **e' `dup`, non un
   pezzo del VFS, cio' che manca ai flussi standard.** `fd_alloc` in `vfs.c`
   spiega perche' fd 0/1/2 non sono un meccanismo del kernel ma una convenzione
   di `init` — senza `dup`, `stdout` e `stderr` sono due `open` con due posizioni
   indipendenti, che e' esattamente cio' che `2>&1` chiede di non fare.
3. **Manca `st_dev`** — `include/vfs.h`, stesso commento. Arriva in **M14**
   perche' `struct stat` la vuole. Da non confondere con `major`/`minor`, che
   dicono *quale dispositivo l'inode E'*, non *su quale filesystem vive*.
4. **`size` di un disco gira a 4 GiB** — `kernel/devio.c`, dentro
   `devio_fill_inode`. `nsectors * SECTOR_SIZE` in `uint32_t` sfora a 8388608
   settori, e LBA28 arriva a 128 GiB: raggiungibile in principio, invisibile sui
   nostri dischi da 2048 e 512 settori. Morde in **M14** con `struct stat`, ed e'
   **lo stesso lavoro del debito 3**: una dimensione a 64 bit in `struct inode`.

### Mordono quando qualcuno tocca quel file

5. **Lo scroll usa `memcpy` su regioni sovrapposte** — `kernel/vga.c`, nella
   funzione di scroll. Comportamento indefinito; funziona per la direzione
   attuale. E' anche l'ultimo punto di `vga.c` che butta via il `volatile` del
   framebuffer, con il cast a `(void *)`.
6. **`put_uint` tratta la base 10 come con segno** — `kernel/kprintf.c`, il
   controllo `((int32_t)value) < 0 && base == 10`. Quindi `%d` non stampa
   decimali senza segno sopra 2³¹. **`snprintf` NON l'ha risolto**: il difetto e'
   dentro `put_uint`, e `shell.c` ci convive con dei cast a `int`.
7. **Il bounce buffer di `blk_read`/`blk_write` sta sullo stack** —
   `kernel/devio.c`, commento sopra le due funzioni. 512 byte, cioè **un ottavo**
   dello stack di un task, dentro la catena `shell_cat → vfs_read → blk_read`.
   Locale e non `static` di proposito — la lezione di procfs — quindi la cura non è
   renderlo statico ma **sostituirlo con la buffer cache**, e c'è già un punto solo
   in cui infilarla: `b->read(b, lba, bounce, 1)`. M12.
8. **`copia_nome` in minixfs ha un TODO** — `kernel/minixfs.c`, sopra la
   funzione. Non e' rotta — esiste perche' un nome di 14 caratteri non e'
   terminato — ma il TODO e' li'.

### Non mordono, ma sono difetti visibili

9. **`ring.c` divide invece di mascherare**: `% RING_SIZE` nelle due funzioni,
   quando `RING_MASK` esiste in `include/ring.h` e non lo usa nessuno. Una
   divisione dentro il gestore della tastiera.
10. **`shift_pressed` azzerato nel ramo sbagliato** — `kernel/keyboard.c`, il ramo
   `else` di `keyboard_handler` copre **qualunque** tasto normale: shift premuto
   piu' `AB` da' `Ab`. Andrebbe nel ramo del break code (`0xAA`, `0xB6`). E anche
   allora resterebbe il difetto minore: un flag per due tasti, quindi rilasciarne
   uno lo spegne mentre l'altro e' premuto — servirebbe una maschera a due bit.
   Non blocca niente, i comandi della shell sono minuscoli.

### Assenze dichiarate, che NON sono debiti

Non sono cose fatte male, sono cose non fatte. Stanno qui perche' la domanda
«perche' manca?» venga risposta una volta invece che ogni volta.

- **niente blocking I/O.** Non esistono `task_block`/`task_wake`, quindi la shell
  fa spin su `keyboard_getchar`. E' la ragione per cui le pipe non ci sono —
  `read()` su una pipe vuota *deve* bloccare. Annotato in `include/device.h`,
  `include/ata.h` e `include/shell.h`;
- **niente `unlink`.** Fuori da M11b di proposito: con la sola allocazione le
  bitmap possono solo crescere, quindi un bit di troppo ha un colpevole solo.
  Conseguenza: il ramo «voce cancellata» di `minix_readdir` non si esercita mai;
- **il doppio indiretto si rifiuta in scrittura** — `kernel/minixfs.c`.
  Sull'immagine da 256 KB servirebbero file oltre 519 KB, quindi sarebbe codice
  mai eseguito. L'asimmetria con la lettura e' voluta;
- **`umount` non esiste.** Vuole il debito 2.

### Saldati

- **La protezione del nome non terminato in `device_register` era codice morto**
  (aperta in M8, saldata in M11e). La guardia usava `strpos` per cercare `'\0'`,
  che ha un ramo esplicito `else if (a == '\0') r = -1` in `memory.c`: ritornava
  sempre -1, e `-1 > 16` è falso. Quindi la riga non è mai stata eseguita, e il suo
  `return 1` avrebbe violato il contratto «0 oppure -1» dichiarato tre righe sopra.

  Ciò che proteggeva davvero era la `strlen` sotto, cioè **esattamente la scansione
  illimitata contro cui l'header metteva in guardia per venticinque righe**. Ha
  funzionato per quattro milestone per accidente di layout: `major` stava subito
  dopo `name`, quindi la `strlen` si fermava uno o due byte fuori dall'array.

  Due cose che il debito nascondeva, e che sono uscite solo saldandolo:

  - **i due test host che lo coprivano PASSAVANO**, e uno dei due dichiarava di
    «distinguere una scansione limitata da una strlen con il limite sbagliato».
    Distingueva la lunghezza, non il modo di misurarla — e la differenza si vede
    solo cambiando il layout della struct, cosa che `dev_entry` ha fatto;
  - **il caso forte non è più costruibile.** In M8 si azzerava `major` per mettere
    uno zero subito dopo l'array; in `dev_entry` quel posto è `kind`, e azzerarlo
    significa `DEV_NONE`, che viene rifiutato da un altro controllo. Il test resta
    utile — verifica l'ordine dei controlli — ma **ha perso potere diagnostico**, ed
    è annotato dentro il test perché un controllo che sembra forte e non lo è è
    peggio di uno assente.

- **`kprintf` formattava due volte riusando lo stesso `va_list`** (aperto in M1,
  saldato dopo M11d). La cura era quella scritta nel debito stesso — una passata
  sola con un sink doppio — e ci si e' arrivati partendo da un'altra domanda:
  togliere la globale da `vsnprintf`. Il sink di `kvprintf` ha guadagnato un
  `void *ctx`, e `kputc_console` e' venuto gratis.

  Tre cose che il debito nascondeva, e che sono uscite **solo saldandolo**:

  - **`panic.c` aveva lo stesso bug e nessuno l'aveva notato**: due `kvprintf`
    con lo **stesso** `va_list`, sopravvissuto al `va_copy` che aveva sistemato
    `kprintf`. Un panic che mente e' il posto peggiore dove averlo;
  - **niente verificava che `kprintf` arrivasse alla VGA**, ed era un buco aperto
    da M1: i controlli della VGA chiamano `vga_putc` direttamente, quelli di
    `kprintf` leggono la seriale, e il pezzo in mezzo non lo guardava nessuno.
    Adesso c'e' `check_kprintf_due_sink`, **verificato con un sabotaggio**;
  - **il prezzo, dichiarato**: l'output sui due dispositivi si alterna per
    carattere. Stessi byte nello stesso ordine su ciascuno; l'unica differenza e'
    che una tripla fault a meta' di una `kprintf` adesso tronca anche la riga
    sulla seriale, mentre prima la seriale era gia' completa.

  La lezione che vale piu' del debito: **un debito annotato con la propria cura
  si salda quando qualcos'altro rende quella cura conveniente**, non quando ci si
  ricorda di lui. Per questo l'elenco e' ordinato per «quando morde».

## Regola non negoziabile: chi scrive cosa

Walter sta scrivendo questo kernel per capirlo. Se Claude scrive i moduli
concettualmente rilevanti, il progetto perde il suo scopo.

**File che Claude NON scrive.** Su questi Claude spiega i concetti prima,
risponde a domande durante, e fa code review dopo — ma non produce
l'implementazione, nemmeno parzialmente, nemmeno "come esempio da cui partire",
nemmeno se richiesto in modo generico come "fammi vedere come si fa":

```text
kernel/vga.c        kernel/kprintf.c    kernel/gdt.c      kernel/idt.c
kernel/pic.c        kernel/panic.c      kernel/timer.c    kernel/keyboard.c
kernel/task.c       kernel/switch.S     kernel/ring.c     kernel/memory.c

secondo blocco:
kernel/shell.c      kernel/dev.c        kernel/vfs.c      kernel/devfs.c
kernel/devio.c      kernel/minixfs.c    kernel/procfs.c   kernel/pmm.c
kernel/paging.c     kernel/syscall.c
user/sh.c
```

`kernel/device.c` non esiste piu' da M11e: si e' diviso in `dev.c` — il registry
agnostico — e `devio.c`, il ponte fra i due mondi dei dispositivi e il VFS.
Entrambi restano di Walter, e `devio.c` **piu'** degli altri: e' l'unico posto del
kernel dove `enum dev_kind` si apre in uno switch, cioe' dove vive tutto il
polimorfismo del device layer.

Vale anche per le **aggiunte** a `kernel/task.c`: `fork`, `exec` e `wait` sono
di Walter come il resto del file.

**File che Claude scrive e mantiene** (infrastruttura: non insegna nulla e
costa solo tempo):

```text
Makefile            linker.ld           boot/multiboot.S    kernel/main.c
kernel/serial.c     kernel/selftest.c   include/types.h     include/io.h
include/panic.h     kernel/gdt.S        kernel/isr.S        tests/**
docs/**

secondo blocco:
kernel/ata.c        kernel/elf.c        kernel/tss.c
user/crt0.S         user/syscall.S      user/Makefile     user/user.ld
tools/**
```

`ata.c` e' una sequenza fissa di `outb` trascritta da un datasheet, `elf.c` e'
parsing di struct: sono i due punti del blocco dove il rapporto fra tempo speso
e concetti appresi e' piu' basso. `tss.c` e' bit-packing in un descrittore GDT,
che Walter ha gia' fatto in M2.

Gli header dei moduli di Walter (`vga.h`, `idt.h`, `task.h`, …) li scrive
Claude: sono contratti d'interfaccia, già fissati nello spec, e servono per
compilare.

**Se Walter chiede esplicitamente il codice di un file della prima lista:**
prima chiedi se preferisce un suggerimento a parole o una spiegazione del
concetto sottostante. Se confermano di volere il codice, scrivilo — la
decisione è loro. Ma la richiesta va fatta una volta, non ogni volta.

Zona intermedia: se Walter è bloccato, la scala è pseudocodice → una singola
riga chiave → il registro o il bit specifico da guardare. In quest'ordine, non
partendo dalla fine.

## Build ed esecuzione

```text
make          build/waltex.elf
make run      QEMU con finestra, output VGA visibile
make test     headless: test host + smoke test in QEMU, exit code reale
make debug    qemu -s -S in attesa di gdb sulla :1234
make clean
```

Prerequisito: `qemu-system-x86` (`sudo apt install qemu-system-x86`). Non c'è
cross-compiler: si usa il gcc di sistema con `-m32 -ffreestanding`.

Dopo ogni modifica al codice del kernel esegui `make test`, non solo `make`.
Un kernel che compila non è un kernel che booota.

## Vincoli del codice

Questo è codice freestanding: **non esiste la libc**. Niente `stdio.h`,
`string.h`, `stdlib.h`, `assert.h`. I tipi vengono da `include/types.h`, scritto
a mano. Se serve `memset` o `strlen`, si scrivono.

- **Nessuna allocazione dinamica fino a M12.** Array statici a capacità fissa:
  `MAX_TASKS 8`, ring buffer tastiera 128 byte, e nel secondo blocco
  `MAX_DEVICES 16`, `MAX_INODES 64`, `MAX_OPEN_FILES 32`, `TASK_FDS 8`.
  L'allocatore arriva in M12 perché il paging lo **forza** — una page directory
  per processo, allineata a 4 KB — non prima. Con gli array statici il
  fallimento è deterministico e il bilancio della RAM si vede a tempo di link.
- **Assembly in sintassi GNU as** (file `.S`), mai nasm.
- `assert()` è sempre attivo e chiama `panic()`. Non introdurre `NDEBUG`.
- Nessun `float`/`double`: l'FPU non è inizializzata.
- Ogni sottosistema ha una `*_init()` esplicita, chiamata da `kmain` in ordine
  visibile. Nessuna inizializzazione implicita o lazy.
- `kprintf` scrive su VGA **e** su COM1. La seriale è ciò che leggono i test:
  non aggiungere output diagnostico solo su VGA.

**Vincolo POSIX, dal secondo blocco.** Non si porta glibc e non si eseguono i
coreutils GNU — misurato: un `puts` statico contro glibc fa dieci syscall di
cerimonia per una di lavoro, e fra quelle `set_thread_area` (che implica una LDT
per thread, perché `errno` sta in TLS), `getrandom` e l'infrastruttura futex, più
il vettore ausiliario con `AT_RANDOM` prima di `main`. Quello è un progetto di
compatibilità ABI Linux, non di scrittura di un kernel.

Ma la porta si tiene aperta, e costa zero:

- codici d'errore come **ritorno negativo** con i valori Linux (`-ENOENT` = -2,
  `-EBADF` = -9, `-EINVAL` = -22, `-ENOSYS` = -38), mai un `-1` generico;
- i **numeri di syscall veri di Linux i386**: 1 exit, 2 fork, 3 read, 4 write,
  5 open, 6 close, 7 waitpid, 11 execve, 12 chdir, 19 lseek, 20 getpid,
  39 mkdir, 41 dup, 42 pipe, 45 brk, 54 ioctl, 106 stat, 108 fstat,
  141 getdents. Così gli stub di una libc compilata per Linux funzionano non
  modificati;
- `int $0x80` con gli argomenti in `ebx ecx edx esi edi ebp`;
- `struct stat` col layout Linux i386;
- firme POSIX nel VFS: `open(path, flags, mode)`, non `vfs_apri()`.

La vetta raggiungibile è **newlib** (M17): vuole ~15 stub, che sono esattamente
la forma del VFS di M9.

**Un puntatore che arriva da ring 3 non è un puntatore.** Da M14 ogni argomento
di syscall che è un indirizzo va validato prima dell'uso — dentro lo spazio
utente, mappato, con la lunghezza che non scavalca il confine. Saltare questo
controllo significa che un processo utente può far scrivere al kernel dove
vuole.

## Disciplina delle milestone

Una milestone alla volta. Non anticipare codice di milestone successive,
nemmeno se "tanto poi serve": lo scopo della struttura incrementale è che
quando qualcosa si rompe la superficie di sospetto sia di poche decine di
righe.

Ogni milestone si chiude con: kernel che booota, smoke test verde, un commit.
Il commit lo **proponi** quando i test passano, con un messaggio nella forma
`M3: idt, exception handler, remap PIC` — ma non eseguirlo se Walter non
conferma.

## Review: cosa guardare in questo dominio

Quando fai code review dei moduli di Walter, i bug che contano qui non sono
quelli di stile. In ordine di quanto costano da diagnosticare:

- Bit-packing dei descrittori GDT/IDT: limit, granularità, access byte,
  la parte alta della base o dell'offset dimenticata.
- EOI al PIC mancante o inviato al PIC sbagliato per gli IRQ 8-15: il primo
  interrupt arriva, il secondo mai.
- Offset dello stack in `switch.S` non allineati a cosa `struct task` contiene
  davvero, e ordine di push/pop asimmetrico.
- Accesso a stato condiviso fra handler e codice normale senza `cli`/`sti`
  (ring buffer della tastiera, tabella dei task), o `sti` fatto troppo presto.
- Variabili toccate da un handler non dichiarate `volatile`.
- `struct` dei descrittori senza `__attribute__((packed))`: il compilatore
  inserisce padding e l'hardware legge spazzatura.
- Handler che fanno troppo lavoro, o che chiamano `kprintf` dove non serve.

Dal secondo blocco si aggiungono, nello stesso ordine di costo:

- `CR0.PG` acceso con una mappa identità che non copre il kernel **e le tabelle
  stesse**: il prossimo fetch fa fault e la tripla fault non dice niente. Le
  tabelle si camminano e si verificano *prima* di scrivere `CR0`.
- Il TSS: serve per una cosa sola, da dove la CPU prende `esp0` quando un
  interrupt arriva mentre è in ring 3. Sbagliato, e *ogni* interrupt in user mode
  è un double fault.
- `refs++` sui refcount condivisi del VFS senza sezione critica.
- Argomenti di syscall che sono indirizzi, usati senza validazione.
- `argc`/`argv`/`envp` disposti male sullo stack utente in `execve`: il
  programma parte e legge spazzatura, cioè fallisce lontano dalla causa.

## Debug

Non debuggare a tentativi. Gli strumenti esistono già:

- `make debug` più `gdb`, `target remote :1234`,
  `symbol-file build/waltex.elf`. Il kernel è compilato con `-g`.
- `-d int,cpu_reset` nei flag QEMU logga ogni interrupt e ogni reset con lo
  stato della CPU: è così che si identifica una tripla fault.
- `-no-reboot` è già attivo: la VM si ferma invece di ciclare.

Se il sintomo è "la VM riparte in silenzio", il sospetto quasi sempre è nella
milestone appena scritta, ed è un problema di tabelle o di stack, non di logica.

## Lettura di accompagnamento

Linux 0.01 è materiale di lettura per milestone (`head.s`, `kernel/traps.c`,
`kernel/sched.c`, `kernel/keyboard.s`, `include/linux/sched.h`), non una
struttura da seguire. Non proporre di portare quel codice.

Nel secondo blocco Linux 0.01 diventa **più** pertinente, non meno: `fs/`
implementa esattamente il minix v1 di M11, `fs/exec.c` è l'`execve` di M15,
`kernel/fork.c` è M16.

Si aggiungono **xv6** del MIT — il riferimento migliore per il VFS a tre livelli
di M9 e per il confine delle syscall di M14, perché fa le stesse scelte spiegate
meglio — il **Tanenbaum** per il capitolo sui filesystem, che descrive minix v1
perché minix è suo, e il **manuale Intel volume 3A** capitoli 4 (paging) e 7
(task management, per il TSS), che in M13 e M14 serve spesso perché OSDev è
ambiguo.
