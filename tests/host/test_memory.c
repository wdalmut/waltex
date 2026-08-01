/* Test di kernel/memory.c col gcc dell'host, senza QEMU.
   WALTEX_HOSTED (definito nel Makefile) fa arrivare i tipi da stdint.h.

   Nessun <string.h>: le funzioni sotto test hanno gli stessi nomi di quelle di
   glibc, e includerlo confonderebbe chi legge su quale versione stia girando il
   test. Sotto test c'e' sempre quella di memory.h, perche' -fno-builtin (vedi
   il Makefile) impedisce a gcc di espanderle inline per conto suo.

   Non c'e' nessun test sulle regioni sovrapposte: per memcpy sono
   comportamento indefinito, e un test che le esercitasse certificherebbe
   qualcosa che il contratto non promette. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "memory.h"

static int failures;

static void check(const char *name, int ok)
{
    printf("%s -- %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

/* Arena con guardie: 8 byte di 0xAA prima e dopo l'area utile, per accorgersi
   di una scrittura fuori dai limiti invece di sperare che si noti.
   Union per garantire l'allineamento a 16 bit richiesto da memset16. */
#define GUARD 8
#define AREA  64

static union {
    uint16_t w[(GUARD + AREA + GUARD) / 2];
    uint8_t  b[GUARD + AREA + GUARD];
} arena;

static uint8_t *area(void)
{
    return arena.b + GUARD;
}

static void arena_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(arena.b); i++)
        arena.b[i] = 0xAA;
}

static int guards_intact(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        if (arena.b[i] != 0xAA)
            return 0;
        if (arena.b[GUARD + AREA + i] != 0xAA)
            return 0;
    }
    return 1;
}

/* Quanti byte dell'area utile, a partire da 'from', sono ancora intonsi. */
static int untouched_from(size_t from)
{
    size_t i;
    for (i = from; i < AREA; i++)
        if (area()[i] != 0xAA)
            return 0;
    return 1;
}

static void test_memcpy(void)
{
    uint8_t src[16];
    size_t i;
    int ok;

    for (i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)(0x10 + i);

    /* Copia ogni byte, non solo il primo: e' il bug che si ottiene avanzando
       il puntatore sbagliato dentro il ciclo. */
    arena_reset();
    memcpy(area(), src, sizeof(src));
    ok = 1;
    for (i = 0; i < sizeof(src); i++)
        if (area()[i] != src[i])
            ok = 0;
    check("memcpy copia tutti i byte, in ordine", ok);

    check("memcpy non scrive oltre n", untouched_from(sizeof(src)));
    check("memcpy non tocca le guardie", guards_intact());

    /* n = 0 non deve scrivere nulla. */
    arena_reset();
    memcpy(area(), src, 0);
    check("memcpy con n=0 non scrive nulla", untouched_from(0));

    /* Il valore di ritorno e' la destinazione. */
    arena_reset();
    check("memcpy restituisce dest", memcpy(area(), src, 4) == area());

    /* Lunghezza dispari e destinazione disallineata: e' una funzione che
       ragiona in byte, non deve avere preferenze. */
    arena_reset();
    memcpy(area() + 1, src, 7);
    ok = 1;
    for (i = 0; i < 7; i++)
        if (area()[1 + i] != src[i])
            ok = 0;
    check("memcpy funziona disallineato e con lunghezza dispari", ok);
    check("memcpy disallineato non sfora", area()[0] == 0xAA && untouched_from(8));
}

static void test_memset(void)
{
    size_t i;
    int ok;

    arena_reset();
    memset(area(), 0x5C, 10);
    ok = 1;
    for (i = 0; i < 10; i++)
        if (area()[i] != 0x5C)
            ok = 0;
    check("memset riempie n byte con il valore", ok);

    check("memset non scrive oltre n", untouched_from(10));
    check("memset non tocca le guardie", guards_intact());

    /* Il parametro e' un int ma lo standard lo converte a unsigned char:
       conta solo il byte basso. */
    arena_reset();
    memset(area(), 0x1234, 4);
    ok = 1;
    for (i = 0; i < 4; i++)
        if (area()[i] != 0x34)
            ok = 0;
    check("memset tronca il valore al byte basso", ok);

    arena_reset();
    memset(area(), 0x5C, 0);
    check("memset con n=0 non scrive nulla", untouched_from(0));

    arena_reset();
    check("memset restituisce dest", memset(area(), 0, 4) == area());
}

