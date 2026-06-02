#include "telemetry.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

static drs_telemetry_t *g_tel = NULL;
static int               g_fd  = -1;

int telemetry_init(void)
{
    g_fd = open(DRS_SHM_STATE_PATH,
                O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (g_fd < 0)
        return -1;

    if (ftruncate(g_fd, (off_t)sizeof(drs_telemetry_t)) < 0) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    void *m = mmap(NULL, sizeof(drs_telemetry_t),
                   PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (m == MAP_FAILED) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    g_tel = (drs_telemetry_t *)m;
    memset(g_tel, 0, sizeof(*g_tel));
    g_tel->pid     = (uint32_t)getpid();
    g_tel->version = DRS_TELEMETRY_VERSION;
    atomic_store_explicit(&g_tel->seqlock, 0, memory_order_release);
    return 0;
}

void telemetry_close(void)
{
    if (g_tel) {
        munmap(g_tel, sizeof(drs_telemetry_t));
        g_tel = NULL;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

drs_telemetry_t *telemetry_get(void)
{
    return g_tel;
}

void telemetry_begin_write(void)
{
    if (!g_tel)
        return;
    uint32_t s = atomic_load_explicit(&g_tel->seqlock, memory_order_relaxed);
    atomic_store_explicit(&g_tel->seqlock, s + 1, memory_order_release);
}

void telemetry_end_write(void)
{
    if (!g_tel)
        return;
    uint32_t s = atomic_load_explicit(&g_tel->seqlock, memory_order_relaxed);
    atomic_store_explicit(&g_tel->seqlock, s + 1, memory_order_release);
}
