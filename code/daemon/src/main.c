#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sched.h>
#include <pthread.h>
#include <netinet/in.h>
#include <stdint.h>

#include "statemachine.h"
#include "net_io.h"
#include "gpio_bcm2711.h"
#include "pulse_engine.h"
#include "telemetry.h"
#include "telemetry_udp.h"
#include "errors.h"
#include "cmd.h"
#include "../include/drs_sync_config.h"

/* ── Signal handling ─────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;

static void handle_sig(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── RT setup ────────────────────────────────────────────────────────── */

static void rt_apply(void)
{
    /* CPU affinity: Core 3 only */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(DRS_RT_CPU, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
        errors_push(DRS_ERR_RT_PRIO_FAIL, "sched_setaffinity failed");

    /* SCHED_FIFO priority 85 */
    struct sched_param sp = { .sched_priority = DRS_RT_PRIO };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        errors_push(DRS_ERR_RT_PRIO_FAIL, "sched_setscheduler failed");

    /* Lock all pages (current + future) */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        errors_push(DRS_ERR_RT_PRIO_FAIL, "mlockall failed");
}

/* ── timerfd helpers ─────────────────────────────────────────────────── */

static int make_timerfd_periodic(int64_t period_ns)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    struct itimerspec its = {
        .it_interval = {
            .tv_sec  = period_ns / INT64_C(1000000000),
            .tv_nsec = period_ns % INT64_C(1000000000),
        },
        .it_value = {
            .tv_sec  = period_ns / INT64_C(1000000000),
            .tv_nsec = period_ns % INT64_C(1000000000),
        },
    };
    timerfd_settime(fd, 0, &its, NULL);
    return fd;
}

static void drain_timerfd(int fd)
{
    uint64_t exp;
    if (read(fd, &exp, sizeof(exp)) < 0) { /* ignore */ }
}

/* ── epoll helpers ───────────────────────────────────────────────────── */

#define MAX_EVENTS 16

