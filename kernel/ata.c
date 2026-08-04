#include "ata.h"
#include "blockdev.h"
#include "devio.h"
#include "io.h"
#include "memory.h"
#include "panic.h"

/* Driver ATA PIO in polling, canale primario, LBA28. Vedi include/ata.h per il
   perche' di ognuna di quelle tre scelte.

   Non c'e' nessun concetto di sistemi operativi qui dentro: c'e' un protocollo.
   Si scrivono sei registri in un ordine fisso, si aspetta che un bit si spenga,
   si leggono 256 word. La parte che vale la pena capire e' l'interfaccia che
   questo file ESPORTA — struct blockdev — non quello che ci sta sotto. */

#define ATA_PRIMARY_BASE  0x1F0
#define ATA_PRIMARY_CTRL  0x3F6

/* Gli offset da base. In lettura e scrittura la stessa porta significa cose
   diverse: 1 e' features in scrittura ed errore in lettura, 7 e' il comando in
   scrittura e lo stato in lettura. */
#define ATA_REG_DATA      0     /* 16 bit, non 8 */
#define ATA_REG_ERROR     1
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA_LO    3
#define ATA_REG_LBA_MID   4
#define ATA_REG_LBA_HI    5
#define ATA_REG_DRIVE     6
#define ATA_REG_STATUS    7
#define ATA_REG_COMMAND   7

/* I bit del registro di stato, e l'ordine in cui vanno guardati.

   BSY e' il primo di tutti: FINCHE' E' ACCESO GLI ALTRI SETTE NON SIGNIFICANO
   NIENTE. Leggere DRQ mentre BSY e' alto e' il modo canonico di scrivere un
   driver che funziona su QEMU — che e' veloce e prevedibile — e muore su
   hardware vero. Per questo ata_wait prende una MASCHERA invece di un bit: la
   coppia BSY|DRQ si chiede insieme, e cosi' non si puo' guardarli nell'ordine
   sbagliato. */
#define ATA_SR_BSY        0x80
#define ATA_SR_DRDY       0x40
#define ATA_SR_DF         0x20
#define ATA_SR_DRQ        0x08
#define ATA_SR_ERR        0x01

#define ATA_CMD_READ      0x20
#define ATA_CMD_WRITE     0x30
#define ATA_CMD_FLUSH     0xE7
#define ATA_CMD_IDENTIFY  0xEC

/* Il tetto di OGNI attesa, ed e' la difesa contro la trappola numero uno di
   M10: su un canale vuoto il bus fluttuante legge 0xFF, cioe' BSY acceso, e un
   ciclo senza limite non ritorna mai. Il kernel si fermerebbe dopo "timer a
   100 Hz" senza un messaggio — sintomo identico a una tripla fault, causa
   completamente diversa.

   Dieci milioni e' generoso: su QEMU un settore e' istantaneo, ma un disco vero
   che si sveglia prende secondi, e sul caso normale questo numero non costa
   niente perche' il ciclo esce al primo giro. */
#define ATA_TIMEOUT       10000000

#define ATA_MAX_DRIVES    2

/* Cio' che finisce in blockdev.priv: QUALE disco.

   E' il primo posto del progetto in cui priv serve davvero. In M8 nessuno lo
   usava e la sua ragione d'essere era una promessa; qui la promessa si
   riscuote, perche' i due dischi hanno lo STESSO puntatore a read e l'unica
   cosa che li distingue e' questa struct. */
struct ata_drive {
    uint16_t base;
    uint16_t ctrl;
    int      drive;    /* 0 master, 1 slave — i bit 4 di 0x1F6 */
};

/* static, e non locali di ata_init: nessuno copia queste struct.

   In M10 questa era la DIFFERENZA con M8, un piano sotto: la' i driver riempivano
   una struct sullo STACK e device_register la copiava, quindi la memoria locale
   andava bene, mentre qui ata_drive() restituisce l'indirizzo di questi slot e
   devono sopravvivere a ata_init.

   Da M11e non e' piu' una differenza, e' la REGOLA: il registry conserva
   dev_entry.impl, cioe' un puntatore, quindi anche i tre driver a caratteri hanno
   dovuto rendere static la propria struct. Questo file non e' cambiato — ci era
   arrivato prima, per una ragione sua. */
static struct ata_drive infos[ATA_MAX_DRIVES];
static struct blockdev  drives[ATA_MAX_DRIVES];
static int              ndrives;

