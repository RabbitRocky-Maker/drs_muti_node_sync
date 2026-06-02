#ifndef STATEMACHINE_H
#define STATEMACHINE_H

#include <stdint.h>
#include <netinet/in.h>
#include "vclock.h"
#include "pi_ctrl.h"
#include "min_delay.h"
#include "election.h"
#include "net_io.h"
#include "pulse_engine.h"
#include "telemetry.h"

typedef enum {
    DRS_STATE_GROUND       = 0,
    DRS_STATE_CALIBRATION  = 1,
    DRS_STATE_LISTEN       = 2,
    DRS_STATE_CANDIDATE    = 3,
    DRS_STATE_FOLLOWER     = 4,
    DRS_STATE_LEADER       = 5,
    DRS_STATE_HOLDOVER     = 6,
} drs_state_t;

typedef struct {
    drs_state_t  state;

    /* Sub-components */
    vclock_t     vc;
    pi_ctrl_t    pi;
    min_delay_t  mdf;
    election_t   el;

    /* Network I/O (pointer — owned by main) */
    net_io_t    *net;

    /* Pulse engine (pointer — owned by main) */
    pulse_engine_t *pe;

    /* Timing */
    int64_t  state_entry_ns;       /* CLOCK_MONOTONIC_RAW at state entry */
    int64_t  last_pi_update_ns;    /* last PI controller invocation */
    int64_t  last_leader_ns;       /* last ANNOUNCE received (FOLLOWER) */
    int64_t  holdover_entry_ns;
    int64_t  sync_req_sent_ns;     /* when last SYNC_REQ was sent */

    /* Sequence tracking */
    uint16_t last_leader_seq;

    /* Rate tracking for telemetry */
    uint64_t last_rate_q32;

    /* Retry storm */
    int      sync_fail_count;
    int64_t  sync_rate_ns;          /* current SYNC_REQ interval */

    /* Pending SYNC_REQ */
    int      sync_req_pending;      /* 1 if awaiting SYNC_RESP */
    drs_packet_t last_sync_req;     /* copy sent, for T1 */
    int64_t  sync_req_t1;

} sm_t;

void sm_init(sm_t *sm, net_io_t *net, pulse_engine_t *pe,
             uint32_t own_node_id, int lock_mode);

/* Returns 1 if state >= LISTEN (CALIBRATION complete) */
int sm_is_active(const sm_t *sm);

/*
 * Periodic 50 ms tick: run state machine, send heartbeats, update telemetry.
 * t_now = current CLOCK_MONOTONIC_RAW.
 */
void sm_tick(sm_t *sm, int64_t t_now);

/* Handle incoming ANNOUNCE (from sock_mcast) */
void sm_on_announce(sm_t *sm, const drs_packet_t *pkt,
                    const struct sockaddr_in *src, int64_t t_now);

/* Handle incoming SYNC_REQ (from sock_ucast, only when LEADER) */
void sm_on_sync_req(sm_t *sm, const drs_packet_t *pkt,
                    const struct sockaddr_in *src, int64_t t2);

/* Handle incoming SYNC_RESP (from sock_ucast, only when FOLLOWER) */
void sm_on_sync_resp(sm_t *sm, const drs_packet_t *pkt, int64_t t4);

/* Process a command from the command channel */
void sm_on_command(sm_t *sm, uint32_t cmd, int64_t t_now);

#endif /* STATEMACHINE_H */
