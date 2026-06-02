#include "telemetry_udp.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

/* ── Ring buffer (power-of-2, single-producer single-consumer) ──────── */

#define TELEM_RING_SIZE  64
#define TELEM_RING_MASK  (TELEM_RING_SIZE - 1)

static telem_udp_record_t g_ring[TELEM_RING_SIZE];
static _Atomic uint64_t   g_write_idx;  /* producer (RT thread)  */
static _Atomic uint64_t   g_read_idx;   /* consumer (send thread) */

/* ── Socket and sender thread ───────────────────────────────────────── */

static int               g_sock      = -1;
static struct sockaddr_in g_dst       = {0};
static uint32_t           g_node_id   = 0;
static pthread_t          g_thread    = {0};
static volatile int       g_running   = 0;

/* ── Sender thread ──────────────────────────────────────────────────── */

static void *sender_thread(void *arg)
{
    (void)arg;

    /* Poll at ~10 ms intervals */
    const struct timespec poll_interval = {
        .tv_sec  = 0,
        .tv_nsec = 10000000,  /* 10 ms */
    };

    while (g_running) {
        /* Drain ring buffer */
        uint64_t r = atomic_load_explicit(&g_read_idx, memory_order_relaxed);
        uint64_t w = atomic_load_explicit(&g_write_idx, memory_order_acquire);

        while (r < w) {
            telem_udp_record_t rec = g_ring[r & TELEM_RING_MASK];

            /* Fix up node_id in case it was set to 0 at init */
            rec.node_id = g_node_id;

            ssize_t n = sendto(g_sock, &rec, sizeof(rec), 0,
                               (struct sockaddr *)&g_dst, sizeof(g_dst));

            if (n < 0 && errno == ENOBUFS) {
                /* Socket buffer full – wait and retry next poll cycle */
                break;
            }

            r++;
            atomic_store_explicit(&g_read_idx, r, memory_order_release);
        }

        /* Sleep ~10 ms */
        struct timespec rem;
        nanosleep(&poll_interval, &rem);
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int telem_udp_init(const char *dest_ip, uint32_t node_id)
{
    g_node_id = node_id;

    /* Destination address */
    const char *ip = dest_ip ? dest_ip : "127.0.0.1";
    g_dst.sin_family      = AF_INET;
    g_dst.sin_port        = htons(4242);
    if (inet_pton(AF_INET, ip, &g_dst.sin_addr) != 1) {
        fprintf(stderr, "[telem_udp] invalid dest IP: %s\n", ip);
        return -1;
    }

    /* Create UDP socket */
    g_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (g_sock < 0) {
        fprintf(stderr, "[telem_udp] socket creation failed\n");
        return -1;
    }

    /* Increase socket send buffer to reduce drops under load */
    int bufsize = 212992;  /* 208 kB */
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    /* Initialise ring buffer */
    atomic_store_explicit(&g_write_idx, 0, memory_order_release);
    atomic_store_explicit(&g_read_idx,  0, memory_order_release);

    /* Start sender thread (non-RT, will be pinned to non-isolated core) */
    g_running = 1;
    if (pthread_create(&g_thread, NULL, sender_thread, NULL) != 0) {
        fprintf(stderr, "[telem_udp] pthread_create failed\n");
        close(g_sock);
        g_sock = -1;
        g_running = 0;
        return -1;
    }

    fprintf(stderr, "[telem_udp] sending to %s:4242  node_id=%u\n", ip, g_node_id);
    return 0;
}

void telem_udp_close(void)
{
    g_running = 0;
    if (g_thread) {
        pthread_join(g_thread, NULL);
        g_thread = 0;
    }
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}

void telem_udp_emit(int64_t  timestamp_ns,
                    int32_t  state,
                    int64_t  offset_ns,
                    int64_t  rtt_ns,
                    int64_t  rate_q32)
{
    uint64_t w = atomic_load_explicit(&g_write_idx, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&g_read_idx,  memory_order_acquire);

    /* Ring full — drop silently */
    if (w - r >= TELEM_RING_SIZE)
        return;

    telem_udp_record_t *slot = &g_ring[w & TELEM_RING_MASK];
    slot->timestamp_ns = timestamp_ns;
    slot->state        = state;
    slot->offset_ns    = offset_ns;
    slot->rtt_ns       = rtt_ns;
    slot->rate_q32     = rate_q32;
    slot->node_id      = g_node_id;

    atomic_store_explicit(&g_write_idx, w + 1, memory_order_release);
}
