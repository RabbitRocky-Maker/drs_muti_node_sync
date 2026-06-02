#include "statemachine.h"
#include "calibrate.h"
#include "errors.h"
#include "cmd.h"
#include "telemetry_udp.h"
#include "../include/drs_sync_config.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>

static const char *state_name(drs_state_t s)
{
    switch (s) {
    case DRS_STATE_GROUND:       return "GROUND";
    case DRS_STATE_CALIBRATION:  return "CALIBRATION";
    case DRS_STATE_LISTEN:       return "LISTEN";
    case DRS_STATE_CANDIDATE:    return "CANDIDATE";
    case DRS_STATE_FOLLOWER:     return "FOLLOWER";
    case DRS_STATE_LEADER:       return "LEADER";
    case DRS_STATE_HOLDOVER:     return "HOLDOVER";
    default:                     return "?";
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void enter_state(sm_t *sm, drs_state_t next, int64_t t_now)
{
    fprintf(stderr, "[drs] %s -> %s\n", state_name(sm->state), state_name(next));
    fflush(stderr);
    sm->state          = next;
    sm->state_entry_ns = t_now;
}

static void enter_listen(sm_t *sm, int64_t t_now)
{
    enter_state(sm, DRS_STATE_LISTEN, t_now);
    election_arm_timeout(&sm->el);
}

static void enter_candidate(sm_t *sm, int64_t t_now)
{
    enter_state(sm, DRS_STATE_CANDIDATE, t_now);
    election_arm_timeout(&sm->el);
}

static void discipline_reset(sm_t *sm)
{
    min_delay_reset(&sm->mdf);
    pi_ctrl_init(&sm->pi);
    sm->pi.step_accum_ns   = 0;
    sm->sync_fail_count    = 0;
    sm->sync_rate_ns       = DRS_SYNC_EXCHANGE_NS;
    sm->sync_req_pending   = 0;
}

static void enter_follower(sm_t *sm, const drs_packet_t *pkt,
                           const struct sockaddr_in *src, int64_t t_now)
{
    if (!sm_is_active(sm))
        return; /* Active-Before-Follower Guard (F-23) */

    /* Check if leader actually changed */
    int leader_changed = (sm->el.leader_node_id != pkt->node_id);

    sm->el.leader_node_id = pkt->node_id;

    /* Store leader's unicast address for SYNC_REQ */
    if (src) {
        net_set_leader(sm->net, src->sin_addr.s_addr);
        char ip_str[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &src->sin_addr, ip_str, sizeof(ip_str));
        fprintf(stderr, "[drs] leader node_id=%-3u  addr=%s:%u\n",
                pkt->node_id, ip_str, DRS_PORT);
        fflush(stderr);
    }

    if (leader_changed)
        discipline_reset(sm);   /* F-14: filter + PI reset on leader change */

    enter_state(sm, DRS_STATE_FOLLOWER, t_now);
    sm->last_leader_ns = t_now;
}

static void enter_leader(sm_t *sm, int64_t t_now)
{
    election_promote(&sm->el);
    enter_state(sm, DRS_STATE_LEADER, t_now);
    /* Calibrate on promotion (F-32) */
    if (calibrate_loopback(&sm->vc) != 0)
        errors_push(DRS_ERR_CALIB_TIMEOUT, "calib on leader promote failed");
}

static void enter_holdover(sm_t *sm, int64_t t_now)
{
    enter_state(sm, DRS_STATE_HOLDOVER, t_now);
    sm->holdover_entry_ns = t_now;
    /* Rate is left frozen; offset is frozen (no more PI updates) */
    /* GPIO 23 is driven HIGH by pulse_engine_update_health */

    drs_telemetry_t *tel = telemetry_get();
    if (tel) {
        telemetry_begin_write();
        tel->holdover_count++;
        telemetry_end_write();
    }
}

static void update_telemetry(sm_t *sm, int64_t t_now)
{
    drs_telemetry_t *tel = telemetry_get();
    if (!tel)
        return;

    int in_holdover = (sm->state == DRS_STATE_HOLDOVER);
    int64_t offset = tel->offset_ns; /* last theta_residual from sm_on_sync_resp */

    telemetry_begin_write();
    tel->state            = (uint32_t)sm->state;
    tel->leader_node_id   = sm->el.leader_node_id;
    tel->election_term    = sm->el.own_term;
    tel->rate_q32_32      = (int64_t)sm->last_rate_q32;
    tel->latency_corr_ns  = sm->vc.lat_corr_ns;
    tel->lock_mode        = (uint32_t)sm->el.lock_mode;
    tel->pulse_count      = sm->pe->pulse_count;

    if (in_holdover) {
        int64_t elapsed = t_now - sm->holdover_entry_ns;
        int64_t remain  = DRS_HOLDOVER_MAX_NS - elapsed;
        tel->holdover_remaining_ms =
            (uint32_t)((remain > 0 ? remain : 0) / INT64_C(1000000));
        tel->flags = DRS_FLAG_HOLDOVER;
    } else {
        tel->holdover_remaining_ms = 0;
        uint32_t flags = 0;
        if (sm->state == DRS_STATE_LEADER)     flags |= DRS_FLAG_LEADER;
        if (sm->state >= DRS_STATE_LISTEN)     flags |= DRS_FLAG_CALIBRATED;
        tel->flags = flags;
    }
    telemetry_end_write();

    pulse_engine_update_health(sm->pe, offset, in_holdover);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void sm_init(sm_t *sm, net_io_t *net, pulse_engine_t *pe,
             uint32_t own_node_id, int lock_mode)
{
    memset(sm, 0, sizeof(*sm));
    sm->net           = net;
    sm->pe            = pe;
    sm->sync_rate_ns  = DRS_SYNC_EXCHANGE_NS;

    vclock_init(&sm->vc, mono_raw_ns());
    pi_ctrl_init(&sm->pi);
    min_delay_init(&sm->mdf);
    election_init(&sm->el, own_node_id, lock_mode);
    sm->last_rate_q32 = DRS_RATE_NOMINAL_Q32;

    enter_state(sm, DRS_STATE_GROUND, mono_raw_ns());
}

int sm_is_active(const sm_t *sm)
{
    return sm->state != DRS_STATE_GROUND &&
           sm->state != DRS_STATE_CALIBRATION;
}

void sm_tick(sm_t *sm, int64_t t_now)
{
    switch (sm->state) {

    case DRS_STATE_GROUND:
        if (t_now - sm->state_entry_ns >= DRS_GROUND_DURATION_NS) {
            enter_state(sm, DRS_STATE_CALIBRATION, t_now);
        }
        break;

    case DRS_STATE_CALIBRATION: {
        int64_t t_local = mono_raw_ns();
        vclock_init(&sm->vc, t_local);
        int ret = calibrate_loopback(&sm->vc);
        if (ret != 0) {
            errors_push(DRS_ERR_CALIB_TIMEOUT, "initial calibration failed");
            /* Retry in next tick — stay in CALIBRATION */
            break;
        }
        pulse_engine_arm(sm->pe, &sm->vc);
        enter_listen(sm, t_now);
        break;
    }

    case DRS_STATE_LISTEN:
        if (election_timed_out(&sm->el))
            enter_candidate(sm, t_now);
        break;

    case DRS_STATE_CANDIDATE:
        if (election_timed_out(&sm->el))
            enter_leader(sm, t_now);
        break;

    case DRS_STATE_FOLLOWER: {
        /* Check leader timeout (3 × ANNOUNCE = 300 ms) */
        if (t_now - sm->last_leader_ns >= DRS_FOLLOWER_TIMEOUT_NS) {
            enter_holdover(sm, t_now);
            break;
        }

        /* Check SYNC_REQ timeout */
        if (sm->sync_req_pending &&
            t_now - sm->sync_req_sent_ns >= DRS_SYNC_REQ_TIMEOUT_NS) {
            sm->sync_req_pending = 0;
            sm->sync_fail_count++;
            if (sm->sync_fail_count >= DRS_RETRY_STORM_THRESHOLD)
                sm->sync_rate_ns = DRS_RETRY_STORM_RATE_NS;
        }

        /* Send periodic SYNC_REQ */
        if (!sm->sync_req_pending &&
            t_now - sm->last_pi_update_ns >= sm->sync_rate_ns) {

            drs_packet_t req = {
                .msg_type     = DRS_MSG_SYNC_REQ,
                .flags        = DRS_FLAG_CALIBRATED,
                .seq          = sm->last_leader_seq,
                .node_id      = sm->el.own_node_id,
                .election_term= sm->el.own_term,
            };
            int64_t t1 = 0;
            if (net_send_sync_req(sm->net, &req, &t1) == 0) {
                sm->last_sync_req    = req;
                sm->sync_req_t1      = t1;
                sm->sync_req_sent_ns = t_now;
                sm->sync_req_pending = 1;
            } else {
                errors_push(DRS_ERR_NET_INIT_FAIL, "SYNC_REQ sendto failed");
            }
        }

        /* Emit UDP telemetry heartbeat every tick so the monitor always
         * shows follower state even when the SYNC exchange is stalled.
         * When SYNC succeeds, sm_on_sync_resp() emits a second record
         * with the real offset/RTT values — that is intentional. */
        {
            drs_telemetry_t *tel = telemetry_get();
            telem_udp_emit(t_now, DRS_STATE_FOLLOWER,
                           tel ? tel->offset_ns   : 0,
                           tel ? tel->last_rtt_ns : 0,
                           (int64_t)sm->last_rate_q32);
        }
        break;
    }

    case DRS_STATE_LEADER: {
        /* Emit leader telemetry: one record per 50 ms tick */
        telem_udp_emit(t_now, DRS_STATE_LEADER,
                       0, 0, DRS_RATE_NOMINAL_Q32);
        break;
    }

    case DRS_STATE_HOLDOVER: {
        int64_t elapsed = t_now - sm->holdover_entry_ns;
        if (elapsed >= DRS_HOLDOVER_MAX_NS) {
            /* Budget exhausted: re-calibrate + re-elect */
            errors_push(DRS_ERR_HOLDOVER_EXPIRED, "holdover expired, re-electing");
            if (calibrate_loopback(&sm->vc) != 0)
                errors_push(DRS_ERR_CALIB_TIMEOUT, "calib after holdover failed");
            discipline_reset(sm);
            enter_candidate(sm, t_now);
        }
        break;
    }
    }

    update_telemetry(sm, t_now);
}

void sm_on_announce(sm_t *sm, const drs_packet_t *pkt,
                    const struct sockaddr_in *src, int64_t t_now)
{
    elect_result_t res = election_on_announce(&sm->el, pkt);

    switch (sm->state) {

    case DRS_STATE_GROUND:
    case DRS_STATE_CALIBRATION:
        /* Active-Before-Follower Guard: ignore during startup */
        return;

    case DRS_STATE_LISTEN:
    case DRS_STATE_CANDIDATE:
        if (res == ELECT_BECOME_FOLLOWER)
            enter_follower(sm, pkt, src, t_now);
        else if (pkt->election_term == sm->el.own_term && (pkt->flags & DRS_FLAG_LEADER)
                 && pkt->node_id < sm->el.own_node_id)
            enter_follower(sm, pkt, src, t_now);
        break;

    case DRS_STATE_FOLLOWER:
        sm->last_leader_ns = t_now;
        if (res == ELECT_BECOME_FOLLOWER) {
            /* New leader (higher term or tiebreak) */
            enter_follower(sm, pkt, src, t_now);
        }
        break;

    case DRS_STATE_LEADER:
        if (res == ELECT_BECOME_FOLLOWER) {
            /* Strictly higher term observed — demote */
            discipline_reset(sm);
            enter_follower(sm, pkt, src, t_now);
        }
        break;

    case DRS_STATE_HOLDOVER:
        if (res == ELECT_BECOME_FOLLOWER || pkt->node_id == sm->el.leader_node_id) {
            /* Leader reappeared within 10 s */
            discipline_reset(sm);
            enter_follower(sm, pkt, src, t_now);
        }
        break;
    }
}

void sm_on_sync_req(sm_t *sm, const drs_packet_t *req,
                    const struct sockaddr_in *src, int64_t t2)
{
    if (sm->state != DRS_STATE_LEADER)
        return;

    /* Use vclock timestamps so follower syncs to our vclock epoch (not physical clock).
     * This ensures GPIO pulse boundaries align after convergence. */
    int64_t t2_vc = vclock_now(&sm->vc, t2);
    int64_t t3_vc = vclock_now(&sm->vc, mono_raw_ns());
    drs_packet_t resp = {
        .msg_type      = DRS_MSG_SYNC_RESP,
        .flags         = DRS_FLAG_LEADER | DRS_FLAG_CALIBRATED,
        .seq           = req->seq,
        .node_id       = sm->el.own_node_id,
        .election_term = sm->el.own_term,
        .t1            = req->t1,
        .t2            = t2_vc,
        .t3            = t3_vc,
        .t4            = 0,
    };
    net_send_sync_resp(sm->net, &resp, src, NULL);
}

void sm_on_sync_resp(sm_t *sm, const drs_packet_t *pkt, int64_t t4)
{
    if (sm->state != DRS_STATE_FOLLOWER)
        return;
    if (!sm->sync_req_pending)
        return;

    sm->sync_req_pending = 0;
    sm->sync_fail_count  = 0;
    sm->sync_rate_ns     = DRS_SYNC_EXCHANGE_NS; /* restore normal rate */

    int64_t t1 = sm->sync_req_t1;
    int64_t t2 = pkt->t2;
    int64_t t3 = pkt->t3;
    /* t4 is the local timestamp taken at recvfrom */

    /* Sequence-discontinuity check */
    uint16_t seq_delta = pkt->seq - sm->last_leader_seq;
    if (seq_delta > DRS_SEQ_DISC_THRESHOLD) {
        errors_push(DRS_ERR_SEQ_DISCONTINUITY, "seq jump");
        min_delay_reset(&sm->mdf);
    }
    sm->last_leader_seq = pkt->seq;

    int64_t rtt   = (t4 - t1) - (t3 - t2);  /* physical RTT for filter */
    drs_telemetry_t *tel = telemetry_get();
    if (!min_delay_update(&sm->mdf, rtt)) {
        /* outlier rejected */
        if (tel) {
            telemetry_begin_write();
            tel->sync_rejected++;
            telemetry_end_write();
        }
        return;
    }
    if (tel) {
        telemetry_begin_write();
        tel->sync_accepted++;
        telemetry_end_write();
    }

    /*
     * Offset computation using vclock timestamps for proper PI feedback.
     * Converting physical t1/t4 to vclock means rate adjustments made by
     * the PI affect future theta measurements, closing the control loop.
     * The RTT calculation above still uses physical timestamps for accuracy.
     */
    int64_t t1_vc = vclock_now(&sm->vc, t1);
    int64_t t4_vc = vclock_now(&sm->vc, t4);
    int64_t theta = ((t2 - t1_vc) + (t3 - t4_vc)) / 2;
    int64_t theta_residual = theta;

    /* Update telemetry fields (F-36) */
    if (tel) {
        telemetry_begin_write();
        tel->offset_ns    = theta_residual;
        tel->last_rtt_ns  = rtt;
        telemetry_end_write();
    }

    /* PI update */
    int64_t t_now    = mono_raw_ns();
    int64_t period   = t_now - sm->last_pi_update_ns;
    if (period <= 0)
        period = DRS_PI_UPDATE_PERIOD_NS;
    sm->last_pi_update_ns = t_now;

    uint64_t new_rate = DRS_RATE_NOMINAL_Q32;
    int step = pi_ctrl_update(&sm->pi, theta_residual, period, &new_rate);

    if (step) {
        /* Step vclock to correct large offset, then reset PI */
        vclock_step_offset(&sm->vc, +theta_residual, t_now);
        vclock_set_rate(&sm->vc, DRS_RATE_NOMINAL_Q32, t_now);
        pi_ctrl_init(&sm->pi);
        if (tel) {
            telemetry_begin_write();
            tel->step_count++;
            telemetry_end_write();
        }

        /* Phase-step > 5 ms resets min-delay filter (F-14) */
        int64_t abs_step = theta_residual < 0 ? -theta_residual : theta_residual;
        if (abs_step > DRS_PHASE_STEP_RESET_NS) {
            min_delay_reset(&sm->mdf);
            /* step_accum_ns must NOT be reset here — it was just updated above */
        }
    } else {
        vclock_set_rate(&sm->vc, new_rate, t_now);
    }

    sm->last_rate_q32 = vclock_rate(&sm->vc);

    /* Convergence detection */
    int64_t abs_off = theta_residual < 0 ? -theta_residual : theta_residual;
    if (tel) {
        telemetry_begin_write();
        if (abs_off < DRS_STABLE_OFFSET_THRESH_NS) {
            if (!tel->convergence_unix_ns)
                tel->convergence_unix_ns = (uint64_t)t_now;
        } else {
            tel->convergence_unix_ns = 0;
        }
        tel->rate_q32_32 = (int64_t)sm->last_rate_q32;
        telemetry_end_write();
    }

    /* Emit UDP telemetry record for accepted sync exchange */
    telem_udp_emit(t4, DRS_STATE_FOLLOWER, theta_residual, rtt,
                   (int64_t)sm->last_rate_q32);
}

void sm_on_command(sm_t *sm, uint32_t cmd, int64_t t_now)
{
    switch (cmd) {
    case DRS_CMD_RECALIBRATE:
        if (calibrate_loopback(&sm->vc) != 0)
            errors_push(DRS_ERR_CALIB_TIMEOUT, "manual recalibrate failed");
        discipline_reset(sm);
        break;

    case DRS_CMD_FORCE_HOLDOVER:
        enter_holdover(sm, t_now);
        break;

    case DRS_CMD_FORCE_DEMOTE:
        if (sm->state == DRS_STATE_LEADER)
            enter_candidate(sm, t_now);
        break;

    case DRS_CMD_RESET_FILTERS:
        discipline_reset(sm);
        break;

    case DRS_CMD_FORCE_LEADER:
        sm->el.lock_mode = DRS_LOCK_AS_LEADER;
        sm->el.own_term  = sm->el.own_term + DRS_LOCK_TERM_BOOST;
        enter_leader(sm, t_now);
        break;

    case DRS_CMD_FORCE_FOLLOWER:
        sm->el.lock_mode = DRS_LOCK_AS_FOLLOWER;
        if (sm->state == DRS_STATE_LEADER)
            enter_listen(sm, t_now);
        break;

    case DRS_CMD_AUTO_MODE:
        sm->el.lock_mode = DRS_LOCK_AUTO;
        break;

    default:
        break;
    }
}