/* Il ritardo di 400 ns che il protocollo vuole dopo aver selezionato un disco.

   Quattro letture della porta di stato ALTERNATA, non un ciclo vuoto: il chip
   vuole quattro cicli di bus, e un for vuoto il compilatore lo puo' eliminare —
   con volatile diventerebbe comunque un'attesa di durata sconosciuta. inb e'
   asm volatile, quindi queste quattro restano. */
static void ata_400ns(uint16_t ctrl)
{
    inb(ctrl);
    inb(ctrl);
    inb(ctrl);
    inb(ctrl);
}

/* Gira finche' i bit selezionati da mask valgono val, o finche' non scadono i
   tentativi. E' l'unica attesa del file: ogni altra funzione ci passa.

   Si legge 0x3F6 e non 0x1F7, e la differenza conta: leggere il registro di
   stato principale CANCELLA l'interrupt pendente. In polling non si vede,
   perche' l'IRQ 14 e' mascherato e nessuno lo guarda — ma la lettura alternata
   costa uguale e non lascia una mina per il giorno in cui si passasse
   all'interrupt.

   ERR e DF si controllano DENTRO il ciclo, non dopo. Su un comando rifiutato
   BSY si spegne e DRQ non si accende mai: un'attesa che guardasse solo i bit
   richiesti arriverebbe fino in fondo al tetto e riporterebbe "scaduto" invece
   di "errore". Due diagnosi diverse per la stessa causa, e quella sbagliata
   manda a cercare un disco lento che non esiste. */
static int ata_wait(uint16_t ctrl, uint8_t mask, uint8_t val, int limite)
{
    while (limite-- > 0) {
        uint8_t st = inb(ctrl);

        if (st & (ATA_SR_ERR | ATA_SR_DF))
            return -1;

        if ((st & mask) == val)
            return 0;
    }

    return -1;
}

/* Chiede al disco chi e', e ne ricava la capacita'.

   La sequenza e' fissa e l'ordine e' vincolato; le due righe che non sono
   trascrizione sono il controllo su stato == 0 e quello sulla firma ATAPI. */
static int ata_identify(uint16_t base, uint16_t ctrl, int drive,
                        uint32_t *nsectors)
{
    uint16_t id[256];
    uint8_t  st;

    /* 0xA0 e non 0xE0: IDENTIFY non ha un indirizzo, quindi il bit LBA non si
       accende e i quattro bit bassi restano a zero. */
    outb((uint16_t)(base + ATA_REG_DRIVE), (uint8_t)(0xA0 | (drive << 4)));
    ata_400ns(ctrl);

    /* Azzerare i quattro registri di indirizzo fa parte del protocollo, e
       serve al controllo sulla firma piu' sotto: se il disco e' ATAPI, ce li
       riscrive lui. */
    outb((uint16_t)(base + ATA_REG_SECCOUNT), 0);
    outb((uint16_t)(base + ATA_REG_LBA_LO),   0);
    outb((uint16_t)(base + ATA_REG_LBA_MID),  0);
    outb((uint16_t)(base + ATA_REG_LBA_HI),   0);

    outb((uint16_t)(base + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);
    ata_400ns(ctrl);

    /* La rilevazione vera: stato ZERO significa che non c'e' niente. Il bus
       fluttuante darebbe 0xFF, un disco presente qualcosa con DRDY acceso. Lo
       zero e' l'unico valore che nessun dispositivo produce. */
    st = inb((uint16_t)(base + ATA_REG_STATUS));
    if (st == 0)
        return -1;

    if (ata_wait(ctrl, ATA_SR_BSY, 0, ATA_TIMEOUT) < 0)
        return -1;

    /* La firma ATAPI. QEMU puo' presentare un CD-ROM sullo stesso canale, e un
       dispositivo ATAPI risponde a IDENTIFY con un errore dopo aver messo
       0x14 0xEB nei due registri che abbiamo appena azzerato. Senza questo
       controllo si finirebbe ad aspettare un DRQ che non arriva mai — cioe' a
       consumare il tetto intero per poi dire la cosa sbagliata. */
    if (inb((uint16_t)(base + ATA_REG_LBA_MID)) != 0 ||
        inb((uint16_t)(base + ATA_REG_LBA_HI))  != 0)
        return -1;

    if (ata_wait(ctrl, ATA_SR_BSY | ATA_SR_DRQ, ATA_SR_DRQ, ATA_TIMEOUT) < 0)
        return -1;

    /* 256 word, tutte. Leggerne meno lascia il disco con DRQ acceso, e a
       sbagliare non e' questo comando ma il PROSSIMO. */
    insw((uint16_t)(base + ATA_REG_DATA), id, 256);

    /* Le word 60 e 61 sono UN NUMERO A 32 BIT spezzato in due, non due numeri.
       Leggerne una sola tronca la capacita' a 65535 settori — che su
       un'immagine da 2048 non si vede, e in M11 si', quando l'immagine minix
       sara' piu' grande. Il self-check sul numero esatto esiste per questo.

       *nsectors si scrive SOLO in caso di successo: e' la convenzione di
       vfs_resolve e di shell_parse_hex, e per la stessa ragione — chi chiama
       deve poter tenere quello che aveva. */
    *nsectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);

    return 0;
}

