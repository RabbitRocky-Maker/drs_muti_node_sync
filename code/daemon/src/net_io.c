#include "net_io.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <time.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

int64_t mono_raw_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static int __attribute__((unused)) make_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Find the routable (non-loopback) IPv4 address on the given interface.
 * Returns the address in network byte order, or 0 on failure.
 */
static uint32_t get_iface_addr(const char *ifname)
{
    struct ifaddrs *ifa_list, *ifa;
    if (getifaddrs(&ifa_list) != 0)
        return 0;

    uint32_t addr = 0;
    for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(ifa->ifa_name, ifname) != 0)
            continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        /* Skip loopback (127.x.x.x) */
        if ((a >> 24) == 127)
            continue;
        addr = sin->sin_addr.s_addr; /* network byte order */
        break;
    }
    freeifaddrs(ifa_list);
    return addr;
}

/* ── Multicast socket ─────────────────────────────────────────────────── */

static int open_mcast_socket(const char *ifname, uint32_t iface_addr_ne)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    /* Bind to any address on the DRS port */
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DRS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Join multicast group on the target interface */
    struct ip_mreqn mreq = {0};
    inet_pton(AF_INET, DRS_MCAST_GROUP, &mreq.imr_multiaddr);
    mreq.imr_address.s_addr = iface_addr_ne;
    mreq.imr_ifindex         = (int)if_nametoindex(ifname);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    /* Disable multicast loopback (we don't want our own ANNOUNCEs) */
    unsigned char loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    /* Low-delay TOS */
    int tos = IPTOS_LOWDELAY;
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    return fd;
}

/* ── Unicast socket ───────────────────────────────────────────────────── */

static int open_ucast_socket(uint32_t iface_addr_ne)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* Bind to interface address, OS-assigned port */
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = 0,
        .sin_addr.s_addr = iface_addr_ne,
    };
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(fd);
        return -1;
    }

    int tos = IPTOS_LOWDELAY;
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    /* Force multicast (ANNOUNCE) out the sync interface — the routing
     * table on multi-interface hosts (e.g. eth0 + wlan0) may otherwise
     * pick the wrong interface and ANNOUNCE never reaches peers. */
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr_ne,
               sizeof(iface_addr_ne));

    return fd;
}

/* ── Link / multicast recovery ──────────────────────────────────────── */

/*
 * Check interface carrier state.  If the link just came back up after
 * being down, drop and re-join the multicast group.  This re-programs
 * the NIC hardware multicast filter, which some drivers lose on carrier
 * loss/gain cycles.
 *
 * Returns  1 if multicast was refreshed,
 *          0 if no action was needed,
 *         -1 on error.
 */
