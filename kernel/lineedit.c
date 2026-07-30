#include "lineedit.h"
#include "types.h"
#include "memory.h"

void lineedit_init(struct lineedit *le, void (*echo)(char c))
{
    le->len = 0;
    le->echo = echo;
    for (uint8_t i=0; i<LINE_MAX; i++) {
        le->buf[i] = '\0';
    }
}

int lineedit_putc(struct lineedit *le, char c)
{
    int ret = 0;

    uint8_t ll = le->len;

    if (c == '\b') {
        if (ll > 0) {
            le->buf[ll-1] = '\0';
            le->len--;

            if (le->echo) {
                le->echo('\b');
                le->echo(' ');
                le->echo('\b');
            }
            
        }    
    } else if (c >= 32) {
        if (ll+1 < LINE_MAX) {
            le->buf[ll] = c;
            le->buf[ll+1] = '\0';

            le->len++;

            if (le->echo) {
                le->echo(c);
            }
        }
    }

    if (c == '\n') {
        if (le->echo) {
            le->echo('\n');
        }
        
        ret = 1;
    }

    if (ll-1 >= LINE_MAX) {
        ret = 1;
    }

    return ret;
}

void lineedit_reset(struct lineedit *le)
{
    le->len = 0;
    for (uint8_t i=0; i<LINE_MAX; i++) {
        le->buf[i] = '\0';
    }
}
