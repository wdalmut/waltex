#include "serial.h"
#include "io.h"
#include "device.h"
#include "panic.h"

/* COM1. Driver in polling: nessun interrupt, nessun buffer. È il canale su cui
   girano i test, quindi conta che sia semplice e sempre funzionante, non che
   sia efficiente. */
#define COM1 0x3F8

#define REG_DATA         (COM1 + 0)
#define REG_INT_ENABLE   (COM1 + 1)
#define REG_FIFO_CTRL    (COM1 + 2)
#define REG_LINE_CTRL    (COM1 + 3)
#define REG_MODEM_CTRL   (COM1 + 4)
#define REG_LINE_STATUS  (COM1 + 5)

#define LSR_TX_EMPTY 0x20

/* L'adattatore fra la firma uniforme del device layer e la funzione che questo
   driver ha già. È tutto quello che serve: un ciclo.

   Il tranello sta nella prima riga. buf è un `const void *`, e su un void non si
   può indicizzare né fare aritmetica — il compilatore non sa di quanti byte
   avanzare. Serve il cast a un tipo di dimensione nota PRIMA di scrivere p[i].
   È lo stesso errore che in M1 faceva `(void *)VGA_MEM + VGA_COLS` avanzare di
   80 byte invece di 80 celle.

   d non serve a niente qui, e la firma lo impone comunque: in M10 il driver ATA
   iscriverà due dischi con la STESSA funzione read, e sarà d — con il suo priv —
   a dire quale dei due. Il cast a void zittisce -Wextra senza nascondere nulla.

   Non interpreta i byte: nessuna traduzione di '\n', nessun filtro sui non
   stampabili. L'interpretazione sta un livello sotto, in serial_putc, e farla
   anche qui la farebbe accadere due volte. */
static int serial_dev_write(struct device *d, const void *buf, uint32_t n)
{
    const char *p = (const char *)buf;
    uint32_t i;

    (void)d;

    for (i = 0; i < n; i++)
        serial_putc(p[i]);

    /* Tutti i byte scritti: la seriale in polling non può fallire, perché
       serial_putc aspetta che il registro sia libero e poi scrive. n == 0 è
       legittimo e ritorna 0 — un ciclo scritto come do…while lo sbaglierebbe. */
    return (int)n;
}

void serial_init(void)
{
    /* La struct è LOCALE, e non è una distrazione: device_register copia, quindi
       questa memoria può sparire appena serial_init ritorna. Se il registro
       conservasse il puntatore invece di copiare, il guasto si manifesterebbe
       esattamente qui — ed è il caso che test_device costruisce modificando la
       sorgente dopo l'iscrizione.

       L'inizializzatore designato azzera per intero ciò che non nomina, quindi
       read e priv finiscono a zero senza scriverlo. Conta: una struct locale non
       inizializzata contiene spazzatura dello stack, e read conterrebbe un
       indirizzo casuale su cui qualcuno prima o poi salterebbe. */
    struct device dev = {
        .name  = "ttyS0",
        .major = 4,
        .minor = 64,
        .write = serial_dev_write
        /* .read resta 0: la seriale in questo progetto non si legge, e un
           puntatore nullo dice "operazione non supportata" — non "errore". È la
           stessa convenzione di exc_handlers[vec] == 0 in idt.c. */
    };

    outb(REG_INT_ENABLE, 0x00);   /* nessun interrupt: scriviamo in polling  */
    outb(REG_LINE_CTRL,  0x80);   /* DLAB=1: i primi due registri diventano
                                     il divisore del baud rate              */
    outb(REG_DATA,       0x03);   /* divisore 3 -> 38400 baud, byte basso    */
    outb(REG_INT_ENABLE, 0x00);   /* byte alto del divisore                  */
    outb(REG_LINE_CTRL,  0x03);   /* DLAB=0, 8 bit, no parità, 1 stop bit    */
    outb(REG_FIFO_CTRL,  0xC7);   /* FIFO on, svuotate, soglia 14 byte       */
    outb(REG_MODEM_CTRL, 0x0B);   /* DTR e RTS attivi, OUT2 abilitato        */

    /* Il ritorno si verifica: un'iscrizione che fallisce al boot deve essere
       rumorosa. assert è sempre attiva in questo progetto e chiama panic, quindi
       il guasto diventa un messaggio leggibile invece di un dispositivo che in
       M9 non c'è senza spiegazione. */
    assert(device_register(&dev) == 0);
}

void serial_putc(char c)
{
    while ((inb(REG_LINE_STATUS) & LSR_TX_EMPTY) == 0)
        ;
    outb(REG_DATA, (uint8_t)c);
}
