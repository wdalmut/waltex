#include "selftest.h"
#include "types.h"
#include "io.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "rtc.h"
#include "keyboard.h"
#include "serial.h"
#include "vga.h"
#include "device.h"
#include "devfs.h"
#include "vfs.h"
#include "ata.h"
#include "blockdev.h"
#include "kprintf.h"

#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25

#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5
#define CRTC_CURSOR_HI 0x0E
#define CRTC_CURSOR_LO 0x0F

static int failures;

static void puts_(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

static void report(const char *name, int ok)
{
    puts_(ok ? "selftest: ok   -- " : "selftest: FAIL -- ");
    puts_(name);
    serial_putc('\n');
    if (!ok)
        failures++;
}

/* Il framebuffer è memoria come tutte le altre: dopo aver scritto un
   carattere possiamo rileggere la cella e controllare cosa c'è davvero. */
static void check_putc(void)
{
    vga_clear();
    vga_putc('X');
    report("vga_putc scrive il carattere in (0,0)",
           (VGA_MEM[0] & 0xFF) == 'X');
    report("vga_putc lascia un attributo non nullo",
           (VGA_MEM[0] >> 8) != 0);
}

static void check_clear(void)
{
    vga_putc('Y');
    vga_clear();
    report("vga_clear azzera i caratteri",
           (VGA_MEM[0] & 0xFF) == ' ' || (VGA_MEM[0] & 0xFF) == 0);
}

static void check_newline(void)
{
    vga_clear();
    vga_putc('A');
    vga_putc('\n');
    vga_putc('B');
    report("newline porta il cursore a inizio riga 1",
           (VGA_MEM[VGA_COLS] & 0xFF) == 'B');
}

/* Riempita l'ultima riga, il contenuto deve salire di una posizione: quello
   che era in riga 1 finisce in riga 0. */
static void check_scroll(void)
{
    int i;

    vga_clear();
    vga_putc('0');            /* riga 0: e' quella che lo scroll butta via */
    vga_putc('\n');
    vga_putc('1');            /* riga 1, colonna 0 */
    vga_putc('Z');            /* riga 1, colonna 1 */
    for (i = 0; i < VGA_ROWS - 1; i++)
        vga_putc('\n');

    report("lo scroll fa salire le righe",
           (VGA_MEM[0] & 0xFF) == '1');

    /* Due celle adiacenti con contenuto diverso: un memcpy rotto che replica
       un byte le renderebbe uguali, e passerebbe il check qui sopra. */
    report("lo scroll sposta un blocco, non riempie",
           (VGA_MEM[1] & 0xFF) == 'Z');

    report("lo scroll svuota l'ultima riga",
           (VGA_MEM[(VGA_ROWS - 1) * VGA_COLS] & 0xFF) == ' ' ||
           (VGA_MEM[(VGA_ROWS - 1) * VGA_COLS] & 0xFF) == 0);
}

/* I registri del cursore del CRTC sono leggibili, non solo scrivibili: su
   hardware muto la rilettura e' l'unica conferma che esista. Verificato che
   QEMU la supporta. */
static uint16_t cursor_hw_pos(void)
{
    uint16_t pos;

    outb(VGA_CRTC_INDEX, CRTC_CURSOR_LO);
    pos = inb(VGA_CRTC_DATA);
    outb(VGA_CRTC_INDEX, CRTC_CURSOR_HI);
    pos |= (uint16_t)inb(VGA_CRTC_DATA) << 8;

    return pos;
}

static void check_cursor(void)
{
    int i;

    vga_clear();
    report("vga_clear porta il cursore hardware a (0,0)",
           cursor_hw_pos() == 0);

    vga_putc('A');
    vga_putc('B');
    vga_putc('C');
    report("il cursore hardware segue la scrittura",
           cursor_hw_pos() == 3);

    vga_putc('\n');
    report("il cursore hardware segue il newline",
           cursor_hw_pos() == VGA_COLS);

    /* Riempita l'ultima riga, lo scroll riporta la posizione a inizio
       ultima riga: anche il cursore hardware deve seguirla. */
    vga_clear();
    for (i = 0; i < VGA_ROWS; i++)
        vga_putc('\n');
    report("dopo lo scroll il cursore hardware e' a inizio ultima riga",
           cursor_hw_pos() == (VGA_ROWS - 1) * VGA_COLS);
}

static void check_color(void)
{
    int i;
    uint16_t atteso = (VGA_RED << 4) | VGA_WHITE;

    vga_set_color(VGA_WHITE, VGA_RED);
    vga_clear();
    vga_putc('E');

    report("vga_putc usa il colore corrente",
           (VGA_MEM[0] >> 8) == atteso);

    report("vga_clear riempie di spazi con il colore corrente",
           (VGA_MEM[1] & 0xFF) == ' ' && (VGA_MEM[1] >> 8) == atteso);

    /* La riga svuotata dallo scroll e' l'altro posto dove il colore corrente
       va applicato, ed e' quello che si dimentica. */
    vga_clear();
    for (i = 0; i < VGA_ROWS; i++)
        vga_putc('\n');
    report("la riga svuotata dallo scroll usa il colore corrente",
           (VGA_MEM[(VGA_ROWS - 1) * VGA_COLS] >> 8) == atteso);

    /* ...e lo spazio, come vga_clear: una riga svuotata dallo scroll e una
       svuotata da clear devono contenere la stessa cosa. */
    report("la riga svuotata dallo scroll contiene spazi",
           (VGA_MEM[(VGA_ROWS - 1) * VGA_COLS] & 0xFF) == ' ');

    /* Uno sfondo fuori dai 16 colori non deve accendere il bit 7, che non e'
       intensita' ma lampeggio. */
    vga_set_color(VGA_WHITE, VGA_YELLOW);
    vga_clear();
    vga_putc('E');
    report("uno sfondo fuori intervallo non accende il lampeggio",
           (VGA_MEM[0] & 0x8000) == 0);

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

/* --- M7: il backspace -------------------------------------------------------
   '\b' vale 8, quindi non passa ne' per il ramo c >= 32 ne' per quello del
   newline: gli serve un caso suo in vga_putc.

   Questo e' l'unico posto in cui la cosa si puo' verificare. Sulla seriale il
   backspace funziona comunque, perche' lo interpreta il terminale all'altro
   capo; sulla VGA il framebuffer non interpreta niente, e se vga_putc non
   sposta il cursore non lo sposta nessuno.

   Cancellare richiede TRE caratteri — '\b', spazio, '\b' — e qui si verifica
   solo il primo, cioe' che il cursore arretri. Lo spazio lo manda l'editor di
   riga, ed e' coperto dai test host. */

static void check_backspace(void)
{
    vga_clear();

    /* Il caso limite, e il solo che produce un sintomo bizzarro: il cursore e'
       un uint16_t, quindi decrementarlo a zero da' 65535. Il controllo di
       scroll due righe sotto scatta, e il risultato e' lo schermo che scorre
       quando premi backspace di troppo su una riga vuota. */
    vga_putc('\b');
    report("il backspace a inizio schermo lascia il cursore a zero",
           cursor_hw_pos() == 0);

    vga_clear();
    vga_putc('X');
    vga_putc('\b');
    report("il backspace riporta il cursore hardware indietro di uno",
           cursor_hw_pos() == 0);

    /* Il backspace sposta e non cancella: la cella si svuota quando qualcuno ci
       scrive sopra. Qui ci scriviamo 'Y', e se il cursore fosse rimasto a 1 la
       cella 0 conterrebbe ancora 'X'. */
    vga_putc('Y');
    report("dopo il backspace si scrive sopra il carattere cancellato",
           (VGA_MEM[0] & 0xFF) == 'Y');
}

/* --- M8: il registro dei dispositivi ----------------------------------------
   Il registro in sé è coperto dai test host, che sono istantanei. Qui restano le
   cose che esistono solo dentro la VM: che i driver si siano iscritti davvero,
   con i numeri giusti e le capacità giuste. */

static void check_device_serial(void)
{
    struct device *d = device_find("ttyS0");

    report("ttyS0 e' iscritto nel registro", d != 0);

    if (d == 0)
        return;

    /* 4:64 sono i numeri veri di /dev/ttyS0 su Linux. Non ci obbliga nessuno,
       ma costa zero e sta nella stessa direzione del vincolo POSIX di M14. */
    report("ttyS0 ha i numeri 4:64", d->major == 4 && d->minor == 64);

    /* La capacita' si legge dalla nullita' dei puntatori: write c'e', read no.
       E' la convenzione che permettera' a devs di stampare -w senza un campo
       apposta. */
    report("ttyS0 sa scrivere e non sa leggere",
           d->write != 0 && d->read == 0);

    /* Esercita l'adattatore sul caso limite: zero byte scrive nulla e ritorna
       zero. Un ciclo scritto come do...while qui scriverebbe un byte.

       Il caso n > 0 non si verifica da qui: la seriale non si rilegge, quindi
       non c'e' modo di confermare dall'interno che i byte siano usciti. Lo
       stesso ciclo su VGA invece si verifica rileggendo il framebuffer, ed e'
       il controllo che copre la forma di entrambi. */
    report("la write di ttyS0 con n=0 non scrive e ritorna 0",
           d->write(d, "", 0) == 0);
}

static void check_device_console(void)
{
    struct device *d = device_find("console");

    report("console e' iscritto nel registro", d != 0);

    if (d == 0)
        return;

    report("console ha i numeri 5:1", d->major == 5 && d->minor == 1);
    report("console sa scrivere e non sa leggere",
           d->write != 0 && d->read == 0);

    /* Qui l'adattatore si verifica davvero, e non solo nelle sue proprieta': il
       framebuffer si rilegge. E' il solo dei tre dispositivi su cui si possa
       fare, perche' la seriale non si rilegge e la tastiera non si scrive —
       quindi questo controllo copre la FORMA del ciclo di entrambi gli
       adattatori di scrittura, che sono identici. */
    vga_clear();
    report("la write di console ritorna il numero di byte",
           d->write(d, "AB", 2) == 2);
    report("la write di console mette i byte nel framebuffer",
           (VGA_MEM[0] & 0xFF) == 'A' && (VGA_MEM[1] & 0xFF) == 'B');
    vga_clear();
}

static void check_device_kbd(void)
{
    struct device *d = device_find("kbd");
    char buf[8];
    int i, r;

    report("kbd e' iscritto nel registro", d != 0);

    if (d == 0)
        return;

    /* 13:64 sono i numeri di /dev/input/event0 su Linux. */
    report("kbd ha i numeri 13:64", d->major == 13 && d->minor == 64);
    report("kbd sa leggere e non sa scrivere",
           d->read != 0 && d->write == 0);

    /* Nessuno ha digitato: la read deve ritornare ZERO, che significa "adesso
       non c'e' niente" e NON "fine del file". E' il contratto su cui in M9
       cat /dev/kbd fara' spin.

       Nota sull'ordine: questa chiamata legge il ring buffer della tastiera, che
       ammette un solo consumatore. Qui e' al sicuro perche' i self-check girano
       PRIMA di task_create(shell_task), quindi la shell non esiste ancora — ma
       e' al sicuro per l'ordine delle righe in kmain, non per costruzione.
       Spostare selftest_run dopo la creazione della shell farebbe rubare
       caratteri. */
    for (i = 0; i < 8; i++)
        buf[i] = (char)0xAA;

    r = d->read(d, buf, sizeof(buf));

    report("la read di kbd a buffer vuoto ritorna 0", r == 0);

    /* E non deve aver scritto niente: zero byte letti, zero byte toccati. */
    report("la read di kbd a buffer vuoto non tocca la destinazione",
           buf[0] == (char)0xAA && buf[7] == (char)0xAA);
}

/* Il conteggio, che e' il controllo indiretto sull'ordine di device_init in
   kmain: chiamata dopo i driver, azzererebbe il contatore e i tre dispositivi
   diventerebbero invisibili pur essendo nell'array. */
static void check_device_count(void)
{
    report("i tre driver si sono iscritti", device_count() == 3);
}

/* --- M9b: devfs, l'albero vero ------------------------------------------------

   I 75 test host di M9a girano su un albero FINTO di sei nodi e verificano la
   logica del VFS. Questi girano sull'albero vero — quello che devfs genera dal
   registro che i driver hanno riempito dentro la VM — ed e' l'unico posto dove i
   cinque livelli stanno uno sopra l'altro:

     vfs_resolve  →  devfs lookup  →  ino_devices[i].priv  →  struct device

   Vengono DOPO i check_device_*: se il registro fosse vuoto, /dev sarebbe vuota
   e questi fallirebbero tutti senza dire che la causa sta un piano piu' sotto. */

static void check_devfs_root(void)
{
    struct inode *root = devfs_root();

    report("devfs_root non e' nullo dopo devfs_init", root != 0);

    if (root == 0)
        return;

    /* Il tipo si controlla a parte, e non e' pedanteria: una radice mai
       riempita e' tutta zeri, quindi il puntatore e' valido e type vale
       INODE_NONE. Il primo controllo passerebbe e ogni resolve fallirebbe. */
    report("la radice e' una directory", root->type == INODE_DIR);
}

static void check_devfs_resolve(void)
{
    struct inode *dev, *kbd, *console, *niente;
    struct device *d;

    report("vfs_resolve(\"/dev\") riesce e da' una directory",
           vfs_resolve("/dev", &dev) == 0 && dev->type == INODE_DIR);

    report("vfs_resolve(\"/dev/kbd\") riesce",
           vfs_resolve("/dev/kbd", &kbd) == 0);

    if (vfs_resolve("/dev/kbd", &kbd) != 0)
        return;

    report("/dev/kbd e' un dispositivo a caratteri",
           kbd->type == INODE_CHARDEV);

    /* Confrontati con il REGISTRO, non con 13:64 scritti qui: che i numeri
       giusti siano 13:64 lo verifica gia' check_device_kbd. Quello che questo
       controlla e' che devfs li abbia COPIATI, che e' una cosa diversa e puo'
       rompersi da sola. */
    d = device_find("kbd");
    report("/dev/kbd porta i numeri del suo dispositivo",
           d != 0 && kbd->major == d->major && kbd->minor == d->minor);

    report("vfs_resolve(\"/dev/console\") da' un chardev",
           vfs_resolve("/dev/console", &console) == 0 &&
           console->type == INODE_CHARDEV);

    /* Il controllo negativo, e vale gli altri messi insieme: una lookup che
       sbaglia il valore di ritorno sull'insuccesso — 1 invece di -1 — fa
       credere a vfs_resolve di aver trovato qualcosa, e da li' in poi cammina
       su un puntatore mai inizializzato. Nessun controllo positivo lo vede. */
    report("vfs_resolve(\"/dev/nonesiste\") fallisce",
           vfs_resolve("/dev/nonesiste", &niente) < 0);
}

static void check_devfs_readdir(void)
{
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int fd, idx, n = 0;

    fd = vfs_open("/dev", O_RDONLY);

    report("vfs_open(\"/dev\") riesce", fd >= 0);

    if (fd < 0)
        return;

    /* Il tetto sul ciclo non e' prudenza generica: se readdir ritornasse
       sempre 1 — dimenticando il ramo di fine elenco — un ciclo senza limite
       resterebbe qui per sempre, e un self-check che non termina non e' un
       FAIL, e' un kernel che non booota. Con il tetto il conteggio esce
       sbagliato e il controllo fallisce dicendo cosa. */
    for (idx = 0; idx < MAX_DEVICES + 1; idx++) {
        if (vfs_readdir(fd, idx, nome, &ino) != 1)
            break;
        n++;
    }

    report("readdir su /dev elenca un inode per dispositivo",
           n == device_count());

    vfs_close(fd);
}

static void check_devfs_read(void)
{
    char buf[8];
    int fd, i, r;

    fd = vfs_open("/dev/kbd", O_RDONLY);

    report("vfs_open(\"/dev/kbd\") riesce", fd >= 0);

    if (fd < 0)
        return;

    for (i = 0; i < 8; i++)
        buf[i] = (char)0x5A;

    r = vfs_read(fd, buf, sizeof(buf));

    /* La traversata completa: vfs_read, chardev_read, kbd_dev_read,
       keyboard_getchar, ring buffer. Lo zero significa "adesso non c'e'
       niente" e non "fine del file" — nessuno ha ancora digitato, e la shell
       non esiste. E' il contratto su cui cat /dev/kbd fa spin.

       Vale la nota di ordine di check_device_kbd: si legge il ring buffer, che
       ammette un consumatore solo, ed e' al sicuro perche' selftest_run gira
       prima di task_create(shell_task). */
    report("vfs_read su /dev/kbd a ring vuoto da' 0", r == 0);
    report("e non ha toccato il buffer del chiamante",
           buf[0] == (char)0x5A && buf[7] == (char)0x5A);

    report("vfs_close del descrittore riesce", vfs_close(fd) == 0);
}

/* --- M10: il disco -----------------------------------------------------------

   Il riferimento di questi controlli non e' il kernel: e' build/disk.img, che
   tools/mkdisk.sh ha costruito PRIMA che la VM partisse. E' la stessa
   disciplina dell'orologio CMOS in M4 — un disco non puo' verificare se stesso,
   e un driver che leggesse e riscrivesse solo la propria spazzatura passerebbe
   qualunque controllo interno.

   Il buffer da 512 byte e' la variabile locale piu' grande del progetto, un
   ottavo dello stack di un task. E' fuori dalle funzioni, static, perche' i
   self-check girano sullo stack di kmain e non c'e' ragione di rischiare. */

static uint8_t settore[SECTOR_SIZE];

static void check_ata_presente(void)
{
    /* Due da M11a, ed e' il capovolgimento di un controllo di M10: fino a ieri
       lo slave non c'era. Adesso c'e', e con lui priv viene esercitato per la
       prima volta — le due struct blockdev hanno lo STESSO puntatore a read, e
       solo priv dice quale disco. */
    report("due dischi sul canale primario", ata_drive_count() == 2);
    report("ata_drive(0) esiste", ata_drive(0) != 0);
    report("ata_drive(1) esiste", ata_drive(1) != 0);

    /* E sono due dispositivi DISTINTI, non lo stesso restituito due volte:
       nomi diversi, capacita' diverse — 2048 settori contro 512 — e priv
       diversi. Senza questo, un ata_init che iscrivesse due volte il master
       passerebbe ogni altro controllo. */
    report("i due dischi sono distinti",
           ata_drive(0) != 0 && ata_drive(1) != 0 &&
           ata_drive(0)->priv != ata_drive(1)->priv &&
           ata_drive(1)->nsectors == 512);

    /* 2048 e' il numero che mkdisk.sh ha SCELTO, non uno qualunque: questo
       controllo prende insieme IDENTIFY e la costruzione dell'immagine, e si
       accorge in particolare della word 60-61 letta come una word sola, che
       troncherebbe la capacita' a 65535. */
    report("la capacita' e' quella dell'immagine",
           ata_drive(0) != 0 && ata_drive(0)->nsectors == 2048);
}

static void check_ata_read(void)
{
    struct blockdev *b = ata_drive(0);
    int i, r, uguali;

    if (b == 0)
        return;

    /* Il settore 0: la firma. Un controllo breve, ma indipendente dal pattern
       e quindi utile a distinguere "legge il settore sbagliato" da "legge
       male". */
    r = b->read(b, 0, settore, 1);
    report("la lettura del settore 0 riesce", r == 1);
    report("il settore 0 comincia con la firma dell'immagine",
           r == 1 && settore[0] == 'w' && settore[1] == 'a' &&
           settore[2] == 'l' && settore[3] == 't' && settore[4] == 'e' &&
           settore[5] == 'x');

    /* IL controllo di M10. Il pattern l'ha generato un programma che non e' il
       nostro, e si confrontano tutti e 512 i byte: e' l'unico controllo che
       puo' accorgersi di un driver che legge il settore sbagliato in modo
       COERENTE, perche' tutti gli altri sono d'accordo con se stessi.

       Il pattern e' (i * 7 + 3) & 0xFF, deterministico e non costante: un
       memset di un valore solo passerebbe anche leggendo mezzo settore. */
    r = b->read(b, 1, settore, 1);
    uguali = (r == 1);

    for (i = 0; i < SECTOR_SIZE && uguali; i++)
        if (settore[i] != (uint8_t)((i * 7 + 3) & 0xFF))
            uguali = 0;

    report("il settore 1 e' identico al pattern scritto dall'host", uguali);

    /* Oltre la fine del disco. Il caso che ata_range_ok esiste per prendere, e
       la ragione per cui il confronto e' per sottrazione invece che
       lba + count: con la somma, un lba vicino a 2^32 la farebbe girare e il
       controllo passerebbe. */
    report("leggere oltre l'ultimo settore fallisce",
           b->read(b, 2048, settore, 1) < 0);
    report("leggere a cavallo della fine fallisce",
           b->read(b, 2047, settore, 2) < 0);

    /* Zero settori: la classe di bug di M8, dove kbd_dev_read con n == 0
       consumava un carattere prima di controllare. Qui il danno sarebbe
       peggiore, perche' zero nel registro del conteggio significa 256. */
    settore[0] = 0x5A;
    report("leggere zero settori da' 0 e non tocca il buffer",
           b->read(b, 0, settore, 0) == 0 && settore[0] == 0x5A);
}

static void check_ata_write(void)
{
    struct blockdev *b = ata_drive(0);
    int i, r, uguali;

    if (b == 0)
        return;

    /* Il settore 2 e' lo scratch dell'immagine, azzerato da mkdisk.sh. Ci si
       scrive un pattern DIVERSO da quello del settore 1 — se fossero uguali,
       una write che non fa niente passerebbe grazie a una read che sbaglia
       settore. */
    for (i = 0; i < SECTOR_SIZE; i++)
        settore[i] = (uint8_t)((i * 11 + 5) & 0xFF);

    report("la scrittura del settore 2 riesce", b->write(b, 2, settore, 1) == 1);

    /* Si azzera il buffer prima di rileggere: senza, un read che non facesse
       niente lascerebbe in memoria quello che ci aveva messo la write, e il
       confronto passerebbe verificando la RAM invece del disco. */
    for (i = 0; i < SECTOR_SIZE; i++)
        settore[i] = 0;

    r = b->read(b, 2, settore, 1);
    uguali = (r == 1);

    for (i = 0; i < SECTOR_SIZE && uguali; i++)
        if (settore[i] != (uint8_t)((i * 11 + 5) & 0xFF))
            uguali = 0;

    report("il settore 2 riletto e' quello che si e' scritto", uguali);

    /* Questo controllo dice che il kernel CREDE di aver scritto. Che sul file
       ci sia davvero lo dice solo tests/disk.sh, che rilegge build/disk.img da
       fuori la VM: e' il solo controllo del progetto in cui la verifica avviene
       fuori dalla macchina che ha fatto il lavoro. */
}

/* --- M11a: minix -------------------------------------------------------------

   Il grosso di M11a lo provano i 53 controlli host, che girano sulla STESSA
   immagine e in millisecondi. Qui restano le cose che esistono solo dentro la
   VM: che il filesystem sia montato sul disco vero attraverso il driver ATA, e
   che l'innesto di /dev regga — cioe' i cinque livelli impilati.

   Non si duplica quello che l'host prova gia' meglio. */

static void check_minix(void)
{
    struct inode *ino;
    char buf[64];
    int fd, r;

    /* La radice viene dal disco, non da devfs: e' l'inode 1 di minix. Se il
       mount fosse fallito, kmain avrebbe ripiegato su devfs e qui troveremmo
       l'inode 1 di devfs — che pero' non ha un /etc. */
    report("vfs_resolve(\"/etc\") riesce ed e' una directory",
           vfs_resolve("/etc", &ino) == 0 && ino->type == INODE_DIR);

    report("vfs_resolve(\"/etc/motd\") riesce",
           vfs_resolve("/etc/motd", &ino) == 0);

    /* 35 byte: il numero che mkminix.sh ha scritto, misurato con od
       sull'immagine. Prende insieme il mount, la tabella degli inode e
       l'aritmetica dei settori. */
    report("/etc/motd misura 35 byte",
           vfs_resolve("/etc/motd", &ino) == 0 && ino->size == 35);

    /* Il file che sfonda le sette zone dirette. Che la size sia giusta non
       prova l'indiretto — quello lo provano i test host leggendolo tutto — ma
       conferma che l'inode arriva dal disco vero e non da un albero finto. */
    report("/enorme.txt misura 20000 byte",
           vfs_resolve("/enorme.txt", &ino) == 0 && ino->size == 20000);

    /* L'INNESTO, ed e' il controllo che tiene insieme le due milestone: /dev
       non esiste sull'immagine minix — mkminix.sh non lo crea — quindi se si
       risolve e' perche' minix_lookup consulta l'innesto prima del disco. */
    report("/dev esiste attraverso l'innesto",
           vfs_resolve("/dev", &ino) == 0 && ino->type == INODE_DIR);

    report("/dev/kbd si risolve ancora, con la radice su minix",
           vfs_resolve("/dev/kbd", &ino) == 0 && ino->type == INODE_CHARDEV);

    /* E la catena intera, che e' il punto di M11a: aprire un file su disco e
       leggerlo attraverso VFS, minixfs, blockdev e ATA. Quattro strati sotto la
       stessa vfs_read che in M9b leggeva la tastiera. */
    fd = vfs_open("/etc/motd", O_RDONLY);

    report("vfs_open(\"/etc/motd\") riesce", fd >= 0);

    if (fd < 0)
        return;

    r = vfs_read(fd, buf, sizeof(buf));

    report("vfs_read legge i 35 byte del file", r == 35);
    report("e cominciano con \"waltex\"",
           r == 35 && buf[0] == 'w' && buf[1] == 'a' && buf[2] == 'l' &&
           buf[3] == 't' && buf[4] == 'e' && buf[5] == 'x');

    /* La seconda lettura da' 0: la posizione ha raggiunto size. E' la
       convenzione di M8, e su un file regolare — a differenza di /dev/kbd —
       lo zero significa davvero "finito". */
    report("la seconda lettura da' 0, cioe' fine del file",
           vfs_read(fd, buf, sizeof(buf)) == 0);

    report("vfs_close riesce", vfs_close(fd) == 0);
}

/* --- M2: la GDT ---------------------------------------------------------
   Una GDT corretta non produce nessun effetto visibile, perche' sostituisce
   quella del bootloader con una funzionalmente identica. Quindi non chiediamo
   "il kernel e' sopravvissuto?" ma "quale tabella sta usando la CPU?", e ne
   ispezioniamo i byte. */

/* Il registro GDTR non e' leggibile direttamente: sgdt lo scrive in memoria,
   in un blocco di 6 byte con lo stesso formato che lgdt si aspetta. */
struct gdtr_image {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static void read_gdtr(struct gdtr_image *out)
{
    __asm__ volatile ("sgdt %0" : "=m"(*out));
}

static uint16_t read_cs(void)
{
    uint16_t v;
    __asm__ volatile ("movw %%cs, %0" : "=r"(v));
    return v;
}

static uint16_t read_ds(void)
{
    uint16_t v;
    __asm__ volatile ("movw %%ds, %0" : "=r"(v));
    return v;
}

static uint16_t read_ss(void)
{
    uint16_t v;
    __asm__ volatile ("movw %%ss, %0" : "=r"(v));
    return v;
}

/* I byte attesi di un descrittore piatto ring 0, base 0 e limite 4 GiB.
   L'unica differenza fra codice e dati e' il byte di access.

   Il bit 0 dell'access byte e' "accessed": lo mette la CPU quando il
   descrittore viene caricato in un registro di segmento, quindi non e' sotto
   il controllo di chi scrive la tabella. Confrontarlo renderebbe il test
   dipendente dal momento in cui gira e dall'emulazione. Lo ignoriamo. */
#define ACCESS_A 0x01

static int descrittore_piatto_ok(const uint8_t *d, uint8_t access)
{
    return d[0] == 0xFF &&   /* limite  0-7   */
           d[1] == 0xFF &&   /* limite  8-15  */
           d[2] == 0x00 &&   /* base    0-7   */
           d[3] == 0x00 &&   /* base    8-15  */
           d[4] == 0x00 &&   /* base   16-23  */
           (d[5] | ACCESS_A) == (access | ACCESS_A) &&
           d[6] == 0xCF &&   /* nibble alto: flag; basso: limite 16-19 */
           d[7] == 0x00;     /* base   24-31  */
}

static void check_gdt(void)
{
    struct gdtr_image gdtr;
    const uint8_t *tabella;
    int i;
    int null_azzerato = 1;

    read_gdtr(&gdtr);
    tabella = (const uint8_t *)gdtr.base;

    /* Tre descrittori da 8 byte: il limite e' la dimensione meno uno. */
    report("la GDT caricata ha tre descrittori",
           gdtr.limit == 3 * 8 - 1);

    /* Il descrittore 0 deve essere tutto zero: la CPU lo esige, e un
       selettore che lo referenzia e' un errore per costruzione. */
    for (i = 0; i < 8; i++)
        if (tabella[i] != 0)
            null_azzerato = 0;
    report("il descrittore null e' azzerato", null_azzerato);

    report("il descrittore di codice e' piatto ring 0",
           descrittore_piatto_ok(tabella + 8, 0x9A));

    report("il descrittore di dati e' piatto ring 0",
           descrittore_piatto_ok(tabella + 16, 0x92));

    /* Non basta che la tabella sia giusta: i registri di segmento tengono una
       copia nascosta del descrittore e vanno riscritti perche' la rileggano. */
    report("cs usa il selettore di codice", read_cs() == GDT_SEL_CODE);
    report("ds usa il selettore di dati",   read_ds() == GDT_SEL_DATA);
    report("ss usa il selettore di dati",   read_ss() == GDT_SEL_DATA);
}

/* --- M3: IDT, dispatch, PIC ---------------------------------------------- */

/* Come il GDTR, l'IDTR si legge solo scrivendolo in memoria. */
struct idtr_image {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static void read_idtr(struct idtr_image *out)
{
    __asm__ volatile ("sidt %0" : "=m"(*out));
}

/* Gli stub sono simboli globali: possiamo confrontare l'indirizzo scritto nel
   gate con quello vero, invece di fidarci. */
extern void isr0(void);
extern void irq0(void);

/* L'offset in un gate e' spezzato in due pezzi, come la base nella GDT. */
static uint32_t gate_offset(const uint8_t *g)
{
    return  (uint32_t)g[0]        |
           ((uint32_t)g[1] <<  8) |
           ((uint32_t)g[6] << 16) |
           ((uint32_t)g[7] << 24);
}

static uint16_t gate_selector(const uint8_t *g)
{
    return (uint16_t)g[2] | ((uint16_t)g[3] << 8);
}

static void check_idt(void)
{
    struct idtr_image idtr;
    const uint8_t *tab;

    read_idtr(&idtr);
    tab = (const uint8_t *)idtr.base;

    report("l'IDT caricata ha 256 gate",
           idtr.limit == IDT_ENTRIES * 8 - 1);

    report("il gate 0 punta a isr0",
           gate_offset(tab) == (uint32_t)isr0);

    report("il gate 32 punta a irq0",
           gate_offset(tab + IRQ_BASE * 8) == (uint32_t)irq0);

    report("il gate 0 usa il selettore di codice",
           gate_selector(tab) == GDT_SEL_CODE);

    /* 0x8E: presente, DPL 0, descrittore di sistema, interrupt gate a 32 bit.
       Il byte 4 deve essere zero, e' riservato. */
    report("il gate 0 e' un interrupt gate a 32 bit, DPL 0",
           tab[5] == 0x8E && tab[4] == 0x00);
}

/* Il PIC risponde in lettura sulla porta dati con la maschera corrente:
   un bit a 1 significa linea disabilitata. */
static void check_pic(void)
{
    /* Non si confronta il byte intero: i driver smascherano legittimamente la
       propria linea, e da M4 il timer accende il bit 0. Un check che pretenda
       0xFB diventerebbe rosso a ogni driver aggiunto — e la tentazione
       sarebbe aggiornare la costante invece di chiedersi cosa si sta
       verificando. Qui restano solo le due proprieta' che devono valere
       sempre. */
    report("la cascata sul master resta smascherata",
           (inb(PIC_MASTER_DATA) & 0x04) == 0);

    /* Nessun dispositivo sullo slave, per ora: se un bit si accendesse
       vorrebbe dire che qualcuno ha smascherato una linea che nessuno serve. */
    report("lo slave ha tutto mascherato",
           inb(PIC_SLAVE_DATA) == 0xFF);
}

/* L'unico modo di provare la catena completa senza hardware: un interrupt
   software. Il breakpoint e' l'eccezione giusta perche' e' deliberata e
   riprende dall'istruzione successiva. */
static volatile int bp_calls;
static volatile uint32_t bp_vec, bp_cs, bp_err;

static void bp_handler(struct regs *r)
{
    bp_calls++;
    bp_vec = r->vec;
    bp_cs  = r->cs;
    bp_err = r->err;
}

static void check_dispatch(void)
{
    exception_register(EXC_BREAKPOINT, bp_handler);

    bp_calls = 0;
    __asm__ volatile ("int $3");

    /* Se siamo arrivati qui, iret ha funzionato: la CPU ha ripreso
       l'esecuzione dopo l'int invece di ripartire. */
    report("int $3 viene gestito e l'esecuzione riprende", bp_calls == 1);
    report("il dispatcher riceve il vettore giusto", bp_vec == EXC_BREAKPOINT);
    report("struct regs riporta cs corretto", bp_cs == GDT_SEL_CODE);
    report("il codice d'errore fittizio e' zero", bp_err == 0);

    exception_register(EXC_BREAKPOINT, 0);
}

/* --- M4: il timer ---------------------------------------------------------
   Qui non si verifica una tabella ma un comportamento nel tempo, e serve un
   riferimento esterno: il timer non puo' misurare se stesso. */

static void check_timer(void)
{
    uint32_t t0, t1, misurati;

    /* Il driver deve aver smascherato la propria linea, altrimenti il chip
       genera interrupt che il PIC non presenta alla CPU. */
    report("timer_init smaschera l'IRQ 0",
           (inb(PIC_MASTER_DATA) & 0x01) == 0);

    /* Che i tick avanzino dice che l'interrupt arriva e che l'EOI e' corretto:
       con un EOI mancante il contatore si fermerebbe a 1. */
    t0 = timer_ticks();
    if (!rtc_wait_second_change()) {
        report("l'RTC risponde (serve come riferimento)", 0);
        return;
    }
    report("i tick avanzano dopo la sti", timer_ticks() > t0);

    /* La misura vera. Partiti da un confine di secondo appena attraversato,
       si contano i tick fino al confine successivo. */
    t0 = timer_ticks();
    if (!rtc_wait_second_change()) {
        report("l'RTC risponde per la seconda misura", 0);
        return;
    }
    t1 = timer_ticks();
    misurati = t1 - t0;

    /* 100 Hz nominali. La tolleranza copre il troncamento del divisore
       (100.007 Hz reali) e il fatto che i due confini di secondo non cadono
       esattamente dove li campioniamo. */
    report("la frequenza misurata e' 100 Hz entro la tolleranza",
           misurati >= 95 && misurati <= 105);

    kprintf("selftest:      (tick contati in un secondo: %d)\n",
            (int)misurati);

    /* Un contatore che va all'indietro significherebbe letture non atomiche o
       un gestore che lo azzera. */
    t0 = timer_ticks();
    t1 = timer_ticks();
    report("il contatore non torna indietro", t1 >= t0);
}

/* --- M5: la tastiera --------------------------------------------------------
   La decodifica e il buffer sono coperti dai test host, che sono istantanei.
   Qui restano le due cose che esistono solo dentro la VM. */

static void check_keyboard(void)
{
    report("keyboard_init smaschera l'IRQ 1",
           (inb(PIC_MASTER_DATA) & 0x02) == 0);

    /* Nessuno ha ancora digitato: il buffer deve essere vuoto. Se restituisse
       un carattere, il gestore avrebbe accodato spazzatura all'avvio. */
    report("nessun carattere in attesa all'avvio",
           keyboard_getchar() == -1);
}

int selftest_run(void)
{
    failures = 0;

    check_gdt();
    check_idt();
    check_pic();
    check_dispatch();
    check_timer();
    check_keyboard();
    check_putc();
    check_clear();
    check_newline();
    check_scroll();
    check_cursor();
    check_color();
    check_backspace();
    check_device_serial();
    check_device_console();
    check_device_kbd();
    check_device_count();
    check_devfs_root();
    check_devfs_resolve();
    check_devfs_readdir();
    check_devfs_read();
    check_ata_presente();
    check_ata_read();
    check_ata_write();
    check_minix();

    vga_clear();
    return failures;
}
