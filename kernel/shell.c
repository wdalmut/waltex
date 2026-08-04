#include "shell.h"
#include "types.h"
#include "memory.h"
#include "lineedit.h"
#include "kprintf.h"
#include "keyboard.h"
#include "timer.h"
#include "task.h"
#include "vga.h"
#include "demo.h"
#include "panic.h"
#include "dev.h"
#include "devio.h"
#include "vfs.h"
#include "blockdev.h"
#include "ata.h"

#define HEXA "0123456789abcdef"

/* Uno solo, e static: la disciplina del progetto e' capacita' fissa, e in M14
   lo stack di shell_task diventera' quello di un processo utente. */
static struct lineedit le;

/* L'unico pezzo di questo file che esiste solo per far combaciare due tipi.
   lineedit vuole void (*)(char); kprintf e' variadica e non calza. vga_putc e
   serial_putc calzerebbero, ma manderebbero l'eco a UN solo posto — e i test
   leggono la seriale. */
static void shell_echo(char c)
{
    kprintf("%c", c);
}

/* Dichiarate qui e definite sotto la tabella, perche' shell_help deve leggere
   la tabella e la tabella deve nominare shell_help: uno dei due va anticipato. */
static void shell_help(int argc, char **argv);
static void shell_echo_cmd(int argc, char **argv);
static void shell_ticks(int argc, char **argv);
static void shell_ps(int argc, char **argv);
static void shell_peek(int argc, char **argv);
static void shell_spin(int argc, char **argv);
static void shell_clear(int argc, char **argv);
static void shell_panic(int argc, char **argv);
static void shell_devs(int argc, char **argv);
static void shell_ls(int argc, char **argv);
static void shell_cat(int argc, char **argv);
static void hexdump(const volatile uint8_t *p, uint32_t n, uint32_t base);
static void shell_lsblk(int argc, char **argv);
static void shell_rdsect(int argc, char **argv);
static void shell_wrsect(int argc, char **argv);
static void shell_mkdir(int argc, char **argv);
static void shell_write(int argc, char **argv);

/* const, non solo static: la tabella non cambia mai, e il const la sposta in
   .rodata invece che in .data. Con il bilancio della memoria che si legge a
   tempo di link, e' gratis e si vede.

   Nessuna voce terminatrice a zero: la tabella e' static in questo file e chi
   la percorre sta qui, quindi il numero lo calcola il compilatore. Un
   terminatore aggiungerebbe un modo di rompersi — dimenticarlo — senza
   aggiungere niente. */
static const struct shell_cmd table[] = {
    { "help",     shell_help,     "elenca i comandi" },
    { "echo",     shell_echo_cmd, "stampa i suoi argomenti" },
    { "ticks",    shell_ticks,    "tick del timer dal boot" },
    { "ps",       shell_ps,       "stato della tabella dei task" },
    { "peek",     shell_peek,     "peek <indirizzo> [n] - dump, entrambi in esadecimale" },
    { "spin",     shell_spin,     "avvia i due task di prova rumorosi" },
    { "clear",    shell_clear,    "pulisce lo schermo" },
    { "panic",    shell_panic,    "provoca un panic deliberato" },
    { "devs",     shell_devs,     "elenca i device registrati" },
    { "ls",       shell_ls,       "naviga il filesystem" },
    { "cat",      shell_cat,      "cat <path> [n] - mostra un file, al massimo n byte" },
    { "lsblk",    shell_lsblk,    "elenca i dischi con la loro capacita'" },
    { "rdsect",   shell_rdsect,   "rdsect [disco] <settore> [n] - dump, in decimale" },
    { "wrsect",   shell_wrsect,   "wrsect [disco] <settore> <hex> - riempie il settore ripetendo il pattern" },
    { "mkdir",    shell_mkdir,    "mkdir <path> - crea una directory" },
    { "write",    shell_write,    "write <path> <testo...> - crea o SOVRASCRIVE un file" }
};

#define NCMDS ((int)(sizeof(table) / sizeof(table[0])))

int shell_split(char *line, char **argv, int max)
{
    int c = 0;
    char *l = line;

    for (;;) {
        while (*l == ' ')
            ++l;

        /* Finita la riga, oppure argv e' pieno. Il secondo controllo copre
           anche max == 0 senza un caso a parte. */
        if (*l == '\0' || c >= max)
            return c;

        argv[c] = l;
        ++c;

        while (*l != ' ' && *l != '\0')
            ++l;

        /* Se la parola finisce con il terminatore della riga, non c'e' niente
           da chiudere e non c'e' un dopo: si esce senza scrivere. E' cio' che
           evita di troncare la riga quando il ciclo si ferma per argv pieno. */
        if (*l == '\0')
            return c;

        /* Qui *l e' uno spazio, e diventa il NUL che chiude questa parola. Si
           scrive solo dove uno spazio c'era davvero. */
        *l = '\0';
        ++l;
    }
}

