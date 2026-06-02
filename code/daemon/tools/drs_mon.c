#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>

#include "../src/telemetry.h"
#include "../include/drs_sync_config.h"

/* ── ANSI ────────────────────────────────────────────────────────────── */
#define RST     "\033[0m"
#define BOLD    "\033[1m"
#define RED     "\033[31m"
#define GRN     "\033[32m"
#define YEL     "\033[33m"
#define BLU     "\033[34m"
#define CYN     "\033[36m"
#define WHT     "\033[37m"
#define GRY     "\033[90m"
#define BG_RED  "\033[41m"
#define BG_GRN  "\033[42m"
#define CLS     "\033[2J\033[H"

/* ── Signal ──────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_running = 1;
static void on_sig(int s) { (void)s; g_running = 0; }

/* ── Helpers ─────────────────────────────────────────────────────────── */
static const char *state_name(uint32_t s)
{
    switch (s) {
    case 0: return "GROUND";
    case 1: return "CALIBRATION";
    case 2: return "LISTEN";
    case 3: return "CANDIDATE";
    case 4: return "FOLLOWER";
    case 5: return "LEADER";
    case 6: return "HOLDOVER";
    default: return "UNKNOWN";
    }
}
static const char *state_col(uint32_t s)
{
    switch (s) {
    case 5: return GRN;
    case 4: return CYN;
    case 6: return RED;
    case 2: case 3: return YEL;
    default: return GRY;
    }
}
static const char *lock_name(uint32_t m)
{
    switch (m) {
    case DRS_LOCK_AUTO:        return "AUTO";
    case DRS_LOCK_AS_LEADER:   return "LOCK_LEADER";
    case DRS_LOCK_AS_FOLLOWER: return "LOCK_FOLLOWER";
    default: return "?";
    }
}
static const char *off_col(int64_t ns)
{
    int64_t a = ns < 0 ? -ns : ns;
    if (a < 50000)  return GRN;
    if (a < 100000) return YEL;
    return RED;
}

static drs_telemetry_t snap(const drs_telemetry_t *t)
{
    drs_telemetry_t s;
    uint32_t s1, s2 = 0;
    do {
        s1 = atomic_load_explicit(&t->seqlock, memory_order_acquire);
        if (s1 & 1) continue;
        s  = *t;
        s2 = atomic_load_explicit(&t->seqlock, memory_order_acquire);
    } while (s1 != s2);
    return s;
}

static void bar(double val, double max, int w, const char *col)
{
    int f = (int)(val / max * w);
    if (f > w) f = w;
    if (f < 0) f = 0;
    printf("%s[", col);
    for (int i = 0; i < w; i++) putchar(i < f ? '#' : ' ');
    printf("]" RST);
}

/* ── Holdover banner ─────────────────────────────────────────────────── */
static void print_holdover_banner(int in_holdover, uint32_t holdover_ms,
                                  uint32_t holdover_count)
{
    if (in_holdover) {
        printf(BG_RED BOLD
               "  !!  HOLDOVER ACTIVE  —  Leader lost  —  Freerun mode  !!  \n"
               RST);
        printf(RED BOLD "  Remaining: %u ms   (max %d s)   Total HOLDOVER events: %u\n"
               RST "\n",
               holdover_ms,
               (int)(DRS_HOLDOVER_MAX_NS / INT64_C(1000000000)),
               holdover_count);
    } else {
        printf(BG_GRN BOLD
               "  OK  SYNC ACTIVE  —  Not in holdover                      \n"
               RST);
        if (holdover_count > 0)
            printf(GRY "  (HOLDOVER entered %u time%s this session)\n" RST,
                   holdover_count, holdover_count == 1 ? "" : "s");
        printf("\n");
    }
}

