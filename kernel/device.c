#include "device.h"
#include "memory.h"

static int ndev;
static struct device devs[MAX_DEVICES];

void device_init(void)
{
    ndev = 0;
}

int device_count(void)
{
    return ndev;
}

int device_register(const struct device *d)
{
    if (ndev == MAX_DEVICES) {
        return -1;
    }

    int p = strpos(d->name, '\0');
    if (p > DEV_NAME_MAX) {
        return 1;
    }


    size_t lname = strlen(d->name);
    if (lname >= DEV_NAME_MAX || lname == 0) {
        return -1;
    }

    for (uint8_t i=0; i<ndev; i++) {
        if (strcmp(d->name, devs[i].name) == 0) {
            return -1;
        }
    }

    if (d->write == 0 && d->read == 0) {
        return -1;
    }

    memcpy(&(devs[ndev]), d, sizeof(struct device));
    ++ndev;

    return 0;
}

struct device *device_find(const char *name)
{
    for (uint8_t i=0; i<device_count(); i++) {
        struct device *d = device_at(i);
        if (strcmp(d->name, name) == 0) {
            return d;
        } 
    }

    return 0;
}

struct device *device_by_id(uint16_t major, uint16_t minor)
{
    for (uint8_t i=0; i<device_count(); i++) {
        struct device *d = device_at(i);
        if (d->major == major && d->minor == minor) {
            return d;
        } 
    }

    return 0;
}

struct device *device_at(int i)
{
    if (i<0) {
        return 0;
    }

    if (i >= device_count()) {
        return 0;
    }

    return &(devs[i]);
}