int shell_parse_hex(const char *s, uint32_t *out)
{
    uint32_t val = 0;
    int cifre = 0;
    int pos;
    
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s != '\0') {
        /* strpos su HEXA fa due lavori in uno: da' il valore della cifra, e
           tornando -1 dice che cifra non e'.

           Qui torna utile il contratto fissato in memory.h — il terminatore NON
           e' cercabile: se strpos lo trovasse, il NUL entrerebbe come cifra di
           valore 16. */
        pos = strpos(HEXA, tolower(*s));
        if (pos < 0)
            return 0;

        /* Il tetto si controlla PRIMA di spostare, non dopo. Nove cifre non
           stanno in 32 bit, e uno shift in piu' butterebbe via la cifra piu'
           significativa in silenzio: per un comando come peek e' il peggiore
           dei due esiti, perche' leggeresti il posto sbagliato convinto di
           leggere quello giusto. */
        if (cifre == 8)
            return 0;

        /* Quattro bit per cifra, non otto: una cifra esadecimale E' mezzo byte.
           Ogni cifra nuova spinge le precedenti di una posizione, quindi le
           potenze di 16 si generano da se'. */
        val = (val << 4) | (uint32_t)pos;
        cifre++;
        s++;
    }

    /* Zero cifre lette non e' il numero zero: e' "" oppure "0x" da solo. Senza
       questo contatore le due cose sarebbero indistinguibili, perche' un
       accumulatore inizializzato a zero vale zero in entrambi i casi — e allora
       "peek" senza argomenti leggerebbe l'indirizzo 0 invece di lamentarsi. */
    if (cifre == 0)
        return 0;

    /* *out si scrive SOLO in caso di successo: chi chiama deve poter tenere il
       valore che aveva.

       Ed e' la ragione per cui il valore non puo' essere anche il valore di
       ritorno: "0" e' un risultato valido, "fallito" no, quindi i due esiti
       hanno bisogno di due canali distinti. */
    *out = val;
    return 1;
}

int shell_parse_dec(const char *s, uint32_t *out)
{
    uint32_t val = 0;
    int cifre = 0;

    while (*s != '\0') {
        if (*s < '0' || *s > '9')
            return 0;

        /* Il tetto si controlla PRIMA di moltiplicare, non dopo, ed e' la
           stessa ragione di shell_parse_hex: un overflow silenzioso farebbe
           leggere il settore sbagliato con l'aria di aver letto quello giusto.

           429496729 e' 2^32 / 10: sopra quello, il * 10 gira di sicuro. Al
           valore esatto si controlla anche la cifra. */
        if (val > 429496729u || (val == 429496729u && (*s - '0') > 5))
            return 0;

        val = val * 10 + (uint32_t)(*s - '0');
        cifre++;
        s++;
    }

    /* Zero cifre non e' il numero zero: e' la stringa vuota. Senza il
       contatore le due cose sarebbero indistinguibili, perche' l'accumulatore
       vale zero in entrambi i casi. */
    if (cifre == 0)
        return 0;

    /* *out solo in caso di successo: "0" e' un risultato valido e "fallito" no,
       quindi i due esiti hanno bisogno di due canali distinti. Stesso contratto
       di shell_parse_hex e di vfs_resolve. */
    *out = val;
    return 1;
}

/* ---- i comandi -------------------------------------------------------------

   Tutti hanno la stessa firma, anche quelli che non guardano gli argomenti: e'
   il prezzo dell'uniformita' che permette di metterli in un array, la stessa
   ragione per cui main riceve argc e argv anche nei programmi che li ignorano.
   Da cui i (void) per non far protestare -Wextra.

   Nessuno di loro usa vga_putc o serial_putc: solo kprintf, che scrive su
   entrambi. La seriale e' cio' che legge tests/shell.sh, quindi un comando che
   stampasse solo a schermo sarebbe invisibile alla verifica. */

static void shell_help(int argc, char **argv)
{
    int i, k;

    (void)argc;
    (void)argv;

    /* Si stampa DALLA tabella, non da una stringa scritta a mano: e' l'unico
       modo perche' non menta mai. Aggiungere un comando aggiorna help da se'. */
    for (i = 0; i < NCMDS; i++) {
        kprintf("  %s", table[i].name);

        /* L'incolonnamento a mano perche' '\t' non si puo' usare: vale 9, e
           vga_putc non ha un caso per lui — sulla seriale lo interpreta il
           terminale, a schermo non succede niente. */
        for (k = (int)strlen(table[i].name); k < 8; k++)
            kprintf(" ");

        kprintf("%s\n", table[i].help);
    }
}

