#include "cmd.h"
#include "net_io.h"   /* for mono_raw_ns() */

#include <stdatomic.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static drs_cmd_channel_t *g_cmd    = NULL;
static int                g_fd     = -1;
static uint32_t           g_last_seq = 0;

int cmd_init(void)
{
    g_fd = open(DRS_SHM_CMD_PATH,
                O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (g_fd < 0)
        return -1;

    if (ftruncate(g_fd, (off_t)sizeof(drs_cmd_channel_t)) < 0) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    void *m = mmap(NULL, sizeof(drs_cmd_channel_t),
                   PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (m == MAP_FAILED) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    g_cmd = (drs_cmd_channel_t *)m;
    g_last_seq = g_cmd->seq;
    return 0;
}

void cmd_close(void)
{
    if (g_cmd) {
        munmap(g_cmd, sizeof(drs_cmd_channel_t));
        g_cmd = NULL;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

uint32_t cmd_poll(void)
{
    if (!g_cmd)
        return DRS_CMD_NONE;

    uint32_t cur_seq = atomic_load_explicit((_Atomic uint32_t *)&g_cmd->seq,
                                            memory_order_acquire);
    if (cur_seq == g_last_seq)
        return DRS_CMD_NONE;

    g_last_seq = cur_seq;
    atomic_thread_fence(memory_order_acquire);
    return g_cmd->code;
}

int cmd_send(uint32_t code)
{
    if (!g_cmd)
        return -1;

    g_cmd->code     = code;
    g_cmd->unix_ns  = (uint64_t)mono_raw_ns();
    atomic_thread_fence(memory_order_release);
    uint32_t s = atomic_load_explicit((_Atomic uint32_t *)&g_cmd->seq,
                                      memory_order_relaxed);
    atomic_store_explicit((_Atomic uint32_t *)&g_cmd->seq, s + 1,
                          memory_order_release);
    return 0;
}
