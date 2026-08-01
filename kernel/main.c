#include "types.h"
#include "serial.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "task.h"
#include "shell.h"
#include "demo.h"
#include "device.h"
#include "ata.h"
#include "devfs.h"
#include "vfs.h"
#include "selftest.h"
#include "kprintf.h"

/* Valore che un loader Multiboot 1 conforme lascia in eax. Se non corrisponde,
   non sappiamo nulla di affidabile sull'ambiente in cui siamo partiti. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* I due task di prova stavano qui fino a M6b. Da M7 vivono in kernel/demo.c,
   silenziosi finche' il comando "spin" non li accende: la loro stampa continua
   era il punto della milestone della prelazione, ma rende un prompt
   illeggibile. Cosi' main.c torna a essere la sola sequenza di boot. */

void kmain(uint32_t magic, void *mbinfo)
{
   int failures;

   (void)mbinfo;

   device_init();

   vga_init();
   serial_init();

   kprintf("waltex: booting\n");

   if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
      kprintf("waltex: magic Multiboot errato: %x\n", magic);
      return;
   }
   kprintf("waltex: multiboot ok\n");

   /* Da qui in poi la CPU usa la nostra tabella dei descrittori invece di
      quella del bootloader. La riga dopo non e' decorativa: se la GDT fosse
      malformata, la CPU non arriverebbe a eseguirla. */
   gdt_init();
   kprintf("waltex: gdt caricata\n");

   /* Prima la tabella dei gestori, poi il chip che generera' gli interrupt.
      Nessuna sti in M3: installiamo la capacita' di gestirli, non ne
      riceviamo ancora. La prima sorgente reale e' il timer, in M4. */
   idt_init();
   pic_init();
   kprintf("waltex: idt e pic pronti\n");

   timer_init(100);
   keyboard_init();
   kprintf("waltex: timer a 100 Hz\n");

   /* Il disco. In polling non ha vincoli d'ordine veri — la posizione qui e'
      solo la convenzione del progetto, ogni sottosistema con la sua *_init()
      esplicita. Un canale vuoto e' legittimo: ata_drive(0) puo' essere 0, e la
      riga sotto e' scritta per non dereferenziarlo. */
   ata_init();

   if (ata_drive(0) != 0)
      kprintf("waltex: disco %s, %d settori\n",
              ata_drive(0)->name, (int)ata_drive(0)->nsectors);
   else
      kprintf("waltex: nessun disco sul canale primario\n");

   /* Il filesystem, e l'ordine e' vincolato da entrambi i lati.

      devfs_init LEGGE il registro dei dispositivi, quindi va dopo tutte le
      *_init() dei driver: chiamata prima, device_count() darebbe zero, /dev
      sarebbe vuota, e non ci sarebbe nessun errore da nessuna parte.
      vfs_init prende la radice da devfs, quindi va dopo devfs_init.

      Insieme al vincolo opposto di device_init() — prima di tutti, perche' sono
      i driver a iscriversi — questi due incorniciano le inizializzazioni dei
      driver da sotto. */
   devfs_init();
   vfs_init(devfs_root());
   kprintf("waltex: /dev con %d dispositivi\n", device_count());

   /* La prima sti del progetto. Da questa istruzione il kernel ha due flussi
      di esecuzione: questo, e il gestore del timer che lo interrompe cento
      volte al secondo. Tutto cio' che e' condiviso fra i due va trattato di
      conseguenza. */
   __asm__ volatile ("sti");

   failures = selftest_run();
   if (failures != 0) {
      kprintf("waltex: %d selftest falliti\n", failures);
      return;
   }
   kprintf("waltex: selftest ok\n");

   /* Ultima riga di kmain. Il marker che lo smoke test cerca deve significare
      "tutto quello che precede ha funzionato": spostarlo piu' in alto lo
      trasforma in una decorazione che resta verde anche a kernel rotto.

      E' anche il marker che tests/shell.sh e tests/keyboard.sh aspettano prima
      di digitare, invece del prompt: il prompt non ha un ritorno a capo in
      fondo, quindi cercarlo con grep su un file che sta crescendo sarebbe una
      corsa. */
   kprintf("waltex: M7 ok\n");

   /* Da qui kmain e' il task 0: il suo stack e' quello montato da _start, e
      il suo esp verra' scritto dal primo task_switch che lo abbandona.

      kmain non ritorna piu' da M4, e il cli; hlt in fondo a _start resta come
      rete di sicurezza per il caso "e' tornato, non doveva".

      In M6a il ciclo di idle non poteva dormire: in un sistema cooperativo un
      task che si addormenta non cede il controllo a nessuno, quindi doveva
      girare e chiamare task_yield, con la CPU al 100%. Con la prelazione
      quel vincolo cade. */
   task_init();

   /* shell_init prima di task_create, non dopo: collega il sink di eco
      all'editor di riga, e shell_task se lo aspetta gia' collegato. */
   shell_init();
   task_create(shell_task);

   /* I due task di prova, che restano silenziosi finche' non arriva "spin". */
   demo_tasks_init();

   /* Il ciclo di idle NON legge piu' la tastiera, e non e' una semplificazione:
      il ring buffer ammette un solo consumatore, e da M7 quel consumatore e' la
      shell. Leggere da qui in parallelo farebbe sparire caratteri a caso —
      digitando "echo" la shell ne vedrebbe "eh" e questo ciclo stamperebbe
      "co".

      Il consumatore si SPOSTA, non si aggiunge.

      Per il resto vale quello che valeva da M6b: con la prelazione l'idle puo'
      dormire, perche' il timer sveglia e commuta comunque. */
   for (;;)
      __asm__ volatile ("hlt");
}