static void shell_echo_cmd(int argc, char **argv)
{
    int i;

    /* Gli spazi si rimettono qui perche' shell_split li ha sostituiti con dei
       NUL: argv contiene parole separate, non piu' una riga. */
    for (i = 1; i < argc; i++) {
        if (i > 1)
            kprintf(" ");
        kprintf("%s", argv[i]);
    }

    kprintf("\n");
}

static void shell_ticks(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* %d e non %x perche' un conteggio si legge in decimale. Il cast e' per il
       debito annotato in CLAUDE.md: put_uint tratta la base 10 come con segno,
       quindi sopra 2^31 mentirebbe — a 100 Hz sono 248 giorni di uptime. */
    kprintf("%d tick\n", (int)timer_ticks());
}

static void shell_ps(int argc, char **argv)
{
    int i;

    (void)argc;
    (void)argv;

    kprintf("  task  esp       stato\n");

    for (i = 0; i < MAX_TASKS; i++) {
        struct task *t = task_slot(i);

        if (t == 0 || t->state == TASK_FREE)
            continue;

        kprintf("  %d     %x  %s\n", i, t->esp,
                i == task_current() ? "in esecuzione" : "pronto");
    }

    /* Nota sulla colonna esp: per il task IN ESECUZIONE il valore stampato e'
       vecchio, ed e' giusto che lo sia. Il campo esp di struct task viene
       scritto solo quando il task viene abbandonato da task_switch; finche' sta
       girando, il suo esp vero e' nel registro della CPU. Quindi quella riga
       mostra dove il task era l'ultima volta che ha ceduto il controllo. */
}

static void ltab_string(char *dest, const char *src, int n)
{
    memset(dest, ' ', n);
    memcpy(dest, src, strlen(src));
    dest[n-1]='\0';
}

/* Una riga di "devs", e sta in una funzione sola perche' i due rami del comando la
   stampano entrambi: duplicata, i due formati divergono al primo ritocco. E' la
   lezione di hexdump e di disco_da_argv.

   Le capacita' arrivano da devio_caps e NON si leggono qui guardando i puntatori a
   operazione. Farlo qui vorrebbe dire aprire un SECONDO switch su enum dev_kind —
   dopo quello di devio.c — e shell.c dovrebbe sapere dove stanno read e write
   dentro le due struct. Con la maschera non gli serve nemmeno includerle. */
static void devs_riga(char *s, const struct dev_entry *d)
{
    int caps = devio_caps(d);

    ltab_string(s, d->name, DEV_NAME_MAX);

    /* La specie e' UNA LETTERA, 'c' o 'b', e sono quelle vere di Unix: e' il primo
       carattere che ls -l mostra su un file di dispositivo. Costa zero e sta nella
       direzione del vincolo POSIX, come i numeri major/minor.

       Una parola — "caratteri", "blocchi" — sarebbe stata piu' leggibile e
       sbagliata per una ragione che vale la pena tenere scritta: tests/shell.sh
       verifica le capacita' cercando 'r' e 'w' NELLA RIGA, e dichiara la
       precondizione che lo rende lecito, cioe' che nessun nome di dispositivo
       contenga quelle lettere. "caratteri" ha una 'r' dentro, quindi la riga di
       console avrebbe detto "sa leggere" pur avendo read nullo. Il test l'ha
       preso, e il suo commento diceva in anticipo perche'.

       Sul disco la colonna delle capacita' guadagna significato: "r-" vuol dire
       read-only, che con struct device di M8 non era uno stato esprimibile — la'
       un dispositivo senza write era indistinguibile da un driver incompleto. */
    kprintf("  %c %s %d:%d %c%c\n",
            (d->kind == DEV_BLOCK) ? 'b' : 'c',
            s, d->major, d->minor,
            (caps & DEVIO_CAN_READ)  ? 'r' : '-',
            (caps & DEVIO_CAN_WRITE) ? 'w' : '-');
}

static void shell_devs(int argc, char **argv)
{
    char s[DEV_NAME_MAX];

    if (argc > 2) {
        kprintf("uso: devs [device]\n");
        return;
    }

    if (argc > 1) {
        /* dev_lookup_index rende un INDICE, non un puntatore, e dev_get(-1) rende
           0: le due si incastrano, quindi non serve un controllo in mezzo. */
        const struct dev_entry *d = dev_get(dev_lookup_index(argv[1]));

        if (d == 0) {
            kprintf("devs: %s: nessun dispositivo con questo nome\n", argv[1]);
            return;
        }

        devs_riga(s, d);
    } else {
        int i;

        for (i = 0; i < dev_count(); i++) {
            devs_riga(s, dev_get(i));
        }
    }
}

