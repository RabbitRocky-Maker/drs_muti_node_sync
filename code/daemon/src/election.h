#ifndef ELECTION_H
#define ELECTION_H

#include <stdint.h>
#include "proto_v2.h"
#include "../include/drs_sync_config.h"

typedef enum {
    ELECT_NO_CHANGE = 0,
    ELECT_BECOME_FOLLOWER,
    ELECT_PROMOTE_LEADER,
} elect_result_t;

typedef struct {
    uint32_t own_term;
    uint32_t own_node_id;
    uint32_t leader_node_id;
    int64_t  election_timeout_ns;  /* randomised 250–500 ms */
    int64_t  timeout_deadline_ns;  /* absolute CLOCK_MONOTONIC_RAW */
    int      lock_mode;            /* DRS_LOCK_* */
} election_t;

void election_init(election_t *el, uint32_t own_node_id, int lock_mode);

/* Reset and arm a new randomised election timeout */
void election_arm_timeout(election_t *el);

/*
 * Process an incoming ANNOUNCE.
 * Returns what the state machine should do next.
 * On ELECT_BECOME_FOLLOWER, el->leader_node_id is updated.
 */
elect_result_t election_on_announce(election_t *el, const drs_packet_t *pkt);

/*
 * Called by the state machine when election timeout fires without a higher-
 * termed leader being seen.  Promotes this node to leader.
 */
void election_promote(election_t *el);

/* True if the election timer has expired */
int election_timed_out(const election_t *el);

#endif /* ELECTION_H */
