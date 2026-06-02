#ifndef DRS_SYNC_CONFIG_H
#define DRS_SYNC_CONFIG_H

#include <stdint.h>

/* ── Protocol ─────────────────────────────────────────────────────────── */
#define DRS_MAGIC_V2             UINT32_C(0x44525354)   /* "DRST" */

/* Protocol version selects wire-frame size.
 * Override at build time: make PROTO_VER=1
 *   V1: 66 bytes total, 12 bytes padding
 *   V2: 64 bytes total, 10 bytes padding  (default)
 */
#ifndef DRS_PROTO_VER
#  define DRS_PROTO_VER  2
#endif

#if DRS_PROTO_VER == 1
#  define DRS_VERSION        UINT8_C(0x01)
#  define DRS_PAYLOAD_BYTES  66
#elif DRS_PROTO_VER == 2
#  define DRS_VERSION        UINT8_C(0x02)
#  define DRS_PAYLOAD_BYTES  64
#else
#  error "DRS_PROTO_VER must be 1 or 2"
#endif

#define DRS_PORT                 47200
#define DRS_MCAST_GROUP          "239.192.88.100"
#define DRS_CALIB_LOOPBACK_PORT  47201

/* Message types */
#define DRS_MSG_ANNOUNCE         0x01
#define DRS_MSG_SYNC_REQ         0x02
#define DRS_MSG_SYNC_RESP        0x03

/* Flags bitmask */
#define DRS_FLAG_LEADER          (1u << 0)
#define DRS_FLAG_HOLDOVER        (1u << 1)
#define DRS_FLAG_CALIBRATED      (1u << 2)
#define DRS_FLAG_FAULT           (1u << 3)

/* ── Timing (all in nanoseconds) ──────────────────────────────────────── */
#define DRS_GROUND_DURATION_NS        INT64_C(2000000000)   /* 2 s   */
#define DRS_LEADER_ANNOUNCE_NS        INT64_C(100000000)    /* 100 ms */
#define DRS_SYNC_EXCHANGE_NS          INT64_C(50000000)     /* 50 ms  */
#define DRS_FOLLOWER_TIMEOUT_NS       INT64_C(300000000)    /* 300 ms */
#define DRS_HOLDOVER_MAX_NS           INT64_C(10000000000)  /* 10 s   */
#define DRS_ELECTION_MIN_NS           INT64_C(250000000)    /* 250 ms */
#define DRS_ELECTION_MAX_NS           INT64_C(500000000)    /* 500 ms */
#define DRS_SYNC_REQ_TIMEOUT_NS       INT64_C(200000000)    /* 200 ms */

/* ── Pulse Engine ─────────────────────────────────────────────────────── */
#define DRS_PULSE_PERIOD_NS           INT64_C(1000000000)   /* 1 s    */
#define DRS_PULSE_HIGH_DURATION_NS    INT64_C(10000000)     /* 10 ms  */
#define DRS_STABLE_WINDOW_NS          INT64_C(10000000000)  /* 10 s   */
#define DRS_STABLE_OFFSET_THRESH_NS   INT64_C(100000)       /* 100 µs */

/* ── Min-Delay Filter ─────────────────────────────────────────────────── */
#define DRS_MIN_DELAY_WINDOW          10
/* V6: 50 µs on real GbE, raised to 200 µs for WSL virtual stack jitter */
#define DRS_MIN_DELAY_TOLERANCE_NS    INT64_C(200000)       /* 200 µs */
#define DRS_SEQ_DISC_THRESHOLD        1024u
#define DRS_PHASE_STEP_RESET_NS       INT64_C(5000000)      /* 5 ms   */

/* ── Calibration ──────────────────────────────────────────────────────── */
#define DRS_CALIB_SAMPLES             50
#define DRS_CALIB_OUTLIER_NS          INT64_C(20000)        /* 20 µs  */
#define DRS_CALIB_MAX_RETRIES         3