/* Il dump esadecimale, sedici byte per riga con l'offset a sinistra.

   Estratto da shell_peek in M10, quando rdsect ha voluto lo stesso output. E'
   la prima volta nel progetto che due comandi chiedono la stessa formattazione,
   e riscriverla vorrebbe dire due incolonnamenti che divergono al primo ritocco.

   base e' il numero stampato a inizio riga, e i due chiamanti gli danno cose
   diverse: peek un indirizzo di memoria, rdsect uno zero, perche' dentro un
   settore l'offset e' relativo al settore.

   Il puntatore e' const VOLATILE perche' peek guarda memoria che qualcun altro
   puo' cambiare — il framebuffer, domani i registri di un dispositivo — e il
   compilatore non deve accorpare o eliminare quelle letture. Chi passa memoria
   normale non ci perde niente: aggiungere un qualificatore e' sempre lecito. */
static void hexdump(const volatile uint8_t *p, uint32_t n, uint32_t base)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if ((i % 16) == 0)
            kprintf("%x: ", base + i);

        /* Lo zero iniziale a mano: %x non lo mette, quindi 0x0A uscirebbe come
           "a" e le colonne si disallineerebbero. */
        if (p[i] < 0x10)
            kprintf(" 0%x", p[i]);
        else
            kprintf(" %x", p[i]);

        if ((i % 16) == 15)
            kprintf("\n");
    }

    /* L'ultima riga, se non era piena. */
    if ((n % 16) != 0)
        kprintf("\n");
}

static void shell_peek(int argc, char **argv)
{
    uint32_t addr, n = 64;
    const volatile uint8_t *p;

    if (argc < 2) {
        kprintf("uso: peek <indirizzo> [n]\n");
        return;
    }

    if (!shell_parse_hex(argv[1], &addr)) {
        kprintf("peek: \"%s\" non e' un indirizzo esadecimale\n", argv[1]);
        return;
    }

    /* Anche n e' esadecimale, perche' shell_parse_hex ha una base sola. E'
       scritto nella riga di help: "peek b8000 20" mostra 32 byte, non 20. */
    if (argc >= 3 && !shell_parse_hex(argv[2], &n)) {
        kprintf("peek: \"%s\" non e' un numero esadecimale\n", argv[2]);
        return;
    }

    /* volatile: stiamo guardando memoria che qualcun altro puo' cambiare — il
       framebuffer, domani i registri di un dispositivo — e il compilatore non
       deve accorpare o eliminare queste letture.

       Nessun controllo sull'indirizzo, e non e' una dimenticanza: senza paging
       la segmentazione e' piatta su 4 GiB e nessun indirizzo puo' faultare. Da
       M13 non sara' piu' vero, e sara' questo comando a mostrarlo. */
    p = (const volatile uint8_t *)addr;

    hexdump(p, n, addr);
}

static void shell_spin(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kprintf("i due task di prova cominciano a stampare\n");
    demo_tasks_start();
}

static void shell_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Pulisce solo lo schermo: il log seriale conserva tutto, ed e' giusto —
       cancellare la traccia che leggono i test sarebbe un danno. */
    vga_clear();
}

static void shell_panic(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Serve a vedere dal prompt che M3 regge ancora: nome dell'eccezione, EIP e
       registri. panic e' noreturn, quindi da qui non si torna. */
    panic("panic richiesto dal prompt");
}

/* L'ultimo componente di un path: "/dev/kbd" -> "kbd". Serve solo al ramo di ls
   che stampa una voce sola, e non a vfs_resolve, che i componenti se li taglia
   da se'. Il ciclo non ha un caso speciale per la barra finale perche' non
   arriva mai qui: "/dev/" risolve a una directory, e le directory prendono
   l'altro ramo. */
static const char *basename(const char *path)
{
    const char *s;

    while ((s = strchr(path, '/')) != 0)
        path = s + 1;

    return path;
}

