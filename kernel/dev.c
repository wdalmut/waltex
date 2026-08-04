#include "dev.h"
#include "memory.h"

static uint8_t curr = 0;
static struct dev_entry entries[DEV_MAX];

void dev_init(void)
{
    curr = 0;
    memset(entries, 0, sizeof(struct dev_entry)*DEV_MAX);
}

int dev_register(const struct dev_entry *e)
{
    const struct dev_entry *is_exists = dev_by_id(e->major, e->minor);

    if (is_exists) {
        return -1;
    }

    if (curr >= DEV_MAX) {
        return -1;
    }

    if (e->kind != DEV_CHAR && e->kind != DEV_BLOCK) {
        return -1;
    }

    const char *n = e->name;
    while (*n != '\0') {
        ++n;
        if (n - e->name >= DEV_NAME_MAX) {
            return -1;
        }
    }

    if (strcmp(e->name, "") == 0) {
        return -1;
    }

    for (uint8_t i=0; i<curr; i++) {
        if (strcmp(entries[i].name, e->name) == 0) {
            return -1;
        }
    }
    
    memcpy(&entries[curr], e, sizeof(struct dev_entry));
    ++curr;

    return 0;
}

int dev_lookup_index(const char *name)
{
    for (uint8_t i=0; i<curr; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

const struct dev_entry *dev_get(int i)
{
    if (i >= (int)curr || i < 0) {
        return 0;
    }

    return &(entries[i]);
}

const struct dev_entry *dev_by_id(uint16_t major, uint16_t minor)
{
    for (uint8_t i=0; i<curr; i++) {
        if (entries[i].major == major && entries[i].minor == minor) {
            return &(entries[i]);
        }
    }

    return 0;
}

int dev_count(void)
{
    return curr;
}