int net_check_recover(net_io_t *net, const char *ifname)
{
    static int carrier_was_up = 1;  /* assume up before first check */

    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    char buf[2] = {0};
    int carrier_now = 0;
    if (read(fd, buf, 1) > 0 && buf[0] == '1')
        carrier_now = 1;
    close(fd);

    int just_came_up = (!carrier_was_up && carrier_now);
    carrier_was_up = carrier_now;

    if (!just_came_up)
        return 0;

    /* Refresh multicast membership */
    uint32_t iface_addr = get_iface_addr(ifname);
    if (!iface_addr)
        return -1;

    struct ip_mreqn mreq = {0};
    inet_pton(AF_INET, DRS_MCAST_GROUP, &mreq.imr_multiaddr);
    mreq.imr_address.s_addr = iface_addr;
    mreq.imr_ifindex         = (int)if_nametoindex(ifname);

    setsockopt(net->sock_mcast, IPPROTO_IP, IP_DROP_MEMBERSHIP,
               &mreq, sizeof(mreq));
    if (setsockopt(net->sock_mcast, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0)
        return -1;

    fprintf(stderr, "[drs] link-up on %s, multicast membership refreshed\n", ifname);
    fflush(stderr);
    return 1;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int net_io_init(net_io_t *net, const char *ifname)
{
    memset(net, 0, sizeof(*net));
    net->sock_mcast = -1;
    net->sock_ucast = -1;

    uint32_t iface_addr_ne = get_iface_addr(ifname);
    if (!iface_addr_ne) {
        /* Fallback: 10.0.0.31 (Team 3, Node 1) — will be overridden by DHCP */
        inet_pton(AF_INET, "10.0.0.31", &iface_addr_ne);
    }

    /* node_id = last octet of routable IPv4 */
    net->node_id = ntohl(iface_addr_ne) & 0xFF;
    if (net->node_id == 0)
        net->node_id = 1; /* guard against .0 subnet addresses */

    net->sock_mcast = open_mcast_socket(ifname, iface_addr_ne);
    if (net->sock_mcast < 0)
        return -1;

    net->sock_ucast = open_ucast_socket(iface_addr_ne);
    if (net->sock_ucast < 0) {
        close(net->sock_mcast);
        net->sock_mcast = -1;
        return -1;
    }

    return 0;
}

void net_io_close(net_io_t *net)
{
    if (net->sock_mcast >= 0) { close(net->sock_mcast); net->sock_mcast = -1; }
    if (net->sock_ucast >= 0) { close(net->sock_ucast); net->sock_ucast = -1; }
}

void net_set_leader(net_io_t *net, uint32_t leader_ip_be)
{
    net->leader_addr.sin_family      = AF_INET;
    net->leader_addr.sin_port        = htons(DRS_PORT);
    net->leader_addr.sin_addr.s_addr = leader_ip_be;
}

static int send_packet(int sock, const drs_packet_t *pkt,
                       const struct sockaddr_in *dst, int64_t *ts_out)
{
    uint8_t buf[DRS_PAYLOAD_BYTES];
    if (proto_encode(buf, pkt) != 0)
        return -1;

    /* Timestamp immediately before sendto */
    if (ts_out)
        *ts_out = mono_raw_ns();

    ssize_t n = sendto(sock, buf, DRS_PAYLOAD_BYTES, 0,
                       (const struct sockaddr *)dst, sizeof(*dst));
    return (n == DRS_PAYLOAD_BYTES) ? 0 : -1;
}

int net_send_announce(net_io_t *net, const drs_packet_t *pkt, int64_t *t1_out)
{
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(DRS_PORT),
    };
    inet_pton(AF_INET, DRS_MCAST_GROUP, &dst.sin_addr);
    /* Send via sock_mcast, not sock_ucast — sock_mcast already has
     * IP_MULTICAST_IF configured for the sync interface (and has joined
     * the group for reception).  Sending ANNOUNCE via sock_ucast risks
     * the kernel routing the packet out the wrong interface on
     * multi-homed hosts. */
    return send_packet(net->sock_mcast, pkt, &dst, t1_out);
}

int net_send_sync_req(net_io_t *net, drs_packet_t *pkt, int64_t *t1_out)
{
    return send_packet(net->sock_ucast, pkt, &net->leader_addr, t1_out);
}

int net_send_sync_resp(net_io_t *net, drs_packet_t *pkt,
                       const struct sockaddr_in *dst, int64_t *t3_out)
{
    return send_packet(net->sock_ucast, pkt, dst, t3_out);
}

int net_recv(int sock, uint8_t buf[DRS_PAYLOAD_BYTES],
             struct sockaddr_in *src_addr, int64_t *t4_out)
{
    socklen_t addrlen = sizeof(*src_addr);
    ssize_t n = recvfrom(sock, buf, DRS_PAYLOAD_BYTES, 0,
                         (struct sockaddr *)src_addr, &addrlen);
    /* Timestamp immediately after recvfrom */
    if (t4_out)
        *t4_out = mono_raw_ns();

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (n != DRS_PAYLOAD_BYTES)
        return 0; /* wrong size — discard */
    return (int)n;
}