static void test_memset16(void)
{
    uint16_t *cells;
    size_t i;
    int ok;

    /* La cella VGA: due byte diversi nello stesso elemento. E' il caso che
       memset non puo' esprimere, ed e' la ragione per cui memset16 esiste. */
    arena_reset();
    cells = (uint16_t *)area();
    memset16(cells, 0x0720, 6);
    ok = 1;
    for (i = 0; i < 6; i++)
        if (cells[i] != 0x0720)
            ok = 0;
    check("memset16 replica un pattern a 16 bit", ok);

    /* count conta ELEMENTI: 6 elementi sono 12 byte, il tredicesimo e' intonso.
       Se qualcuno lo interpretasse come byte, si fermerebbe a meta'. */
    check("memset16 conta elementi, non byte", untouched_from(12));
    check("memset16 non tocca le guardie", guards_intact());

    arena_reset();
    cells = (uint16_t *)area();
    memset16(cells, 0xBEEF, 0);
    check("memset16 con count=0 non scrive nulla", untouched_from(0));

    arena_reset();
    cells = (uint16_t *)area();
    check("memset16 restituisce dest", memset16(cells, 0, 2) == (void *)cells);
}

/* I controlli sono scritti confrontando con costanti intere, non con una
   variabile di tipo dichiarato: cosi' restano validi sia con la firma attuale
   (int) sia se un giorno diventasse size_t come vuole lo standard. */
static void test_strlen(void)
{
    char scritta[128];      /* non AREA: qui sotto ci scriviamo 101 byte */
    size_t i;
    int ok;

    /* Il caso che si sbaglia scrivendo il ciclo come do-while: la stringa vuota
       ha lunghezza zero, non uno. */
    check("strlen(\"\") e' 0", strlen("") == 0);

    check("strlen(\"a\") e' 1", strlen("a") == 1);
    check("strlen(\"abc\") e' 3", strlen("abc") == 3);

    /* Si ferma al PRIMO NUL, non alla fine dell'array. L'arena e' piena di
       0xAA, quindi un ciclo che tirasse oltre il terminatore conterebbe altri
       61 byte prima di uscire dall'area utile — e probabilmente oltre. */
    arena_reset();
    area()[0] = 'a';
    area()[1] = 'b';
    area()[2] = '\0';
    check("strlen si ferma al primo NUL, non alla fine dell'array",
          strlen((const char *)area()) == 2);

    /* Non conta il terminatore, verificato come lo userebbe un chiamante:
       il byte all'indice strlen(s) deve essere proprio il NUL. E' la proprieta'
       che serve a chi usa il risultato per copiare, ed e' l'errore che fa
       scrivere fuori di uno. */
    arena_reset();
    memcpy(area(), "waltex", 7);
    check("il byte all'indice strlen(s) e' il NUL",
          area()[strlen((const char *)area())] == '\0');

    /* Una stringa lunga: una lunghezza tenuta in un contatore troppo piccolo,
       o un limite fisso nascosto, si vedono solo qui. 100 caratteri sono
       dell'ordine di una riga di comando vera (LINE_MAX e' 128). */
    for (i = 0; i < 100; i++)
        scritta[i] = 'x';
    scritta[100] = '\0';
    check("strlen conta una stringa di 100 caratteri", strlen(scritta) == 100);

    /* Non modifica cio' che legge. Il parametro e' const, quindi il compilatore
       lo impedirebbe in C — ma un cast dentro l'implementazione lo aggirerebbe
       in silenzio, e questo controllo no. */
    ok = 1;
    for (i = 0; i < 100; i++)
        if (scritta[i] != 'x')
            ok = 0;
    check("strlen non modifica la stringa", ok && scritta[100] == '\0');
}

/* strpos non e' standard: l'equivalente C e' strchr, che ritorna un puntatore.
   Il contratto e' in memory.h — indice della prima occorrenza, -1 se assente. */
static void test_strpos(void)
{
    check("strpos trova un carattere in mezzo", strpos("abc", 'b') == 1);
    check("strpos trova il primo carattere", strpos("abc", 'a') == 0);
    check("strpos trova l'ultimo carattere", strpos("abc", 'c') == 2);

    /* La PRIMA occorrenza, non l'ultima: un ciclo che non esce appena trova
       restituisce 3 invece di 1. */
    check("strpos ritorna la prima occorrenza", strpos("abab", 'b') == 1);

    check("strpos su carattere assente ritorna -1", strpos("abc", 'z') == -1);
    check("strpos su stringa vuota ritorna -1", strpos("", 'a') == -1);

    /* Il controllo che prende il bug piu' costoso di questa funzione: fermarsi
       al terminatore.

       L'arena e' piena di 0xAA, e ci mettiamo "ab" seguito dal NUL. Cercare
       0xAA DEVE dare -1: se il ciclo confronta il puntatore invece del
       carattere — while (s != '\0') invece di while (*s != '\0') — non si ferma
       mai al terminatore, cammina nella memoria adiacente, e qui trova un 0xAA
       al terzo byte restituendo 2. Un indice plausibile e sbagliato, che e'
       peggio di un crash perche' nessuno se ne accorge. */
    arena_reset();
    area()[0] = 'a';
    area()[1] = 'b';
    area()[2] = '\0';
    check("strpos si ferma al terminatore e non legge oltre",
          strpos((const char *)area(), (char)0xAA) == -1);

    /* Il terminatore non e' cercabile, ed e' una scelta scritta in memory.h:
       strchr lo troverebbe, qui no. La funzione serve per caratteri visibili.

       I due controlli restano invece di sparire perche' un angolo documentato
       vale solo se qualcosa lo verifica: senza, il giorno che l'implementazione
       cambiasse e cominciasse a "trovare" il terminatore, nessuno se ne
       accorgerebbe finche' un chiamante non ricevesse un indice al posto di
       -1. */
    check("strpos non trova il terminatore, per contratto",
          strpos("abc", '\0') == -1);
    check("strpos('\\0') su stringa vuota ritorna -1", strpos("", '\0') == -1);
}

