#ifndef GPIO_BCM2711_H
#define GPIO_BCM2711_H

#include <stdint.h>
#include "../include/drs_sync_config.h"

/*
 * Minimal BCM2711 GPIO driver via /dev/gpiomem mmap.
 * No root required — user must be in the gpio group.
 */

int  gpio_init(void);   /* mmap /dev/gpiomem, configure pins as output */
void gpio_close(void);  /* munmap */

void gpio_set(int pin);    /* set pin HIGH */
void gpio_clear(int pin);  /* set pin LOW  */

#endif /* GPIO_BCM2711_H */