/* ── PI Controller ────────────────────────────────────────────────────── */
#define DRS_PI_KP_Q16                 6554    /* ≈ 0.1  (Q16.16) */
#define DRS_PI_KI_Q16                 655     /* ≈ 0.01 (Q16.16) */
#define DRS_PI_UPDATE_PERIOD_NS       INT64_C(50000000)     /* 50 ms  */
#define DRS_STEP_THRESHOLD_NS         INT64_C(1000000)      /* 1 ms   */
#define DRS_STEP_CONFIRM_SAMPLES      3

/* ── Virtual Clock (Q32.32) ───────────────────────────────────────────── */
#define DRS_RATE_NOMINAL_Q32          UINT64_C(0x100000000) /* 1.0    */
#define DRS_RATE_PPM_Q32              INT64_C(4295)         /* 2^32/1e6 */
#define DRS_RATE_MAX_PPM              INT64_C(1000)
#define DRS_RATE_MIN_Q32              (DRS_RATE_NOMINAL_Q32 - (uint64_t)(DRS_RATE_MAX_PPM * DRS_RATE_PPM_Q32))
#define DRS_RATE_MAX_Q32              (DRS_RATE_NOMINAL_Q32 + (uint64_t)(DRS_RATE_MAX_PPM * DRS_RATE_PPM_Q32))

/* ── RT Configuration ─────────────────────────────────────────────────── */
#define DRS_RT_CPU                    3
#define DRS_RT_PRIO                   85

/* ── GPIO ─────────────────────────────────────────────────────────────── */
#define DRS_GPIO_PULSE_PIN            18
#define DRS_GPIO_HEALTH_PIN           23

/* ── Lock Mode ────────────────────────────────────────────────────────── */
#define DRS_LOCK_AUTO                 0
#define DRS_LOCK_AS_LEADER            1
#define DRS_LOCK_AS_FOLLOWER          2
#define DRS_LOCK_TERM_BOOST           1000u

/* ── Shared Memory paths ──────────────────────────────────────────────── */
#define DRS_SHM_STATE_PATH            "/dev/shm/drs-sync.state"
#define DRS_SHM_CMD_PATH              "/dev/shm/drs-sync.cmd"
#define DRS_SHM_ERRORS_PATH           "/dev/shm/drs-sync.errors"

/* ── Telemetry / Errors ───────────────────────────────────────────────── */
#define DRS_TELEMETRY_VERSION         1u
#define DRS_ERROR_RING_SLOTS          64
#define DRS_RETRY_STORM_THRESHOLD     5
#define DRS_RETRY_STORM_RATE_NS       INT64_C(200000000)    /* 5 Hz   */

/* ── Holdover retry ───────────────────────────────────────────────────── */
#define DRS_HOLDOVER_REAPPEAR_S       10

/* ── Error codes ──────────────────────────────────────────────────────── */
#define DRS_ERR_CALIB_TIMEOUT         1
#define DRS_ERR_HOLDOVER_EXPIRED      2
#define DRS_ERR_CRC_MISMATCH          3
#define DRS_ERR_SEQ_DISCONTINUITY     4
#define DRS_ERR_GPIO_OPEN_FAIL        5
#define DRS_ERR_RT_PRIO_FAIL          6
#define DRS_ERR_TX_TIMESTAMP_LOST     7
#define DRS_ERR_RX_TIMEOUT            8
#define DRS_ERR_INVALID_PACKET        9
#define DRS_ERR_NET_INIT_FAIL         10
#define DRS_ERR_SHM_INIT_FAIL         11

/* ── Command codes ────────────────────────────────────────────────────── */
#define DRS_CMD_NONE                  0
#define DRS_CMD_RECALIBRATE           1
#define DRS_CMD_FORCE_HOLDOVER        2
#define DRS_CMD_FORCE_DEMOTE          3
#define DRS_CMD_RESET_FILTERS         4
#define DRS_CMD_FORCE_LEADER          5
#define DRS_CMD_FORCE_FOLLOWER        6
#define DRS_CMD_AUTO_MODE             7

#endif /* DRS_SYNC_CONFIG_H */
