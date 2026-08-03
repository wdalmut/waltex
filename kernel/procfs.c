/* procfs: /proc sopra la tabella dei task.
 *
 * Riempie le stesse cinque caselle di inode_ops che riempiono devfs.c e
 * minixfs.c, e il VFS non sa quale dei tre sta parlando. Agganciarlo non ha
 * richiesto una riga in vfs.c ne' in minixfs.c — che era il vero scopo di M11d,
 * perche' fino a qui la tabella di mount di M11c aveva un cliente solo, ed era
 * lo stesso di prima.
 *
 * La differenza vera con gli altri due: qui il contenuto NON ESISTE. minixfs
 * serve byte che stanno sul disco, devfs byte che arrivano da un driver, procfs
 * li costruisce nell'istante in cui glieli si chiede.
 */

#include "procfs.h"
#include "vfs.h"
#include "types.h"
#include "task.h"
#include "kprintf.h"
#include "memory.h"

/* 1 + 8 + 8 inode statici, meno di mezzo kilobyte di .bss, e il bilancio si
   legge a tempo di link — che e' il punto della disciplina statica fino a M12.
   Gli inode dei task esistono TUTTI, anche per gli slot liberi: sono lookup e
   readdir a nascondere quelli che non servono. */
static int          ready;
static struct inode ino_root;
static struct inode ino_task[MAX_TASKS];
static struct inode ino_status[MAX_TASKS];

static int root_lookup (struct inode *dir, const char *name, struct inode **out);
static int root_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);
static int task_lookup (struct inode *dir, const char *name, struct inode **out);
static int task_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);
static int status_read (struct inode *ino, uint32_t off, void *buf, uint32_t n);

/* Inizializzatori designati, non la forma posizionale: da M11b inode_ops ha
   cinque campi, e la forma posizionale farebbe protestare -Wextra su ogni
   tabella incompleta — cioe' produrrebbe un avviso permanente e giusto, che e'
   un avviso che si smette di leggere. I campi assenti restano a zero
   DICHIARANDOLO. */
static const struct inode_ops ops_root = {
    .lookup = root_lookup, .readdir = root_readdir
};

static const struct inode_ops ops_taskdir = {
    .lookup = task_lookup, .readdir = task_readdir
};

/* Solo read. write e create nulli significano "non supportate", non "errore":
   e' la convenzione di M8, ed e' cio' che fa fallire "mkdir /proc/x" da se',
   senza che nessuno scriva un caso a parte. */
static const struct inode_ops ops_status = {
    .read = status_read
};

/* ---- aiutanti --------------------------------------------------------------- */

/* Lo slot i esiste e ospita un task vivo?
 *
 * L'ordine dei controlli non e' scambiabile: task_slot ritorna 0 per un indice
 * fuori intervallo, quindi guardare state prima di aver escluso il puntatore
 * nullo dereferenzia l'indirizzo zero. E' la regola di M10 — BSY prima di tutto,
 * perche' finche' e' acceso gli altri sette bit non significano niente.
 */
static int task_attivo(int i)
{
    struct task *t;

    if (i < 0 || i >= MAX_TASKS)
        return 0;

    t = task_slot(i);

    if (t == 0)
        return 0;

    return t->state != TASK_FREE;
}

/* Il nome di una voce di /proc e' un numero, e questo lo traduce all'indietro.
 *
 * Si RIFIUTA tutto quello che non e' cifre decimali dalla prima all'ultima:
 * "0pippo" non e' il task 0, " 3" non e' il task 3, e "03" non e' un secondo
 * nome per lo stesso file. Un parser che si fermasse alla prima cifra non
 * numerica accetterebbe tutti e tre, ed e' la stessa specie di errore del nome
 * di dispositivo troncato in M8: due grafie che risolvono allo stesso oggetto.
 *
 * Ritorna l'indice, oppure -1 — che non e' un indice valido, quindi non si
 * confonde con un risultato.
 */
static int indice_da_nome(const char *name)
{
    int v = 0;
    int i;

    if (name == 0 || name[0] == '\0')
        return -1;

    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] < '0' || name[i] > '9')
            return -1;

        v = v * 10 + (name[i] - '0');

        /* Il taglio va DENTRO il ciclo: "99999999999" farebbe girare un int
           prima di arrivare al confronto finale, e un numero girato puo'
           ricadere per caso in intervallo. */
        if (v >= MAX_TASKS)
            return -1;
    }

    return v;
}

/* Il testo di /proc/<i>/status, e la funzione che fa esistere il contenuto.
 *
 * Ritorna la lunghezza scritta senza il terminatore, oppure -1 se il task non e'
 * attivo o se il testo non ci sta.
 *
 * NIENTE TAB. Linux allinea /proc/N/status con '\t'; noi non possiamo, perche'
 * '\t' vale 9 e vga_putc gestisce solo >= 32 piu' '\n' e '\b'. Un tab
 * funzionerebbe sulla seriale e sparirebbe sul framebuffer, che e' esattamente
 * il bug del backspace di M7 — li' era '\b' a valere 8 e a non passare per
 * nessuno dei due rami.
 *
 * Nota sul valore di esp, la stessa che shell_ps ha in fondo: per il task IN
 * ESECUZIONE il numero e' vecchio, ed e' giusto che lo sia. Il campo esp di
 * struct task viene scritto solo quando il task viene abbandonato da
 * task_switch; finche' sta girando, il suo esp vero e' nel registro della CPU.
 * Quindi la riga dice dove il task era l'ultima volta che ha ceduto il
 * controllo.
 */