static void shell_ls(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/";
    struct inode *ino;
    char nome[VFS_NAME_MAX + 1];
    uint32_t n;
    int fd, idx, r;

    if (argc > 2) {
        kprintf("uso: ls [path]\n");
        return;
    }

    /* Si risolve PRIMA di aprire, perche' da un descrittore non si risale
       all'inode: fstat non esiste, e senza il tipo non si sa quale dei due rami
       prendere. E' il chiamante che a vfs_resolve mancava in M9a. */
    if (vfs_resolve(path, &ino) < 0) {
        kprintf("ls: %s: non esiste\n", path);
        return;
    }

    /* Su qualcosa che non e' una directory si mostra quella sola voce, come fa
       ls vero quando gli si passa un file. */
    if (ino->type != INODE_DIR) {
        kprintf("  %d %s", (int)ino->ino, basename(path));

        /* Tre rami e non due, da M11e: senza il secondo, un disco si annuncerebbe
           come "file 1048576 byte" — vero sulla dimensione e muto su cio' che
           conta, cioe' che dietro c'e' un dispositivo con dei numeri.

           Il disco mostra ENTRAMBE le cose, i numeri e la dimensione, perche' le
           ha entrambe: e' l'unico tipo di inode per cui major/minor e size sono
           significativi nello stesso momento. */
        if (ino->type == INODE_CHARDEV)
            kprintf("  chardev %d:%d", ino->major, ino->minor);
        else if (ino->type == INODE_BLOCKDEV)
            kprintf("  blockdev %d:%d  %d byte",
                    ino->major, ino->minor, (int)ino->size);
        else
            kprintf("  file %d byte", (int)ino->size);

        kprintf("\n");
        return;
    }

    fd = vfs_open(path, O_RDONLY);

    if (fd < 0) {
        kprintf("ls: %s: nessun descrittore libero\n", path);
        return;
    }

    for (idx = 0; ; idx++) {
        r = vfs_readdir(fd, idx, nome, &n);

        /* Zero e -1 sono distinti apposta, e il ciclo li tratta in modo diverso:
           lo zero e' "le voci sono finite", il -1 e' "la domanda non aveva
           senso". Un ciclo che si fermasse su entrambi sembrerebbe funzionare
           fino al giorno in cui readdir comincia a fallire davvero. */
        if (r == 0)
            break;

        if (r < 0) {
            kprintf("ls: errore leggendo la voce %d\n", idx);
            break;
        }

        kprintf("  %d %s\n", (int)n, nome);
    }

    /* Senza questa, otto ls esauriscono i descrittori del task e il sintomo
       arriva molto dopo la causa. */
    vfs_close(fd);
}

static void shell_cat(int argc, char **argv)
{
    struct inode *ino;
    char buf[64];
    uint32_t limite = 0;        /* 0 = nessun limite */
    uint32_t stampati = 0;
    int fd, r, i, dispositivo, fine;

    if (argc < 2 || argc > 3) {
        kprintf("uso: cat <path> [n]\n");
        return;
    }

    /* Il limite opzionale in byte, da M11e. Senza n il comportamento non cambia di
       una virgola — limite resta 0, cioe' "nessun limite" — quindi i test che
       esistevano coprono ancora quel ramo invariati.

       Serve a due cose, e la seconda non e' cosmetica: rende possibile la prova a
       mano su /dev/hda, che e' 1 MB, e impedisce di inondare la SERIALE, che e' il
       log che i test leggono con grep. Un cat sull'intero disco lo renderebbe
       inutilizzabile. */
    if (argc == 3 && !shell_parse_dec(argv[2], &limite)) {
        kprintf("cat: \"%s\" non e' un numero di byte\n", argv[2]);
        return;
    }

    if (vfs_resolve(argv[1], &ino) < 0) {
        kprintf("cat: %s: non esiste\n", argv[1]);
        return;
    }

    if (ino->type == INODE_DIR) {
        kprintf("cat: %s: e' una directory\n", argv[1]);
        return;
    }

    fd = vfs_open(argv[1], O_RDONLY);

    if (fd < 0) {
        kprintf("cat: %s: nessun descrittore libero\n", argv[1]);
        return;
    }

    /* Il tipo si guarda una volta e si tiene, perche' governa la CONDIZIONE DI
       USCITA, che e' l'unica cosa che distingue i due casi:

         file        read da' 0 quando la posizione ha raggiunto size
         dispositivo non finisce mai, e lo zero significa "adesso niente"

       Un cat che aspettasse lo zero su /dev/kbd resterebbe piantato per sempre:
       non ci sono segnali, quindi nemmeno un Ctrl-C con cui uscire. Su un
       dispositivo si smette al primo '\n'. */
    dispositivo = (ino->type == INODE_CHARDEV);
    fine = 0;

    while (!fine) {
        r = vfs_read(fd, buf, (uint32_t)sizeof(buf));

        if (r < 0) {
            kprintf("\ncat: errore di lettura\n");
            break;
        }

        if (r == 0) {
            if (dispositivo)
                continue;   /* spin: manca il blocking I/O, come in shell_task */

            break;          /* file: la posizione ha raggiunto size */
        }

        /* Un carattere per volta, non %s: read ritorna quanti byte, non una
           stringa, e il buffer non e' terminato. Con %s si stamperebbe fino al
           primo zero che capita nello stack. */
        for (i = 0; i < r; i++) {
            kprintf("%c", buf[i]);
            stampati++;

            /* Il limite conta i byte STAMPATI, non le chiamate a read: con un
               buffer da 64 e n == 15 si legge una volta e si esce a meta' del
               buffer. Contare le read darebbe multipli di 64. */
            if (limite != 0 && stampati >= limite) {
                fine = 1;
                break;
            }

            if (dispositivo && buf[i] == '\n') {
                fine = 1;
                break;
            }
        }
    }

    vfs_close(fd);
}

