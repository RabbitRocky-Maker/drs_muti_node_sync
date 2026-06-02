#include "gpio_bcm2711.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

/*
 * BCM2711 GPIO register layout (base = 0xFE200000 physical, but /dev/gpiomem
 * maps the GPIO peripheral block starting at offset 0).
 */
#define GPIO_PAGE_SIZE     0x1000
#define GPFSEL1_OFFSET     1    /* Function select 1: GPIO 10–19 */
#define GPFSEL2_OFFSET     2    /* Function select 2: GPIO 20–29 */
#define GPSET0_OFFSET      7    /* Set  register: GPIO 0–31 */
#define GPCLR0_OFFSET      10   /* Clear register: GPIO 0–31 */

/* Function select: 001 = output */
#define FSEL_OUTPUT        0x1u

static volatile uint32_t *g_gpio = NULL;
static int                g_fd   = -1;

static void fsel_output(int pin)
{
    /* Each GPFSEL register covers 10 pins, 3 bits each */
    int reg   = pin / 10;      /* 0=GPFSEL0, 1=GPFSEL1, 2=GPFSEL2 */
    int shift = (pin % 10) * 3;
    volatile uint32_t *fsel = g_gpio + reg;
    uint32_t v = *fsel;
    v &= ~(0x7u << shift);         /* clear 3 bits */
    v |= (FSEL_OUTPUT << shift);   /* set output */
    *fsel = v;
}

int gpio_init(void)
{
    g_fd = open("/dev/gpiomem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (g_fd < 0)
        return -1;

    void *m = mmap(NULL, GPIO_PAGE_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (m == MAP_FAILED) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    g_gpio = (volatile uint32_t *)m;

    fsel_output(DRS_GPIO_PULSE_PIN);
    fsel_output(DRS_GPIO_HEALTH_PIN);

    /* Start both pins LOW */
    g_gpio[GPCLR0_OFFSET] = (1u << DRS_GPIO_PULSE_PIN) | (1u << DRS_GPIO_HEALTH_PIN);

    return 0;
}

void gpio_close(void)
{
    if (g_gpio) {
        /* Drive both pins LOW on shutdown */
        g_gpio[GPCLR0_OFFSET] = (1u << DRS_GPIO_PULSE_PIN) | (1u << DRS_GPIO_HEALTH_PIN);
        munmap((void *)g_gpio, GPIO_PAGE_SIZE);
        g_gpio = NULL;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

void gpio_set(int pin)
{
    if (g_gpio)
        g_gpio[GPSET0_OFFSET] = (1u << pin);
}

void gpio_clear(int pin)
{
    if (g_gpio)
        g_gpio[GPCLR0_OFFSET] = (1u << pin);
}