static int genera_status(int i, char *buf, int max)
{
    struct task *t;
    int scritti;

    if (!task_attivo(i) || buf == 0 || max <= 0)
        return -1;

    t = task_slot(i);

    scritti = snprintf(buf, (size_t)max,
                       "Pid:    %d\n"
                       "State:  R (%s)\n"
                       "Esp:    0x%x\n",
                       i,
                       i == task_current() ? "running" : "ready",
                       t->esp);

    /* snprintf ha la semantica C99: ritorna la lunghezza VOLUTA, non quella
       scritta. Quindi un valore >= max significa che ha troncato, e un testo
       troncato e' un guasto da riportare invece di una risposta da consegnare. */
    if (scritti < 0 || scritti >= max)
        return -1;

    return scritti;
}

/* ---- /proc ----------------------------------------------------------------- */

static int root_lookup(struct inode *dir, const char *name, struct inode **out)
{
    int i;

    if (dir == 0 || dir->ino != PROC_INO_ROOT)
        return -1;

    i = indice_da_nome(name);

    /* Uno slot libero NON esiste come voce. E' cio' che distingue "genero le
       voci dalla tabella" da "ho otto directory fisse": senza questo controllo
       /proc/7 ci sarebbe sempre, e cat /proc/7/status leggerebbe uno slot
       vuoto. */
    if (i < 0 || !task_attivo(i))
        return -1;

    /* *out si scrive SOLO in caso di successo. E' il primo dei tre bug di M9b:
       root_lookup di devfs ritornava 1 invece di -1, vfs_resolve controlla < 0,
       e camminava su un puntatore mai inizializzato — nessun sintomo stabile,
       una cosa diversa a ogni boot. */
    *out = &ino_task[i];
    return 0;
}

/* ATTENZIONE: idx e' una POSIZIONE fra i task attivi, NON un indice di task.
 * Con i task 0 e 3 attivi:
 *
 *     idx = 0  ->  "0"
 *     idx = 1  ->  "3"      e NON 0, che significherebbe "le voci sono finite"
 *     idx = 2  ->  0
 *
 * Confondere i due fa fermare l'enumerazione al primo slot libero, e il sintomo
 * e' il peggiore che ci sia: una lista PLAUSIBILE E INCOMPLETA. Oggi non si
 * vedrebbe — al boot i task 0-3 sono contigui — e comparirebbe al primo exit di
 * M16, lontano da qui.
 */
static int root_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out)
{
    int i, visti;

    if (dir == 0 || dir->ino != PROC_INO_ROOT)
        return -1;

    /* -1 e non 0: "la domanda non aveva senso" e "le voci sono finite" sono due
       cose diverse, e un ciclo che si fermasse su entrambe sembrerebbe
       funzionare fino al giorno in cui readdir fallisce davvero. */
    if (idx < 0)
        return -1;

    visti = 0;

    for (i = 0; i < MAX_TASKS; i++) {
        if (!task_attivo(i))
            continue;

        if (visti == idx) {
            /* Il valore di ritorno di utoa — qui snprintf — si controlla: e'
               l'unico posto che sa se il nome ci e' entrato. */
            if (snprintf(name, VFS_NAME_MAX + 1, "%d", i) > VFS_NAME_MAX)
                return -1;

            *ino_out = PROC_INO_TASK(i);
            return 1;
        }

        visti++;
    }

    return 0;
}

/* ---- /proc/<N> -------------------------------------------------------------- */

/* Qui "dir" serve DAVVERO, ed e' la terza volta nel progetto: una funzione sola
 * per otto directory, e dir->ino e' l'unica cosa che le distingue. La prima nota
 * sta nelle inode_ops da M9b, la seconda e' minix_lookup.
 */
static int task_lookup(struct inode *dir, const char *name, struct inode **out)
{
    int i;

    if (dir == 0 || name == 0)
        return -1;

    i = PROC_TASK_DA_DIR(dir->ino);

    /* Si ricontrolla che il task sia vivo, e non e' ridondante con root_lookup:
       fra la risoluzione di /proc/<N> e questa chiamata puo' essere passato un
       tick, e da M16 il task puo' essere uscito nel frattempo. */
    if (!task_attivo(i))
        return -1;

    if (strcmp(name, "status") != 0)
        return -1;

    *out = &ino_status[i];
    return 0;
}