/* ── Main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    int interval_ms = 500;
    if (argc > 1) interval_ms = atoi(argv[1]);
    if (interval_ms < 100)  interval_ms = 100;
    if (interval_ms > 5000) interval_ms = 5000;

    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);

    int fd = open(DRS_SHM_STATE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr,
            RED "drs_mon: cannot open %s\n"
            "Is drs_syncd running?\n" RST, DRS_SHM_STATE_PATH);
        return 1;
    }
    drs_telemetry_t *tel = mmap(NULL, sizeof(drs_telemetry_t),
                                PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (tel == MAP_FAILED) { perror("mmap"); return 1; }

    /* Previous values for delta computation */
    int64_t  prev_offset    = 0;
    int64_t  prev_rate      = (int64_t)DRS_RATE_NOMINAL_Q32;
    uint32_t prev_pulse     = 0;
    uint32_t prev_accepted  = 0;
    uint32_t prev_rejected  = 0;
    uint32_t prev_steps     = 0;
    int      first          = 1;
    int      tick           = 0;

    struct timespec ts = {
        .tv_sec  = interval_ms / 1000,
        .tv_nsec = (interval_ms % 1000) * 1000000L,
    };

    while (g_running) {
        drs_telemetry_t s = snap(tel);

        int    in_holdover = (s.state == 6);
        double offset_us   = (double)s.offset_ns / 1000.0;
        double rate_ppm    = (double)(s.rate_q32_32 - (int64_t)DRS_RATE_NOMINAL_Q32)
                             / DRS_RATE_PPM_Q32;
        double rtt_us      = (double)s.last_rtt_ns / 1000.0;
        double lat_us      = (double)s.latency_corr_ns / 1000.0;
        double d_off       = first ? 0.0 : (double)(s.offset_ns - prev_offset) / 1000.0;
        double d_rate      = first ? 0.0 :
                             (double)(s.rate_q32_32 - prev_rate) / DRS_RATE_PPM_Q32;
        uint32_t d_pulse   = s.pulse_count   - prev_pulse;
        uint32_t d_acc     = s.sync_accepted - prev_accepted;
        uint32_t d_rej     = s.sync_rejected - prev_rejected;
        uint32_t d_steps   = s.step_count    - prev_steps;
        double accept_pct  = (d_acc + d_rej) > 0
                             ? 100.0 * d_acc / (d_acc + d_rej) : 100.0;

        printf(CLS);

        /* ── Header ──────────────────────────────────────────────── */
        printf(BOLD "╔══════════════════════════════════════════════════╗\n"
                    "║          drs_mon  —  live telemetry              ║\n"
                    "╚══════════════════════════════════════════════════╝\n" RST);
        printf(GRY "  refresh %d ms  |  tick %d  |  PID %u\n\n" RST,
               interval_ms, tick++, s.pid);

        /* ── Holdover banner (always shown) ─────────────────────── */
        print_holdover_banner(in_holdover, s.holdover_remaining_ms,
                              s.holdover_count);

        /* ── State & identity ────────────────────────────────────── */
        printf(BOLD "  State      " RST ": %s%s%s",
               state_col(s.state), state_name(s.state), RST);
        if (s.flags & DRS_FLAG_LEADER)     printf(GRN " [LEADER]" RST);
        if (s.flags & DRS_FLAG_HOLDOVER)   printf(RED " [HOLDOVER]" RST);
        if (s.flags & DRS_FLAG_CALIBRATED) printf(CYN " [CAL]" RST);
        if (s.flags & DRS_FLAG_FAULT)      printf(RED " [FAULT]" RST);
        printf("\n");
        printf(BOLD "  Leader ID  " RST ": %-5u  "
               BOLD "Term" RST ": %-5u  "
               BOLD "Lock" RST ": %s\n\n",
               s.leader_node_id, s.election_term, lock_name(s.lock_mode));

        /* ── Offset ──────────────────────────────────────────────── */
        double abs_us = offset_us < 0 ? -offset_us : offset_us;
        printf(BOLD "  Offset     " RST ": %s%+9.3f µs" RST,
               off_col(s.offset_ns), offset_us);
        if (!first) {
            const char *dc = d_off > 0 ? YEL : CYN;
            printf("  %s(Δ %+.3f µs/tick)" RST, dc, d_off);
        }
        printf("\n  ");
        bar(abs_us, 200.0, 32, off_col(s.offset_ns));
        printf("  %.1f / 200 µs\n\n", abs_us);

        /* ── Rate ────────────────────────────────────────────────── */
        double abs_ppm = rate_ppm < 0 ? -rate_ppm : rate_ppm;
        printf(BOLD "  Rate       " RST ": %s%+9.4f ppm" RST,
               abs_ppm < 100 ? GRN : YEL, rate_ppm);
        if (!first && s.rate_q32_32 != prev_rate)
            printf(GRY "  (Δ %+.4f ppm/tick)" RST, d_rate);
        printf("\n  ");
        bar(abs_ppm, 1000.0, 32, abs_ppm < 100 ? GRN : YEL);
        printf("  %.2f / 1000 ppm\n\n", abs_ppm);

        /* ── Network ─────────────────────────────────────────────── */
        printf(BOLD "  RTT (last) " RST ": %7.3f µs\n", rtt_us);
        printf(BOLD "  Lat corr   " RST ": %7.3f µs\n\n", lat_us);

        /* ── Convergence ─────────────────────────────────────────── */
        if (s.convergence_unix_ns)
            printf(BOLD "  Converged  " RST ": " GRN "YES" RST
                   GRY "  (GPIO 23 → LOW)" RST "\n");
        else
            printf(BOLD "  Converged  " RST ": " YEL "NO " RST
                   GRY "  (|offset| ≥ 100 µs or < 10 s stable)" RST "\n");
        printf("\n");

        /* ── Counters ────────────────────────────────────────────── */
        printf(BOLD "  ── Counters ───────────────────────────────────\n" RST);

        /* Sync samples */
        printf(BOLD "  Sync OK    " RST ": %6u total  " GRY "+%u this tick" RST "\n",
               s.sync_accepted, d_acc);
        printf(BOLD "  Sync REJ   " RST ": %6u total  " GRY "+%u this tick" RST,
               s.sync_rejected, d_rej);
        if (!first) {
            const char *ac = accept_pct >= 90 ? GRN : (accept_pct >= 70 ? YEL : RED);
            printf("  %saccept rate %.0f %%" RST, ac, accept_pct);
        }
        printf("\n");

        /* Phase steps */
        printf(BOLD "  Steps      " RST ": %6u total", s.step_count);
        if (!first && d_steps > 0)
            printf("  " YEL "+%u this tick" RST, d_steps);
        printf("\n");

        /* Holdover events */
        printf(BOLD "  HOLDOVER # " RST ": %6u %s\n",
               s.holdover_count,
               s.holdover_count == 0 ? GRN "(none)" RST :
               s.holdover_count < 3  ? YEL "(few)"  RST :
                                       RED "(many)"  RST);

        /* Pulses */
        printf(BOLD "  Pulses     " RST ": %6u total  " GRY "+%u this tick" RST "\n",
               s.pulse_count, d_pulse);

        /* Errors */
        if (s.error_count > 0)
            printf(BOLD "  Errors     " RST ": " RED "%u total, last code = %u" RST "\n",
                   s.error_count, s.error_code_last);
        else
            printf(BOLD "  Errors     " RST ": " GRN "0" RST "\n");

        printf("\n" GRY "  Ctrl+C to exit\n" RST);
        fflush(stdout);

        prev_offset   = s.offset_ns;
        prev_rate     = s.rate_q32_32;
        prev_pulse    = s.pulse_count;
        prev_accepted = s.sync_accepted;
        prev_rejected = s.sync_rejected;
        prev_steps    = s.step_count;
        first         = 0;

        nanosleep(&ts, NULL);
    }

    printf("\n" RST "drs_mon: bye.\n");
    munmap(tel, sizeof(drs_telemetry_t));
    return 0;
}
