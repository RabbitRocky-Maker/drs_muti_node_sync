#include "election.h"
#include "net_io.h"   /* for mono_raw_ns() */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* xorshift64 PRNG seeded from /dev/urandom */
static uint64_t g_xr_state = 1;

static void seed_prng(void)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &g_xr_state, sizeof(g_xr_state)) < 0) { /* ignore */ }
        close(fd);
    }
    if (!g_xr_state)
        g_xr_state = 1;
}

static uint64_t xorshift64(void)
{
    g_xr_state ^= g_xr_state << 13;
    g_xr_state ^= g_xr_state >> 7;
    g_xr_state ^= g_xr_state << 17;
    return g_xr_state;
}

/* Random value in [min_ns, max_ns] */
static int64_t rand_range(int64_t min_ns, int64_t max_ns)
{
    int64_t span = max_ns - min_ns;
    return min_ns + (int64_t)(xorshift64() % (uint64_t)span);
}

void election_init(election_t *el, uint32_t own_node_id, int lock_mode)
{
    memset(el, 0, sizeof(*el));
    seed_prng();
    el->own_node_id  = own_node_id;
    el->own_term     = 0;
    el->lock_mode    = lock_mode;

    if (lock_mode == DRS_LOCK_AS_LEADER)
        el->own_term = DRS_LOCK_TERM_BOOST;

    election_arm_timeout(el);
}

void election_arm_timeout(election_t *el)
{
    el->election_timeout_ns = rand_range(DRS_ELECTION_MIN_NS, DRS_ELECTION_MAX_NS);
    el->timeout_deadline_ns = mono_raw_ns() + el->election_timeout_ns;
}

elect_result_t election_on_announce(election_t *el, const drs_packet_t *pkt)
{
    if (el->lock_mode == DRS_LOCK_AS_LEADER)
        return ELECT_NO_CHANGE; /* ignore all foreign ANNOUNCEs */

    uint32_t peer_term = pkt->election_term;
    uint32_t peer_id   = pkt->node_id;

    if (peer_term > el->own_term) {
        if (peer_id > el->own_node_id) {
            /* Peer has higher term but also a higher ID — it bootstrapped
             * alone and claimed leadership, but we have the lower ID and
             * must be leader per spec.  Adopt their term so our next
             * election_promote() produces a term that beats them. */
            el->own_term = peer_term;
            return ELECT_NO_CHANGE;
        }
        /* Lower-ID peer with a higher term — legitimately won, yield. */
        el->own_term       = peer_term;
        el->leader_node_id = peer_id;
        election_arm_timeout(el);
        return ELECT_BECOME_FOLLOWER;
    }

    if (peer_term < el->own_term)
        return ELECT_NO_CHANGE; /* stale, ignore */

    /* peer_term == own_term — tiebreak on node ID */
    if (peer_id < el->own_node_id) {
        el->leader_node_id = peer_id;
        election_arm_timeout(el);
        return ELECT_BECOME_FOLLOWER;
    }

    return ELECT_NO_CHANGE;
}

void election_promote(election_t *el)
{
    el->own_term++;
    if (el->lock_mode == DRS_LOCK_AS_LEADER)
        el->own_term = DRS_LOCK_TERM_BOOST;
    el->leader_node_id = el->own_node_id;
}

int election_timed_out(const election_t *el)
{
    if (el->lock_mode == DRS_LOCK_AS_FOLLOWER)
        return 0; /* never time out in follower-lock mode */
    return mono_raw_ns() >= el->timeout_deadline_ns;
}