/* Il controllo di limite, e l'unica riga interessante e' la terza.

   lba + count > nsectors sarebbe la scrittura naturale, ed e' SBAGLIATA: con
   lba vicino a 2^32 la somma gira e il controllo passa. Si confronta per
   sottrazione, che qui non puo' andare sotto zero perche' lba < nsectors e'
   gia' stato verificato. */
static int ata_range_ok(const struct blockdev *b, uint32_t lba, uint32_t count)
{
    /* Il registro del conteggio e' un byte, e zero significa 256: e' il motivo
       per cui il caso count == 0 va intercettato dal chiamante e non lasciato
       arrivare fin qui. */
    if (count > 256)
        return 0;

    if (lba >= b->nsectors)
        return 0;

    if (count > b->nsectors - lba)
        return 0;

    return 1;
}

/* Scrive i sei registri e lancia il comando. La parte comune di read e write:
   cambia solo il byte finale. */
static int ata_prepare(struct ata_drive *a, uint32_t lba, uint32_t count,
                       uint8_t cmd)
{
    if (ata_wait(a->ctrl, ATA_SR_BSY, 0, ATA_TIMEOUT) < 0)
        return -1;

    /* Il bit-packing di 0x1F6, la categoria di bug piu' costosa del progetto:

         bit 7   1     fisso
         bit 6   LBA   1 = indirizzamento lineare, 0 = CHS
         bit 5   1     fisso
         bit 4   DRV   0 master, 1 slave
         bit 3-0       LBA 27-24

       0xE0 accende i due fissi e il bit LBA. E' l'unico registro in cui
       l'indirizzo e la selezione del disco convivono, ed e' esattamente il
       genere di dimenticanza che in M2 mangiava il limite della GDT. */
    outb((uint16_t)(a->base + ATA_REG_DRIVE),
         (uint8_t)(0xE0 | (a->drive << 4) | ((lba >> 24) & 0x0F)));
    ata_400ns(a->ctrl);

    outb((uint16_t)(a->base + ATA_REG_SECCOUNT), (uint8_t)count);
    outb((uint16_t)(a->base + ATA_REG_LBA_LO),   (uint8_t)(lba));
    outb((uint16_t)(a->base + ATA_REG_LBA_MID),  (uint8_t)(lba >> 8));
    outb((uint16_t)(a->base + ATA_REG_LBA_HI),   (uint8_t)(lba >> 16));

    outb((uint16_t)(a->base + ATA_REG_COMMAND), cmd);

    return 0;
}

static int ata_dev_read(struct blockdev *b, uint32_t lba, void *buf,
                        uint32_t count)
{
    struct ata_drive *a = (struct ata_drive *)b->priv;
    uint8_t *p = (uint8_t *)buf;
    uint32_t i;

    /* Zero settori non e' un errore, ed e' la classe di bug di M8: kbd_dev_read
       con n == 0 consumava un carattere prima di controllare. Qui il danno
       sarebbe peggiore, perche' 0 nel registro del conteggio significa 256. */
    if (count == 0)
        return 0;

    if (a == 0 || !ata_range_ok(b, lba, count))
        return -1;

    if (ata_prepare(a, lba, count, ATA_CMD_READ) < 0)
        return -1;

    /* L'attesa sta DENTRO il ciclo, una per settore. Il disco alza DRQ,
       consegna un settore, lo riabbassa, poi ne prepara un altro. Aspettare una
       volta sola e poi leggere count * 256 word da' un buffer con il primo
       settore giusto e il resto spazzatura — e su QEMU, che e' veloce,
       funzionerebbe anche. */
    for (i = 0; i < count; i++) {
        if (ata_wait(a->ctrl, ATA_SR_BSY | ATA_SR_DRQ, ATA_SR_DRQ,
                     ATA_TIMEOUT) < 0)
            return -1;

        /* WORD, non byte: SECTOR_SIZE / 2. Passare SECTOR_SIZE riempirebbe due
           settori di buffer con uno solo di dati. */
        insw((uint16_t)(a->base + ATA_REG_DATA), p, SECTOR_SIZE / 2);
        p += SECTOR_SIZE;
    }

    return (int)count;
}

