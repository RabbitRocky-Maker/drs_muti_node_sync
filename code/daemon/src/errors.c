#include "errors.h"
#include "net_io.h"   /* for mono_raw_ns() */

#include <stdatomic.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>

static drs_error_ring_t *g_ring = NULL;
static int                g_fd   = -1;

int errors_init(void)
{
    g_fd = open(DRS_SHM_ERRORS_PATH,
                O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (g_fd < 0)
        return -1;

    size_t sz = sizeof(drs_error_ring_t);
    if (ftruncate(g_fd, (off_t)sz) < 0) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    void *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (m == MAP_FAILED) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    g_ring = (drs_error_ring_t *)m;
    atomic_store_explicit(&g_ring->head, 0, memory_order_relaxed);
    memset(g_ring->slots, 0, sizeof(g_ring->slots));
    return 0;
}

void errors_close(void)
{
    if (g_ring) {
        munmap(g_ring, sizeof(drs_error_ring_t));
        g_ring = NULL;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

void errors_push(uint32_t code, const char *msg)
{
    if (!g_ring)
        return;

    uint32_t idx = atomic_fetch_add_explicit(&g_ring->head, 1, memory_order_relaxed)
                   % DRS_ERROR_RING_SLOTS;

    drs_error_entry_t *e = &g_ring->slots[idx];
    e->code         = code;
    e->timestamp_ns = mono_raw_ns();
    if (msg) {
        strncpy(e->msg, msg, sizeof(e->msg) - 1);
        e->msg[sizeof(e->msg) - 1] = '\0';
    } else {
        e->msg[0] = '\0';
    }
    /* count is cumulative per slot — just overwrite */
    e->count++;

    atomic_thread_fence(memory_order_release);
}

int errors_drain(drs_error_entry_t *out, int max, uint32_t *offset)
{
    if (!g_ring || max <= 0)
        return 0;

    uint32_t head = atomic_load_explicit(&g_ring->head, memory_order_acquire);
    int      n    = 0;

    while (*offset < head && n < max) {
        uint32_t idx = (*offset) % DRS_ERROR_RING_SLOTS;
        out[n++] = g_ring->slots[idx];
        (*offset)++;
    }
    return n;
}
