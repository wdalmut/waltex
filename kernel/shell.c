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
#include "device.h"
#include "vfs.h"

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

/* const, non solo static: la tabella non cambia mai, e il const la sposta in
   .rodata invece che in .data. Con il bilancio della memoria che si legge a
   tempo di link, e' gratis e si vede.

   Nessuna voce terminatrice a zero: la tabella e' static in questo file e chi
   la percorre sta qui, quindi il numero lo calcola il compilatore. Un
   terminatore aggiungerebbe un modo di rompersi — dimenticarlo — senza
   aggiungere niente. */
static const struct shell_cmd table[] = {
    { "help",  shell_help,     "elenca i comandi" },
    { "echo",  shell_echo_cmd, "stampa i suoi argomenti" },
    { "ticks", shell_ticks,    "tick del timer dal boot" },
    { "ps",    shell_ps,       "stato della tabella dei task" },
    { "peek",  shell_peek,     "peek <indirizzo> [n] - dump, entrambi in esadecimale" },
    { "spin",  shell_spin,     "avvia i due task di prova rumorosi" },
    { "clear", shell_clear,    "pulisce lo schermo" },
    { "panic", shell_panic,    "provoca un panic deliberato" },
    { "devs",  shell_devs,     "elenca i device registrati"},
    { "ls",    shell_ls,       "naviga il filesystem"},
    { "cat",   shell_cat,      "mostra il contenuto di un file"}
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

static void shell_devs(int argc, char **argv)
{
    char s[DEV_NAME_MAX];

    if (argc > 2) {
        kprintf("uso: devs [device]\n");
        return;
    }

    int ndevs = device_count();

    if (argc > 1) {
        struct device *d = device_find(argv[1]);
        if (d) {
            ltab_string(s, d->name, DEV_NAME_MAX);
            kprintf("  %s %d:%d %c%c\n", s, d->major, d->minor, (d->read) ? 'r' : '-', (d->write) ? 'w' : '-');
        } else {
            kprintf("devs: %s: nessun dispositivo con questo nome\n", argv[1]);
        }
    } else {
        for (uint8_t i=0; i<ndevs; i++) {
            struct device *d = device_at(i);

            ltab_string(s, d->name, DEV_NAME_MAX);

            kprintf("  %s %d:%d %c%c\n", s, d->major, d->minor, (d->read) ? 'r' : '-', (d->write) ? 'w' : '-');
        }
    }
}

static void shell_peek(int argc, char **argv)
{
    uint32_t addr, n = 64, i;
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

    for (i = 0; i < n; i++) {
        if ((i % 16) == 0)
            kprintf("%x: ", addr + i);

        /* Lo zero iniziale a mano: %x non lo mette, quindi 0x0A uscirebbe come
           "a" e le colonne si disallineerebbero. */
        if (p[i] < 0x10)
            kprintf(" 0%x", p[i]);
        else
            kprintf(" %x", p[i]);

        if ((i % 16) == 15)
            kprintf("\n");
    }

    if ((n % 16) != 0)
        kprintf("\n");
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

        if (ino->type == INODE_CHARDEV)
            kprintf("  chardev %d:%d", ino->major, ino->minor);
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
    int fd, r, i, dispositivo, fine;

    if (argc != 2) {
        kprintf("uso: cat <path>\n");
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

            if (dispositivo && buf[i] == '\n') {
                fine = 1;
                break;
            }
        }
    }

    vfs_close(fd);
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