/* Decide su quale disco lavorano rdsect e wrsect, e dice da dove cominciano gli
   argomenti veri.

     rdsect 1 32        →  hda, e *primo vale 1
     rdsect hdb 7 32    →  hdb, e *primo vale 2

   Il discriminante e' gratis: shell_parse_dec rifiuta "hdb", e un nome di disco
   non puo' mai essere confuso con un numero di settore. Cosi' la forma vecchia
   continua a funzionare senza un caso a parte.

   Per NOME e non per indice perche' blk stampa gia' i nomi: "rdsect hdb 7" si
   legge, "rdsect 1 7" bisogna ricordarselo.

   Una funzione sola per entrambi i comandi, ed e' la lezione di hexdump: con la
   regola in due posti, rdsect e wrsect prima o poi la interpreterebbero in modo
   diverso — e qui la divergenza sarebbe che wrsect scrive sul disco sbagliato.

   Il default e' hda, e la scelta e' deliberata: su hdb c'e' il filesystem, e
   wrsect ne distruggerebbe il superblocco. Il disco a pattern di M10 esiste
   apposta per essere scritto. */
static struct blockdev *disco_da_argv(int argc, char **argv, int *primo)
{
    uint32_t n;

    *primo = 1;

    if (argc > 1 && !shell_parse_dec(argv[1], &n)) {
        *primo = 2;
        return dev_blockdev(argv[1]);
    }

    /* Il default e' hda per NOME e non ata_drive(0) per indice, da M11e: l'indice
       e' l'ordine di iscrizione, quindi con il solo slave presente ata_drive(0)
       sarebbe hdb — cioe' il disco col filesystem, quello che wrsect non deve
       toccare. Il nome chiede il disco che si vuole.

       disco_per_nome e' sparita: era chardev_find un piano sotto, scritta quando i
       dischi non avevano un registro. Adesso ce l'hanno, ed e' dev_blockdev che
       controlla anche kind prima di castare. */
    return dev_blockdev("hda");
}

static void shell_lsblk(int argc, char **argv)
{
    int i;

    (void)argc;
    (void)argv;

    /* Una VISTA FILTRATA sul registry, non un elenco parallelo: da M11e i dischi
       si iscrivono come tutti gli altri, e questo comando li seleziona per kind
       invece di chiedere ad ata.c quali conosce.

       Cambia anche il messaggio: "nessun disco sul canale primario" non e' piu'
       vero, perche' il registry non sa da quale canale vengano — e il giorno che
       si aggiunge un secondo driver a blocchi comparirebbe qui senza una riga di
       modifica. E' la misura di un elenco che ha una sola sorgente. */
    int n = 0;

    for (i = 0; i < dev_count(); i++) {
        const struct dev_entry *e = dev_get(i);
        const struct blockdev *b;

        if (e->kind != DEV_BLOCK)
            continue;

        b = (const struct blockdev *)e->impl;
        n++;

        /* I kilobyte accanto ai settori: "2048 settori" non dice niente a colpo
           d'occhio, "1024 KB" si'. Un settore e' mezzo kilobyte, da cui il / 2.

           I cast a int sono per il debito annotato in CLAUDE.md: put_uint
           tratta la base 10 come con segno, quindi sopra 2^31 mentirebbe. Un
           disco da 1 TiB ci arriverebbe — 2 miliardi di settori — e in LBA28
           non ci puo' stare, ma il cast dichiara che il limite lo conosciamo. */
        kprintf("  %s  %d:%d  %d settori  (%d KB)\n",
                e->name, e->major, e->minor,
                (int)b->nsectors, (int)(b->nsectors / 2));
    }

    if (n == 0)
        kprintf("nessun disco\n");
}

