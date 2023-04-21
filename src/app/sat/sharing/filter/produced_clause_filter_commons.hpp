
#pragma once

#include "app/sat/data/produced_clause_candidate.hpp"

#ifndef MALLOB_MAX_N_APPTHREADS_PER_PROCESS
#define MALLOB_MAX_N_APPTHREADS_PER_PROCESS 32
#endif

#if MALLOB_MAX_N_APPTHREADS_PER_PROCESS <= 8
typedef uint8_t cls_producers_bitset;
#elif MALLOB_MAX_N_APPTHREADS_PER_PROCESS <= 16
typedef uint16_t cls_producers_bitset;
#elif MALLOB_MAX_N_APPTHREADS_PER_PROCESS <= 32
typedef uint32_t cls_producers_bitset;
#elif MALLOB_MAX_N_APPTHREADS_PER_PROCESS <= 64
typedef uint64_t cls_producers_bitset;
#elif MALLOB_MAX_N_APPTHREADS_PER_PROCESS <= 128
typedef uint128_t cls_producers_bitset;
#endif

#ifndef MALLOB_LBD_MAX_BITS
#define MALLOB_LBD_MAX_BITS 5
#endif

// Packed struct to get in all meta data for a produced clause.
struct __attribute__ ((packed)) ClauseInfo {

    // Best LBD so far this clause was PRODUCED (+inserted into buffer) with
    uint32_t minProducedLbd:MALLOB_LBD_MAX_BITS;
    // Best LBD so far this clause was SHARED to all solvers with
    uint32_t minSharedLbd:MALLOB_LBD_MAX_BITS;
    // Epoch of last modification (production, or sharing:=true)
    uint32_t lastSharedEpoch:22;

    // Bitset of which local solver(s) exported the clause
    cls_producers_bitset producers:MALLOB_MAX_N_APPTHREADS_PER_PROCESS;

    ClauseInfo() {
        minProducedLbd = 0;
        minSharedLbd = 0;
        producers = 0;
        lastSharedEpoch = 0;
    }
    ClauseInfo(const ProducedClauseCandidate& c) {
        minProducedLbd = truncateLbd(c.lbd);
        minSharedLbd = 0;
        assert(c.producerId < MALLOB_MAX_N_APPTHREADS_PER_PROCESS);
        producers = 1 << c.producerId;
        lastSharedEpoch = 0;
    }

    static int truncateLbd(int lbd) {
        return std::min(lbd, ((1<<MALLOB_LBD_MAX_BITS) - 1));
    }
};