/* La conversione a minuscola. Serve a shell_parse_hex per accettare "FF" con
   una sola tabella di cifre invece di due.

   ATTENZIONE al nome: in C standard tolower sta in <ctype.h> con firma
   int tolower(int), incompatibile con questa. Finche' nessuno include ctype.h
   non succede niente, ma e' un errore di compilazione in attesa. */
static void test_tolower(void)
{
    /* La direzione: 'A' e' 65, 'a' e' 97, quindi si AGGIUNGE 32. Le maiuscole
       hanno codici piu' bassi delle minuscole, che e' il contrario di come suona
       "portare a minuscola", ed e' l'errore che si fa. */
    check("tolower('A') e' 'a'", tolower('A') == 'a');
    check("tolower('Z') e' 'z'", tolower('Z') == 'z');
    check("tolower('M') e' 'm'", tolower('M') == 'm');

    /* Idempotente: cio' che e' gia' minuscolo non si tocca. Un'implementazione
       che spostasse di 32 senza guardare l'intervallo qui uscirebbe dalle
       lettere. */
    check("tolower('a') resta 'a'", tolower('a') == 'a');
    check("tolower('z') resta 'z'", tolower('z') == 'z');

    /* I quattro caratteri appena FUORI dai due intervalli, che e' dove un
       >= scritto > (o viceversa) si manifesta e da nessun'altra parte:
         '@' = 64, subito prima di 'A'      '[' = 91, subito dopo 'Z'
         '`' = 96, subito prima di 'a'      '{' = 123, subito dopo 'z' */
    check("tolower('@') non cambia, e' appena prima di 'A'", tolower('@') == '@');
    check("tolower('[') non cambia, e' appena dopo 'Z'", tolower('[') == '[');
    check("tolower('`') non cambia, e' appena prima di 'a'", tolower('`') == '`');
    check("tolower('{') non cambia, e' appena dopo 'z'", tolower('{') == '{');

    check("tolower('5') non cambia", tolower('5') == '5');
    check("tolower(' ') non cambia", tolower(' ') == ' ');
    check("tolower('\\0') non cambia", tolower('\0') == '\0');

    /* char e' con segno su x86, quindi 0xC0 e' un valore NEGATIVO. Un confronto
       d'intervallo scritto assumendo valori positivi si comporta in modo
       diverso qui che sui caratteri ASCII. */
    check("tolower su un byte con il bit alto non cambia",
          tolower((char)0xC0) == (char)0xC0);
}

/* Il confronto fra stringhe, che da M7 regge la ricerca nella tabella dei
   comandi e da M9 reggera' i nomi dei file nel VFS.

   Tutti i controlli guardano il SEGNO, non il valore: lo standard promette
   "negativo, zero o positivo", non -1/0/1, e un'implementazione che
   restituisce la differenza fra i due byte e' conforme quanto una che
   restituisce -1. Un test che pretendesse -1 verificherebbe un dettaglio che
   nessuno ha promesso. */