static void shell_rdsect(int argc, char **argv)
{
    /* 512 byte sullo stack, un ottavo dei 4096 di un task: e' la variabile
       locale piu' grande del progetto. Sullo stack e non static, cosi' il
       comando resta rientrante — in M16 le shell saranno piu' di una.

       uint8_t e non char: un byte >= 0x80 dentro un char con segno viene esteso
       a un int negativo, e %x lo stamperebbe come ffffff9f invece di 9f. */
    uint8_t buf[SECTOR_SIZE];
    struct blockdev *b;
    uint32_t lba, n = SECTOR_SIZE;
    int primo;

    b = disco_da_argv(argc, argv, &primo);

    if (argc < primo + 1 || argc > primo + 2) {
        kprintf("uso: rdsect [disco] <settore> [n]  - il disco predefinito e' hda\n");
        return;
    }

    if (b == 0) {
        kprintf("rdsect: nessun disco che si chiami \"%s\"\n", argv[1]);
        return;
    }

    /* DECIMALE, non esadecimale come peek. Gli indirizzi si scrivono in
       esadecimale, i numeri di settore no — e in M11b li leggerai dal
       superblocco minix in decimale. Con parse_hex, "rdsect 10" leggerebbe il
       settore 16. */
    if (!shell_parse_dec(argv[primo], &lba)) {
        kprintf("rdsect: \"%s\" non e' un numero di settore\n", argv[primo]);
        return;
    }

    if (argc == primo + 2 && !shell_parse_dec(argv[primo + 1], &n)) {
        kprintf("rdsect: \"%s\" non e' un numero\n", argv[primo + 1]);
        return;
    }

    if (n > SECTOR_SIZE)
        n = SECTOR_SIZE;

    /* UN settore: il quarto argomento e' un conteggio di SETTORI, non di byte.
       E il valore di ritorno sono settori, quindi si confronta con 1 e non si
       usa come limite del ciclo di stampa. */
    if (b->read(b, lba, buf, 1) != 1) {
        kprintf("rdsect: %s: lettura del settore %d fallita\n",
                b->name, (int)lba);
        return;
    }

    /* L'offset parte da zero: dentro un settore e' relativo al settore, non un
       indirizzo di memoria. */
    hexdump(buf, n, 0);
}

static void shell_wrsect(int argc, char **argv)
{
    uint8_t buf[SECTOR_SIZE];
    struct blockdev *b;
    const char *hex;
    uint32_t lba, i;
    size_t len, nbytes;
    int hi, lo, primo;

    b = disco_da_argv(argc, argv, &primo);

    if (argc != primo + 2) {
        kprintf("uso: wrsect [disco] <settore> <hex>  - predefinito hda\n");
        return;
    }

    if (b == 0) {
        kprintf("wrsect: nessun disco che si chiami \"%s\"\n", argv[1]);
        return;
    }

    if (!shell_parse_dec(argv[primo], &lba)) {
        kprintf("wrsect: \"%s\" non e' un numero di settore\n", argv[primo]);
        return;
    }

    /* Il pattern e' una sequenza di BYTE in esadecimale, quindi le cifre vanno
       a coppie: "deadbeef" sono i quattro byte de ad be ef, non gli otto
       caratteri 'd' 'e' 'a' 'd'... Le due letture sono ugualmente difendibili e
       distinguibili solo rileggendo, che e' il motivo per cui sta scritto nella
       riga di help. */
    hex = argv[primo + 1];
    len = strlen(hex);

    if (len == 0 || (len % 2) != 0 || len > 2 * SECTOR_SIZE) {
        kprintf("wrsect: il pattern vuole un numero PARI di cifre, "
                "da 2 a %d\n", 2 * SECTOR_SIZE);
        return;
    }

    for (i = 0; i < len; i += 2) {
        hi = strpos(HEXA, tolower(hex[i]));
        lo = strpos(HEXA, tolower(hex[i + 1]));

        if (hi < 0 || lo < 0) {
            kprintf("wrsect: \"%s\" non e' esadecimale\n", hex);
            return;
        }

        buf[i / 2] = (uint8_t)((hi << 4) | lo);
    }

    nbytes = len / 2;

    /* Un settore si scrive INTERO, sempre: non esiste una scrittura parziale.
       Il pattern si ripete fino in fondo — e funziona leggendo dal buffer
       stesso, perche' i primi nbytes byte sono gia' a posto. */
    for (i = (uint32_t)nbytes; i < SECTOR_SIZE; i++)
        buf[i] = buf[i % nbytes];

    /* Il ritorno sono SETTORI, quindi si confronta con 1. Stamparlo come "byte
       scritti" direbbe "scritto 1 byte" dopo averne scritti 512, e su un
       fallimento direbbe "scritti -1 byte", che e' un successo annunciato. */
    if (b->write(b, lba, buf, 1) != 1) {
        kprintf("wrsect: %s: scrittura del settore %d fallita\n",
                b->name, (int)lba);
        return;
    }

    /* Il nome del disco nel messaggio non e' decorazione: e' l'unica conferma
       che si e' scritto dove si voleva, e wrsect sul disco sbagliato distrugge
       un filesystem. */
    kprintf("  scritti %d byte nel settore %d di %s\n",
            SECTOR_SIZE, (int)lba, b->name);
}