static int task_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out)
{
    int i;

    if (dir == 0)
        return -1;

    if (idx < 0)
        return -1;

    i = PROC_TASK_DA_DIR(dir->ino);

    if (!task_attivo(i))
        return -1;

    /* Una voce sola. Ritornare 1 per ogni idx manderebbe in ciclo infinito
       chiunque enumeri: shell_ls si ferma solo sullo zero. */
    if (idx > 0)
        return 0;

    if (snprintf(name, VFS_NAME_MAX + 1, "status") > VFS_NAME_MAX)
        return -1;

    *ino_out = PROC_INO_STATUS(i);
    return 1;
}

/* ---- /proc/<N>/status ------------------------------------------------------- */

/* Genera il testo intero e ne consegna la fetta che parte da off.
 *
 * SENZA NESSUNO STATO fra una chiamata e l'altra, ed e' la differenza che conta.
 * Un flag statico che alterni "prima riga" e "finito" funziona per cat e si
 * rompe per tutto il resto: una lseek all'indietro darebbe zero, e due letture
 * intrecciate su due task si spegnerebbero a vicenda perche' il flag e' uno per
 * tutto il filesystem.
 *
 * Il buffer e' LOCALE. Statico costerebbe 128 byte una volta sola, ma fra il
 * "genero" e il "copio" ci sta un tick del timer, e due cat in parallelo si
 * mescolerebbero. Sono 128 byte su uno stack da 4096, e non serve nessuna
 * sezione critica.
 *
 * Rigenerare a ogni read e' O(tre righe). La cache e' il problema che seq_file
 * risolve in Linux, dove i file di /proc possono essere lunghi megabyte.
 */
static int status_read(struct inode *ino, uint32_t off, void *buf, uint32_t n)
{
    char testo[PROC_STATUS_MAX];
    char *dst = (char *)buf;
    int i, len;
    uint32_t k;

    if (ino == 0 || buf == 0)
        return -1;

    /* n == 0 si tratta PRIMA di generare: non c'e' niente da copiare, e
       proseguire significherebbe misurare un buffer che nessuno ha terminato. */
    if (n == 0)
        return 0;

    i = PROC_TASK_DA_STATUS(ino->ino);

    /* Un task uscito fra la open e la read da' un file VUOTO, non un errore. E'
       una scelta, non un caso dimenticato: e' quello che fa Linux, e la ragione
       e' che cat su un processo appena morto deve finire in silenzio invece di
       stampare un messaggio. */
    if (!task_attivo(i))
        return 0;

    len = genera_status(i, testo, (int)sizeof(testo));

    if (len < 0)
        return -1;

    /* Oltre la fine: zero. Su un file regolare lo zero significa davvero
       "finito", ed e' cosi' che shell_cat esce dal suo ciclo. */
    if (off >= (uint32_t)len)
        return 0;

    if (n > (uint32_t)len - off)
        n = (uint32_t)len - off;

    for (k = 0; k < n; k++)
        dst[k] = testo[off + k];

    /* Si ritorna quanti byte si sono COPIATI, non quanti se ne sono generati:
       vfs_read avanza la posizione di questo numero, e mentire fa credere al
       chiamante di avere un buffer pieno che e' mezzo vuoto. E' la stessa classe
       di chardev_read che ritornava 1 in M9b. */
    return (int)n;
}

/* ---- init ------------------------------------------------------------------- */

void procfs_init(void)
{
    int i;

    memset(&ino_root,   0, sizeof(ino_root));
    memset(ino_task,    0, sizeof(ino_task));
    memset(ino_status,  0, sizeof(ino_status));

    ino_root.ino  = PROC_INO_ROOT;
    ino_root.type = INODE_DIR;
    ino_root.size = 0;
    ino_root.ops  = &ops_root;

    /* TUTTI gli slot, anche quelli liberi: nascondere i task inattivi e' compito
       di lookup e readdir, che guardano la tabella al momento della domanda. Se
       questo ciclo saltasse gli slot liberi, /proc sarebbe congelato all'istante
       del boot e "spin" non comparirebbe. */
    for (i = 0; i < MAX_TASKS; i++) {
        ino_task[i].ino  = PROC_INO_TASK(i);
        ino_task[i].type = INODE_DIR;
        ino_task[i].size = 0;
        ino_task[i].ops  = &ops_taskdir;

        /* size 0 e' CORRETTO, non una dimenticanza: la dimensione di un file
           generato non si conosce prima di generarlo. vfs_read non consulta
           size — verificato — e shell_cat esce quando read ritorna 0. E' anche
           quello che fa Linux: ls -l /proc/1/status mostra 0 byte.

           priv resta a zero. Ci starebbe il struct task *, ed e' esattamente la
           cosa da non fare: in M16 gli slot vengono riciclati, e l'indice sta
           gia' dentro ino. */
        ino_status[i].ino  = PROC_INO_STATUS(i);
        ino_status[i].type = INODE_FILE;
        ino_status[i].size = 0;
        ino_status[i].ops  = &ops_status;
    }

    ready = 1;
}

struct inode *procfs_procdir(void)
{
    if (!ready)
        return 0;

    return &ino_root;
}