static int epoll_add(int epfd, int fd, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *ifname       = (argc > 1) ? argv[1] : "eth0";
    int         lock_mode    = DRS_LOCK_AUTO;
    const char *telem_dest   = NULL;
    if (argc > 2) {
        if (strcmp(argv[2], "leader")   == 0) lock_mode = DRS_LOCK_AS_LEADER;
        if (strcmp(argv[2], "follower") == 0) lock_mode = DRS_LOCK_AS_FOLLOWER;
    }
    if (argc > 3) {
        telem_dest = argv[3];
    }

    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);

    /* Shared memory — initialise before RT (may print to stderr on failure) */
    if (telemetry_init() != 0)
        errors_push(DRS_ERR_SHM_INIT_FAIL, "telemetry shm init failed");
    if (errors_init() != 0)
        fprintf(stderr, "drs_syncd: error shm init failed\n");
    if (cmd_init() != 0)
        fprintf(stderr, "drs_syncd: cmd shm init failed\n");

    /* GPIO (non-fatal on unsupported platforms such as WSL) */
    if (gpio_init() != 0) {
        errors_push(DRS_ERR_GPIO_OPEN_FAIL, "/dev/gpiomem open failed — continuing without GPIO");
        fprintf(stderr, "[drs_syncd] WARNING: /dev/gpiomem not available — GPIO pulse/health signals disabled\n");
    }

    /* Network */
    net_io_t net;
    if (net_io_init(&net, ifname) != 0) {
        errors_push(DRS_ERR_NET_INIT_FAIL, "net_io_init failed");
        gpio_close();
        exit(70);
    }

    /* Pulse Engine */
    pulse_engine_t pe;
    if (pulse_engine_init(&pe) != 0) {
        fprintf(stderr, "drs_syncd: pulse_engine_init failed\n");
        net_io_close(&net);
        gpio_close();
        exit(1);
    }

    /* State Machine */
    sm_t sm;
    sm_init(&sm, &net, &pe, net.node_id, lock_mode);

    /* Connect vclock and start dedicated GPIO pulse thread */
    if (pulse_engine_start(&pe, &sm.vc) != 0) {
        fprintf(stderr, "drs_syncd: pulse_engine_start failed\n");
        net_io_close(&net);
        gpio_close();
        exit(1);
    }

    /* Initialise UDP telemetry sender (non-RT thread) */
    if (telem_udp_init(telem_dest, net.node_id) != 0)
        fprintf(stderr, "[drs_syncd] telem_udp_init failed (non-fatal)\n");

    fprintf(stderr, "[drs_syncd] node_id=%-3u  iface=%s  mcast=%s:%u  running\n",
            net.node_id, ifname, DRS_MCAST_GROUP, DRS_PORT);
    fflush(stderr);

    /* Apply RT constraints AFTER socket setup (bind needs normal scheduler) */
    rt_apply();

    /* epoll */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        errors_push(DRS_ERR_RX_TIMEOUT, "epoll_create1 failed");
        exit(1);
    }

    /* 50 ms state-machine tick + telemetry + command poll */
    int tick_fd = make_timerfd_periodic(DRS_SYNC_EXCHANGE_NS);
    /* 100 ms leader ANNOUNCE heartbeat */
    int hb_fd   = make_timerfd_periodic(DRS_LEADER_ANNOUNCE_NS);

    /* Link monitor counter — check carrier every 500 ms (10 ticks) */
    int link_check_ticks = 0;

    epoll_add(epfd, net.sock_mcast,   EPOLLIN);
    epoll_add(epfd, net.sock_ucast,   EPOLLIN);
    epoll_add(epfd, tick_fd,          EPOLLIN);
    epoll_add(epfd, hb_fd,            EPOLLIN);
    /* pe.timer_fd and pe.pulse_low_fd are owned by the pulse thread — not in epoll */

    struct epoll_event events[MAX_EVENTS];

    while (g_running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 200);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            errors_push(DRS_ERR_RX_TIMEOUT, "epoll_wait error");
            break;
        }

        int64_t t_now = mono_raw_ns();

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == tick_fd) {
                drain_timerfd(tick_fd);
                sm_tick(&sm, t_now);

                /* Every 10 ticks (500 ms) check link carrier and recover
                 * multicast membership after a cable flap (F-48). */
                if (++link_check_ticks >= 10) {
                    link_check_ticks = 0;
                    net_check_recover(sm.net, ifname);
                }

                /* Poll command channel */
                uint32_t cmd = cmd_poll();
                if (cmd != DRS_CMD_NONE)
                    sm_on_command(&sm, cmd, t_now);

            } else if (fd == hb_fd) {
                drain_timerfd(hb_fd);
                if (sm.state == DRS_STATE_LEADER) {
                    drs_packet_t ann = {
                        .msg_type      = DRS_MSG_ANNOUNCE,
                        .flags         = DRS_FLAG_LEADER | DRS_FLAG_CALIBRATED,
                        .seq           = sm.last_leader_seq++,
                        .node_id       = sm.el.own_node_id,
                        .election_term = sm.el.own_term,
                    };
                    net_send_announce(sm.net, &ann, NULL);
                }

            } else if (fd == net.sock_mcast) {
                uint8_t buf[DRS_PAYLOAD_BYTES];
                struct sockaddr_in src = {0};
                int64_t t4 = 0;
                int r = net_recv(fd, buf, &src, &t4);
                if (r == DRS_PAYLOAD_BYTES) {
                    drs_packet_t pkt;
                    if (proto_decode(&pkt, buf) == 0) {
                        if (pkt.msg_type == DRS_MSG_ANNOUNCE)
                            sm_on_announce(&sm, &pkt, &src, t_now);
                        else if (pkt.msg_type == DRS_MSG_SYNC_REQ)
                            sm_on_sync_req(&sm, &pkt, &src, t4);
                        else if (pkt.msg_type == DRS_MSG_SYNC_RESP)
                            /* Accept SYNC_RESP on mcast socket too: some foreign
                             * implementations reply to DRS_PORT instead of the
                             * ephemeral source port of the SYNC_REQ. */
                            sm_on_sync_resp(&sm, &pkt, t4);
                    } else {
                        errors_push(DRS_ERR_CRC_MISMATCH, "mcast CRC fail");
                    }
                }

            } else if (fd == net.sock_ucast) {
                uint8_t buf[DRS_PAYLOAD_BYTES];
                struct sockaddr_in src = {0};
                int64_t t4 = 0;
                int r = net_recv(fd, buf, &src, &t4);
                if (r == DRS_PAYLOAD_BYTES) {
                    drs_packet_t pkt;
                    if (proto_decode(&pkt, buf) == 0) {
                        if (pkt.msg_type == DRS_MSG_SYNC_REQ)
                            sm_on_sync_req(&sm, &pkt, &src, t4);
                        else if (pkt.msg_type == DRS_MSG_SYNC_RESP)
                            sm_on_sync_resp(&sm, &pkt, t4);
                    } else {
                        errors_push(DRS_ERR_CRC_MISMATCH, "ucast CRC fail");
                    }
                }

            }
        }
    }

    /* Shutdown */
    close(epfd);
    close(tick_fd);
    close(hb_fd);
    pulse_engine_close(&pe);
    net_io_close(&net);
    gpio_close();
    telem_udp_close();
    telemetry_close();
    errors_close();
    cmd_close();

    return 0;
}