static void shell_mkdir(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("uso: mkdir <path>\n");
        return;
    }

    /* Il messaggio in caso di successo non e' cortesia: e' un comando che
       modifica un disco, e la conferma e' cio' che distingue "fatto" da
       "fallito in silenzio" il giorno che fsck si lamenta. */
    if (vfs_mkdir(argv[1]) < 0) {
        kprintf("mkdir: %s: non riesco a creare\n", argv[1]);
        return;
    }

    kprintf("  creata %s\n", argv[1]);
}

static void shell_write(int argc, char **argv)
{
    /* La riga di comando e' lunga al massimo LINEEDIT_MAX, quindi il contenuto
       non puo' superarla: 128 byte bastano e avanzano, e non servono i 512 di
       rdsect. */
    char testo[LINE_MAX];
    uint32_t len = 0;
    int fd, i, k, r;

    if (argc < 3) {
        kprintf("uso: write <path> <testo...>\n");
        return;
    }

    /* Gli spazi si rimettono a mano, come in echo: shell_split li ha sostituiti
       con dei NUL, quindi argv contiene parole e non piu' una riga. */
    for (i = 2; i < argc; i++) {
        if (i > 2 && len < sizeof(testo) - 1)
            testo[len++] = ' ';

        for (k = 0; argv[i][k] != '\0' && len < sizeof(testo) - 1; k++)
            testo[len++] = argv[i][k];
    }

    /* Il newline finale: senza, cat sull'host attacca il prompt al contenuto. */
    if (len < sizeof(testo))
        testo[len++] = '\n';

    /* O_CREAT e' un BIT, quindi si combina con |. Se il file esiste gia' si apre
       quello che c'e' e lo si SOVRASCRIVE dall'inizio: non c'e' O_APPEND e non
       c'e' troncamento, quindi scrivendo un testo piu' corto del precedente la
       coda vecchia resta. Sta nella riga di help. */
    fd = vfs_open(argv[1], O_WRONLY | O_CREAT);

    if (fd < 0) {
        kprintf("write: %s: non riesco ad aprire\n", argv[1]);
        return;
    }

    r = vfs_write(fd, testo, len);

    vfs_close(fd);

    /* Si riporta quanti byte sono passati DAVVERO, non quanti se ne volevano:
       con il disco pieno il messaggio direbbe una cosa e fsck un'altra. */
    if (r < 0) {
        kprintf("write: %s: scrittura fallita\n", argv[1]);
        return;
    }

    kprintf("  scritti %d byte\n", r);
}

/* ---- il motore -------------------------------------------------------------- */

void shell_init(void)
{
    /* Una volta sola, e prima di task_create(shell_task): lineedit_init azzera
       anche len, quindi rifarlo nel ciclo cancellerebbe la riga mentre la stai
       scrivendo. Per ricominciare dopo un comando c'e' lineedit_reset. */
    lineedit_init(&le, shell_echo);
}

void shell_exec(char *line)
{
    char *argv[SHELL_MAX_ARGS];
    int argc, i;

    argc = shell_split(line, argv, SHELL_MAX_ARGS);

    /* Riga vuota: non e' un errore, si ristampa solo il prompt. */
    if (argc == 0)
        return;

    for (i = 0; i < NCMDS; i++) {
        if (strcmp(argv[0], table[i].name) == 0) {
            table[i].fn(argc, argv);
            return;
        }
    }

    /* Dire QUALE comando non esiste, non solo che non esiste: altrimenti serve
       una ricompilazione per capire se hai sbagliato a digitare. */
    kprintf("%s: comando non trovato\n", argv[0]);
}

void shell_task(void)
{
    for (;;) {
        int c;

        kprintf("%s", SHELL_PROMPT);

        /* Spin su keyboard_getchar, deliberato: manca il blocking I/O, sta
           nello spec sotto "fuori scope", e il punto di decisione e' M9. Con la
           prelazione gli altri task girano comunque.

           Nessun hlt: funzionerebbe oggi, ma e' privilegiata, e in M14 questo
           ciclo finisce in ring 3 dove prende un #GP. Scriverlo adesso senza
           significa non riscriverlo allora. */
        for (;;) {
            c = keyboard_getchar();

            if (c < 0)
                continue;

            if (lineedit_putc(&le, (char)c))
                break;
        }

        shell_exec(le.buf);
        lineedit_reset(&le);
    }
}