static int ata_dev_write(struct blockdev *b, uint32_t lba, const void *buf,
                         uint32_t count)
{
    struct ata_drive *a = (struct ata_drive *)b->priv;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t i;

    if (count == 0)
        return 0;

    if (a == 0 || !ata_range_ok(b, lba, count))
        return -1;

    if (ata_prepare(a, lba, count, ATA_CMD_WRITE) < 0)
        return -1;

    for (i = 0; i < count; i++) {
        if (ata_wait(a->ctrl, ATA_SR_BSY | ATA_SR_DRQ, ATA_SR_DRQ,
                     ATA_TIMEOUT) < 0)
            return -1;

        outsw((uint16_t)(a->base + ATA_REG_DATA), p, SECTOR_SIZE / 2);
        p += SECTOR_SIZE;
    }

    /* FLUSH CACHE, e non e' opzionale: senza, il disco puo' tenere i dati in un
       buffer proprio. Su hardware vero si perdono a un reset; su QEMU possono
       non raggiungere il file, e tests/disk.sh — che rilegge l'immagine da
       FUORI — diventerebbe intermittente. E' meta' della difesa: l'altra meta'
       e' cache=writethrough nella riga di QEMU, dall'altro lato del confine.

       Se il flush fallisce si ritorna -1 anche se i settori sono partiti: dal
       punto di vista di chi ha chiamato, un dato che non e' garantito su disco
       non e' scritto. */
    outb((uint16_t)(a->base + ATA_REG_COMMAND), ATA_CMD_FLUSH);

    if (ata_wait(a->ctrl, ATA_SR_BSY, 0, ATA_TIMEOUT) < 0)
        return -1;

    return (int)count;
}

void ata_init(void)
{
    static const char nomi[ATA_MAX_DRIVES][4] = { "hda", "hdb" };
    uint32_t n;
    int d;

    ndrives = 0;

    for (d = 0; d < ATA_MAX_DRIVES; d++) {
        /* Prima la capacita', POI l'iscrizione. Iscrivere un disco e riempire
           nsectors dopo lascerebbe una finestra in cui ogni controllo di limite
           di ata_range_ok confronta contro zero. */
        if (ata_identify(ATA_PRIMARY_BASE, ATA_PRIMARY_CTRL, d, &n) < 0)
            continue;

        infos[ndrives].base  = ATA_PRIMARY_BASE;
        infos[ndrives].ctrl  = ATA_PRIMARY_CTRL;
        infos[ndrives].drive = d;

        memcpy(drives[ndrives].name, nomi[d], 4);
        drives[ndrives].nsectors = n;
        drives[ndrives].read     = ata_dev_read;
        drives[ndrives].write    = ata_dev_write;
        drives[ndrives].priv     = &infos[ndrives];

        /* L'iscrizione nel registry, da M11e. I numeri sono quelli veri di Linux
           per il canale IDE primario — hda e' 3:0 e hdb 3:64, verificati con
           ls -l /dev/hd* e non ricordati: costa zero e sta nella direzione del
           vincolo POSIX.

           Il minor si calcola su d e NON su ndrives, ed e' la stessa ragione per
           cui il nome viene da nomi[d]: d e' la posizione fisica — master o slave
           — mentre ndrives e' l'ordine di iscrizione. Con il solo slave presente i
           due divergono, e usare ndrives darebbe un disco che si chiama hdb con i
           numeri di hda. Due verita' sulla stessa cosa, e la seconda sbagliata.

           drives e' un array static, quindi il puntatore che il registry conserva
           resta valido: e' il requisito nuovo di M11e, e questo file lo
           soddisfaceva gia' — vedi il commento sopra la sua dichiarazione.

           assert e non un ritorno silenzioso: un fallimento qui significa nome o
           coppia duplicati, cioe' un errore di programmazione e non una condizione
           dell'ambiente. Un disco assente non passa da questa riga, perche'
           ata_identify ha gia' fatto continue. */
        assert(blockdev_register(nomi[d], 3, (uint16_t)(d * 64),
                                 &drives[ndrives]) == 0);

        ndrives++;
    }
}

struct blockdev *ata_drive(int i)
{
    /* Il controllo sul negativo non e' pedanteria, ed e' la stessa nota di
       chardev_at in M8: drives[-1] legge i byte prima dell'array, che in .bss
       sono un'altra variabile. */
    if (i < 0 || i >= ndrives)
        return 0;

    return &drives[i];
}

int ata_drive_count(void)
{
    return ndrives;
}
