
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

#ifndef MALLOB_EPOCH_BITWIDTH
#define MALLOB_EPOCH_BITWIDTH 16
#define MALLOB_EPOCH_NEVER_SHARED ((1 << MALLOB_EPOCH_BITWIDTH) - 1)
#endif

// Packed struct to get in all meta data for a produced clause.
struct __attribute__ ((packed)) ClauseInfo {

    // Epoch of last modification (production, or sharing:=true)
    uint32_t lastSharedEpoch:MALLOB_EPOCH_BITWIDTH;
    uint32_t lastProducedEpoch:MALLOB_EPOCH_BITWIDTH;

    // Bitset of which local solver(s) exported the clause
    cls_producers_bitset producers:MALLOB_MAX_N_APPTHREADS_PER_PROCESS;

    ClauseInfo() {
        producers = 0;
        lastSharedEpoch = MALLOB_EPOCH_NEVER_SHARED;
        lastProducedEpoch = 0;
    }
    ClauseInfo(const ProducedClauseCandidate& c) {
        assert(c.producerId < MALLOB_MAX_N_APPTHREADS_PER_PROCESS);
        producers = 1 << c.producerId;
        lastSharedEpoch = MALLOB_EPOCH_NEVER_SHARED;
        lastProducedEpoch = c.epoch;
    }

    bool wasSharedBefore() const {
        return lastSharedEpoch != MALLOB_EPOCH_NEVER_SHARED;
    }

    bool isAdmissibleForInsertion(int epoch, int epochHorizon) const {
        return epochHorizon >= 0 && epoch - lastProducedEpoch > epochHorizon;
    }

    bool isAdmissibleForSharing(int epoch, int epochHorizon) const {
        if (!wasSharedBefore()) return true;
        return epochHorizon >= 0 && epoch - lastSharedEpoch > epochHorizon;
    }
};