static void test_strcmp(void)
{
    char corta[AREA];

    check("stringhe uguali danno 0", strcmp("abc", "abc") == 0);
    check("due stringhe vuote danno 0", strcmp("", "") == 0);

    /* L'ordine e' quello dei byte: 'a' e' 97, 'b' e' 98. */
    check("\"a\" viene prima di \"b\"", strcmp("a", "b") < 0);
    check("\"b\" viene dopo di \"a\"", strcmp("b", "a") > 0);

    /* Conta solo il PRIMO byte che differisce: quello che viene dopo non deve
       influire, ed e' l'errore di chi somma o accumula le differenze invece di
       fermarsi. */
    check("conta solo il primo byte diverso", strcmp("axx", "byy") < 0);

    /* Un prefisso viene prima della stringa che lo estende, perche' il
       terminatore (0) e' minore di qualunque carattere. */
    check("\"ab\" viene prima di \"abc\"", strcmp("ab", "abc") < 0);
    check("\"abc\" viene dopo di \"ab\"", strcmp("abc", "ab") > 0);
    check("la stringa vuota viene prima di tutto", strcmp("", "a") < 0);
    check("tutto viene dopo la stringa vuota", strcmp("a", "") > 0);

    /* Le maiuscole vengono prima: 'A' e' 65, 'a' e' 97. Conta per la tabella
       dei comandi — "Help" non deve corrispondere a "help". */
    check("maiuscole e minuscole sono diverse", strcmp("Help", "help") < 0);

    /* Non legge oltre il terminatore. L'arena e' piena di 0xAA: se il confronto
       continuasse dopo il NUL, "ab" e il contenuto dell'arena divergerebbero al
       terzo byte e il risultato non sarebbe zero. */
    arena_reset();
    area()[0] = 'a';
    area()[1] = 'b';
    area()[2] = '\0';
    check("strcmp si ferma al terminatore",
          strcmp((const char *)area(), "ab") == 0);

    /* Il byte si confronta come UNSIGNED char, e lo standard e' esplicito.
       0xC0 vale 192, quindi viene DOPO 0x40 che vale 64. Un'implementazione che
       usa char — con segno su x86 — vede -64 e risponde il contrario.

       Per la shell non capiterebbe mai, perche' i comandi sono ASCII. Per i nomi
       dei file di M9 puo' capitare. */
    corta[0] = (char)0xC0;
    corta[1] = '\0';
    check("il confronto e' fra unsigned char, non char",
          strcmp(corta, "\x40") > 0);
}

/* strchr, che a differenza di strpos e' la funzione STANDARD: restituisce un
   puntatore e include il terminatore nella ricerca.

   I controlli confrontano puntatori con s + indice invece di indici: e' cio' che
   la funzione promette, e verificare un indice vorrebbe dire fidarsi che la
   sottrazione sia giusta prima di aver verificato il puntatore. */
static void test_strchr(void)
{
    const char *s = "abc";
    const char *ab = "abab";
    char *r;

    check("strchr trova un carattere in mezzo", strchr(s, 'b') == s + 1);
    check("strchr trova il primo carattere", strchr(s, 'a') == s + 0);
    check("strchr trova l'ultimo carattere", strchr(s, 'c') == s + 2);

    /* La PRIMA occorrenza: un ciclo che non esce appena trova punterebbe alla
       seconda. */
    check("strchr ritorna la prima occorrenza", strchr(ab, 'b') == ab + 1);

    check("strchr su carattere assente ritorna 0", strchr(s, 'z') == 0);
    check("strchr su stringa vuota ritorna 0", strchr("", 'a') == 0);

    /* Qui strchr DIVERGE da strpos, e la divergenza e' deliberata: il contratto
       standard include il terminatore nella ricerca. Il puntatore restituito
       deve puntare a un NUL vero. */
    r = strchr(s, '\0');
    check("strchr TROVA il terminatore", r == s + 3);
    check("e il byte puntato e' davvero il NUL", r != 0 && *r == '\0');
    check("strchr('\\0') su stringa vuota punta al suo terminatore",
          strchr("", '\0') != 0);

    /* La coppia che inchioda la divergenza. Senza questi due controlli, il
       giorno che qualcuno "uniformasse" le due funzioni nessuno se ne
       accorgerebbe — e vfs_resolve, che usa strchr per trovare la fine di un
       componente, comincerebbe a non trovare la fine dell'ultimo. */
    check("dove entrambe hanno senso, concordano",
          strchr(s, 'b') - s == strpos(s, 'b'));
    check("sul terminatore divergono, come documentato",
          strchr(s, '\0') != 0 && strpos(s, '\0') == -1);

    /* Non legge oltre il terminatore. L'arena e' piena di 0xAA: se il ciclo non
       si fermasse al NUL, troverebbe un 0xAA subito dopo e restituirebbe un
       puntatore fuori dalla stringa. */
    arena_reset();
    area()[0] = 'a';
    area()[1] = 'b';
    area()[2] = '\0';
    check("strchr non cerca oltre il terminatore",
          strchr((const char *)area(), (char)0xAA) == 0);

    /* Non modifica cio' che legge: il parametro e' const, e un cast dentro
       l'implementazione aggirerebbe il compilatore ma non questo controllo. */
    arena_reset();
    memcpy(area(), "waltex", 7);
    strchr((const char *)area(), 'x');
    check("strchr non modifica la stringa",
          area()[0] == 'w' && area()[5] == 'x' && area()[6] == '\0');
}

int main(void)
{
    test_memcpy();
    test_memset();
    test_memset16();
    test_strlen();
    test_strpos();
    test_tolower();
    test_strcmp();
    test_strchr();

    if (failures == 0) {
        printf("tutti i test di memory.c passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
