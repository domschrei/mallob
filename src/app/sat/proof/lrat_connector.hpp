
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "app/sat/data/clause.hpp"
#include "app/sat/proof/trusted/trusted_solving.hpp"
#include "app/sat/proof/trusted_checker_process_adapter.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "trusted/trusted_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/background_worker.hpp"

#ifndef MALLOB_TRUSTED_SUBPROCESSING
#define MALLOB_TRUSTED_SUBPROCESSING 1
#endif

class LratConnector {

private:
    const int _local_id;
    SPSCBlockingRingbuffer<LratOp> _ringbuf;
    BackgroundWorker _bg_emitter;
    BackgroundWorker _bg_acceptor;
#if MALLOB_TRUSTED_SUBPROCESSING
    TrustedCheckerProcessAdapter _checker;
#else
    TrustedSolving _checker;
#endif

    ProbingLearnedClauseCallback _cb_probe;
    LearnedClauseCallback _cb_learn;

    bool _unsat_validated {false};
    bool _terminated {false};

    // buffering
    static constexpr int MAX_CLAUSE_LENGTH {512};
    int _clause_lits[MAX_CLAUSE_LENGTH];
    Mallob::Clause _clause {_clause_lits, 0, 0};

    const int* _f_data;
    size_t _f_size;

public:
    LratConnector(Logger& logger, int localId, int nbVars) : _local_id(localId), _ringbuf(1<<16),
#if MALLOB_TRUSTED_SUBPROCESSING
            _checker(logger, _local_id, nbVars) 
#else
            _checker(loggerCCallback, &logger, nbVars)
#endif
            {}

    inline auto& getChecker() {
        return _checker;
    }

    void setLearnedClauseCallback(const LearnedClauseCallback& cb) {_cb_learn = cb;}
    void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& cb) {_cb_probe = cb;}

    void launch(const int* fData, size_t fSize) {
        _f_data = fData;
        _f_size = fSize;

        // summary of formula for debugging
        //std::string summary;
        //for (size_t i = 0; i < std::min(5UL, _f_size); i++) summary += std::to_string(_f_data[i]) + " ";
        //if (_f_size > 10) summary += " ... ";
        //for (size_t i = std::max(5UL, _f_size-5); i < _f_size; i++) summary += std::to_string(_f_data[i]) + " ";
        //LOG(V2_INFO, "PROOF> got formula with %lu lits: %s\n", _f_size, summary.c_str());

        _bg_emitter.run([&]() {runEmitter();});
    }

    inline void push(LratOp&& op) {
        _ringbuf.pushBlocking(op);
    }
    bool waitForValidation() {
        while (!_terminated && !_unsat_validated) usleep(1000);
        return _unsat_validated;
    }

    void terminate() {
        _terminated = true;
        _ringbuf.markExhausted();
        _ringbuf.markTerminated();
        _bg_emitter.stop();
#if MALLOB_TRUSTED_SUBPROCESSING
        _checker.terminate();
#endif
        _bg_acceptor.stop();
    }

private:
    void runEmitter() {

        // Load formula
        size_t offset = 0;
        while (_bg_emitter.continueRunning() && offset < _f_size) {
            size_t nbInts = std::min(1UL<<16, _f_size-offset);
            _checker.load(_f_data+offset, nbInts);
            offset += nbInts;
        }
        if (!_bg_emitter.continueRunning()) return;
        assert(offset == _f_size);

        // End loading, check signature
        bool ok = _checker.endLoading();
        if (!ok) abort();

        // Start acceptor
        _bg_acceptor.run([&]() {runAcceptor();});

        // Lrat operation emission loop
        LratOp op;
        u8 sig[16];
        while (_bg_emitter.continueRunning()) {
            // Wait for an op, then poll it
            bool ok = _ringbuf.pollBlocking(op);
            if (!ok) continue;
            //LOG(V2_INFO, "PROOF> submit %s\n", op.toStr().c_str());
            _checker.submit(op);
        }
    }

    void runAcceptor() {
        LratOp op;
        u8 sig[16];
        while (_bg_acceptor.continueRunning()) {
            bool res;
            bool ok = _checker.accept(op, res, sig);
            if (!ok) break;
            //LOG(V2_INFO, "PROOF> accept (%i) %s\n", (int)res, op.toStr().c_str());
            if (!res) abort();
            if (op.isDerivation()) {
                bool share = op.getGlue() > 0 && _cb_probe(op.getNbLits());
                if (share) {
                    prepareClause(op, sig);
                    _cb_learn(_clause, _local_id);
                }
            } else if (op.isUnsatValidation()) {
                _unsat_validated = true;
            }
        }
    }

    inline void prepareClause(LratOp& op, const u8* sig) {
        _clause.size = 2+4+op.getNbLits();
        assert(_clause.size <= MAX_CLAUSE_LENGTH);
        auto id = op.getId();
        memcpy(_clause.begin, &id, 8);
        memcpy(_clause.begin+2, sig, 16);
        memcpy(_clause.begin+2+4, op.getLits(), op.getNbLits()*sizeof(int));
        _clause.lbd = op.getGlue();
        if (op.getNbLits() == 1) _clause.lbd = 1;
        else _clause.lbd = std::min(_clause.lbd, _clause.size);
    }
};
