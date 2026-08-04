#include "types.h"
#include "keyboard.h"
#include "ring.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "chardev.h"
#include "devio.h"
#include "panic.h"

static const char scancode_to_ascii_normal[128] = {
    [0x01] = 27,   // ESC
    [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',  [0x06] = '5',
    [0x07] = '6',  [0x08] = '7',  [0x09] = '8',  [0x0A] = '9',  [0x0B] = '0',
    [0x0C] = '-',  [0x0D] = '=',  
    [0x0E] = '\b', // Backspace
    [0x0F] = '\t', // Tab
    
    // Riga QWERTY minuscola + Simboli associati
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',  [0x14] = 't',
    [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',  [0x18] = 'o',  [0x19] = 'p',
    [0x1A] = '[',  [0x1B] = ']',  
    [0x1C] = '\n', // Invio
    
    // Riga ASDF minuscola + Simboli associati
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',  [0x22] = 'g',
    [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',  [0x26] = 'l',  
    [0x27] = ';',  [0x28] = '\'', [0x29] = '`',
    
    // Riga ZXCV minuscola + Simboli associati
    [0x2B] = '\\', // Backslash (tasto accanto a Invio o Z a seconda del layout)
    [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',  [0x30] = 'b',
    [0x31] = 'n',  [0x32] = 'm',  
    [0x33] = ',',  [0x34] = '.',  [0x35] = '/',
    
    [0x39] = ' ',  // Spazio
    
    // Tastierino Numerico (Keypad) - Simboli matematici
    [0x37] = '*',  [0x4A] = '-',  [0x4E] = '+',  [0x4C] = '5',
    [0x47] = '7',  [0x48] = '8',  [0x49] = '9',
    [0x4B] = '4',                 [0x4D] = '6',
    [0x4F] = '1',  [0x50] = '2',  [0x51] = '3',
    [0x52] = '0',  [0x53] = '.'
};

// 2. Tabella Shift: Lettere MAIUSCOLE e tutti i SIMBOLI secondari
static const char scancode_to_ascii_shift[128] = {
    [0x01] = 27,   // ESC
    [0x02] = '!',  [0x03] = '@',  [0x04] = '#',  [0x05] = '$',  [0x06] = '%',
    [0x07] = '^',  [0x08] = '&',  [0x09] = '*',  [0x0A] = '(',  [0x0B] = ')',
    [0x0C] = '_',  [0x0D] = '+',  
    [0x0E] = '\b', 
    [0x0F] = '\t',
    
    // Riga QWERTY maiuscola + Simboli modificati
    [0x10] = 'Q',  [0x11] = 'W',  [0x12] = 'E',  [0x13] = 'R',  [0x14] = 'T',
    [0x15] = 'Y',  [0x16] = 'U',  [0x17] = 'I',  [0x18] = 'O',  [0x19] = 'P',
    [0x1A] = '{',  [0x1B] = '}',  
    [0x1C] = '\n',
    
    // Riga ASDF maiuscola + Simboli modificati
    [0x1E] = 'A',  [0x1F] = 'S',  [0x20] = 'D',  [0x21] = 'F',  [0x22] = 'G',
    [0x23] = 'H',  [0x24] = 'J',  [0x25] = 'K',  [0x26] = 'L',  
    [0x27] = ':',  [0x28] = '"',  [0x29] = '~',
    
    // Riga ZXCV maiuscola + Simboli modificati
    [0x2B] = '|',  // Pipe character (Shift + Backslash)
    [0x2C] = 'Z',  [0x2D] = 'X',  [0x2E] = 'C',  [0x2F] = 'V',  [0x30] = 'B',
    [0x31] = 'N',  [0x32] = 'M',  
    [0x33] = '<',  [0x34] = '>',  [0x35] = '?',
    
    [0x39] = ' ',
    
    // Tastierino Numerico con Shift (spesso mantiene gli stessi simboli o numeri)
    [0x37] = '*',  [0x4A] = '-',  [0x4E] = '+',  [0x4C] = '5',
    [0x47] = '7',  [0x48] = '8',  [0x49] = '9',
    [0x4B] = '4',                 [0x4D] = '6',
    [0x4F] = '1',  [0x50] = '2',  [0x51] = '3',
    [0x52] = '0',  [0x53] = '.'
};

static struct ring r;

static int skip_next = 0;
static int shift_pressed = 0;

static void keyboard_handler(struct regs *regs)
{
    (void)regs;

    uint8_t kbd_data = inb(KBD_DATA);
    
    int scancode;

    if (skip_next != 0) {
        skip_next = 0;
    } else if (kbd_data == KBD_SC_EXTENDED) {
        skip_next = 1;
    } else if (kbd_data == KBD_SC_LSHIFT || kbd_data == KBD_SC_RSHIFT) {
        shift_pressed = 1;
    } else {
        scancode = scancode_to_char(kbd_data, shift_pressed);

        if (scancode >= 0) {
            ring_push(&r, (uint8_t)scancode);
        }
        
        shift_pressed = 0;
    }
}


int scancode_to_char(uint8_t scancode, int shift)
{
    int c;

    if (scancode & KBD_BREAK_BIT) {
        return -1;
    }

    if (shift != 0) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii_normal[scancode];
    }
    
    if (c == 0) {
        return -1;
    }

    return c;
}

static int kbd_dev_read(struct chardev *d, void *buf, uint32_t n)
{
    char *p = (char *)buf;
    int i = 0, c = 0;

    (void)d;

    if (n!=0) {
        while ((c = keyboard_getchar()) != -1) {
            *p = c;
            ++p; ++i;
            if (i == (int)n) {
                break;
            }
        }
    }

    return i;
}

void keyboard_init(void)
{
    /* STATIC per la stessa ragione di vga_init e serial_init: da M11e il registry
       conserva dev_entry.impl, cioe' il puntatore, non una copia della struct.
       Con una locale l'assert passa e il guasto arriva alla prima read.

       .write resta 0, e dichiara che la tastiera non si scrive: un puntatore
       nullo significa "operazione non supportata", non "errore". */
    static struct chardev dev = {
        .read = kbd_dev_read
    };

    ring_init(&r);

    irq_register(1, keyboard_handler);
    pic_mask(1, 0);

    assert(chardev_register("kbd", 13, 64, &dev) == 0);
}

int keyboard_getchar(void)
{
    int v = ring_pop(&r);

    return v;
}