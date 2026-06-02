#ifndef NET_IO_H
#define NET_IO_H

#include <stdint.h>
#include <netinet/in.h>
#include "proto_v2.h"
#include "../include/drs_sync_config.h"

typedef struct {
    int      sock_mcast;    /* multicast recv socket (bound to DRS_PORT) */
    int      sock_ucast;    /* unicast send/recv socket */
    uint32_t node_id;       /* derived from last octet of routable IPv4 */
    struct sockaddr_in leader_addr; /* current leader unicast address */
} net_io_t;

/*
 * Initialise sockets.  ifname = "eth0" or "wlan0".
 * Returns 0 on success, -1 on error.
 */
int net_io_init(net_io_t *net, const char *ifname);

/* Close all sockets */
void net_io_close(net_io_t *net);

/*
 * Send an ANNOUNCE to the multicast group.
 * t1_out receives the CLOCK_MONOTONIC_RAW timestamp taken just before sendto.
 */
int net_send_announce(net_io_t *net, const drs_packet_t *pkt, int64_t *t1_out);

/*
 * Send a SYNC_REQ to the current leader (unicast).
 * t1_out receives the timestamp just before sendto (= T1).
 */
int net_send_sync_req(net_io_t *net, drs_packet_t *pkt, int64_t *t1_out);

/*
 * Send a SYNC_RESP back to a follower (unicast).
 * src_addr = follower address from recvfrom.
 * t3_out receives the timestamp just before sendto (= T3).
 */
int net_send_sync_resp(net_io_t *net, drs_packet_t *pkt,
                       const struct sockaddr_in *dst, int64_t *t3_out);

/*
 * Receive one packet from sock_mcast or sock_ucast (non-blocking).
 * Returns number of bytes received, 0 if would block, -1 on error.
 * src_addr is filled with sender address.
 * t4_out receives the timestamp just after recvfrom (= T4 for follower).
 */
int net_recv(int sock, uint8_t buf[DRS_PAYLOAD_BYTES],
             struct sockaddr_in *src_addr, int64_t *t4_out);

/* Set the current leader address for unicast SYNC_REQ */
void net_set_leader(net_io_t *net, uint32_t leader_ip_be);

/*
 * Check carrier state and re-join multicast group if the link just
 * came back up.  Call periodically (~every 500 ms).
 * Returns 1 if recovered, 0 if no action, -1 on error.
 */
int net_check_recover(net_io_t *net, const char *ifname);

/* Helper: read CLOCK_MONOTONIC_RAW in nanoseconds */
int64_t mono_raw_ns(void);

#endif /* NET_IO_H */
