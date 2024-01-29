
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

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

public:
    struct LratOp {
        size_t datalen {0};
        u8* data {nullptr};

        // Derivation / production / addition
        LratOp(u64 id, const int* lits, int nbLits, const u64* hints, int nbHints, int glue) :
                datalen(sizeof(nbLits) + sizeof(nbHints) + sizeof(id) 
                      + nbLits*sizeof(int) + nbHints*sizeof(u64) + sizeof(int)),
                data((u8*) malloc(datalen)) {
            size_t i = 0, n;
            assert(glue >= 0);
            n = sizeof(nbLits);      memcpy(data+i, &nbLits, n);  i += n;
            n = sizeof(nbHints);     memcpy(data+i, &nbHints, n); i += n;
            n = sizeof(id);          memcpy(data+i, &id, n);      i += n;
            n = sizeof(int)*nbLits;  memcpy(data+i, lits, n);     i += n;
            n = sizeof(u64)*nbHints; memcpy(data+i, hints, n);    i += n;
            n = sizeof(glue);        memcpy(data+i, &glue, n);    i += n;
            //assert(isDerivation());
        }
        // Import
        LratOp(u64 id, const int* lits, int nbLits, const u8* sig) :
                datalen(2*sizeof(int) + sizeof(id) + nbLits*sizeof(int) + 16 + sizeof(int)),
                data((u8*) malloc(datalen)) {
            size_t i = 0, n;
            const int nbHints = 16 / sizeof(u64);
            const int glue = -1;
            n = sizeof(nbLits);      memcpy(data+i, &nbLits, n);  i += n;
            n = sizeof(nbHints);     memcpy(data+i, &nbHints, n); i += n;
            n = sizeof(id);          memcpy(data+i, &id, n);      i += n;
            n = sizeof(int)*nbLits;  memcpy(data+i, lits, n);     i += n;
            n = sizeof(u64)*nbHints; memcpy(data+i, sig, n);      i += n;
            n = sizeof(glue);        memcpy(data+i, &glue, n);    i += n;
            //assert(isImport());
        }
        // Deletion
        LratOp(const u64* hints, int nbHints) :
                datalen(2*sizeof(int) + sizeof(u64) + nbHints*sizeof(u64)),
                data((u8*) malloc(datalen)) {
            size_t i = 0, n;
            const int nbLits = 0;
            const u64 id = 0;
            n = sizeof(nbLits);      memcpy(data+i, &nbLits, n);  i += n;
            n = sizeof(nbHints);     memcpy(data+i, &nbHints, n); i += n;
            n = sizeof(id);          memcpy(data+i, &id, n);      i += n;
            n = sizeof(u64)*nbHints; memcpy(data+i, hints, n);    i += n;
            //assert(isDeletion());
        }
        // UNSAT Validation
        LratOp() : datalen(1), data((u8*) malloc(1)) {
            data[0] = 20;
            //assert(isUnsatValidation());
        }
        LratOp(LratOp&& moved) : datalen(moved.datalen), data(moved.data) {
            moved.datalen = 0;
            moved.data = nullptr;
        }
        LratOp& operator=(LratOp&& moved) {
            this->datalen = moved.datalen;
            this->data = moved.data;
            moved.datalen = 0;
            moved.data = nullptr;
            return *this;
        }

        void sortLiterals() {
            std::sort(getLits(), getLits()+getNbLits());
        }

        bool isDerivation() const {return getType() == DERIVATION;}
        bool isImport() const {return getType() == IMPORT;}
        bool isDeletion() const {return getType() == DELETION;}
        bool isUnsatValidation() const {return getType() == VALIDATION;}

        enum Type {DERIVATION, IMPORT, DELETION, VALIDATION};
        Type getType() const {
            if (datalen == 1) return VALIDATION;
            if (getId() == 0) return DELETION;
            if (getGlue() < 0) return IMPORT;
            return DERIVATION;
        }

        int getNbLits() const {return *(u64*) (data);}
        int getNbHints() const {return *(u64*) (data + sizeof(int));}
        u64 getId() const {return *(u64*) (data + 2*sizeof(int));}
        int* getLits() {return (int*) (data + 2*sizeof(int) + sizeof(u64));}
        const int* getLits() const {return (int*) (data + 2*sizeof(int) + sizeof(u64));}
        const u64* getHints() const {return (u64*) (data + 2*sizeof(int) + sizeof(u64) + getNbLits()*sizeof(int));}
        const u8* getSignature() const {return (u8*) getHints();}
        int getGlue() const {return *(int*) (data + datalen - sizeof(int));}

        ~LratOp() {
            if (data) free(data);
        }
    };

private:
    const int _local_id;
    SPSCBlockingRingbuffer<LratOp> _ringbuf;
    BackgroundWorker _bg_worker;
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
    const int _subproc {MALLOB_TRUSTED_SUBPROCESSING};

public:
    LratConnector(Logger& logger, int localId, int nbVars) : _local_id(localId), _ringbuf(1<<16),
#if MALLOB_TRUSTED_SUBPROCESSING
            _checker(_local_id, nbVars) 
#else
            _checker(loggerCCallback, &logger, nbVars)
#endif
            {
        _bg_worker.run([&]() {runBackgroundWorker();});
    }

    inline auto& getChecker() {
        return _checker;
    }

    void setLearnedClauseCallback(const LearnedClauseCallback& cb) {_cb_learn = cb;}
    void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& cb) {_cb_probe = cb;}

    inline void push(LratOp&& op) {
        _ringbuf.pushBlocking(op);
    }
    bool waitForValidation() {
        while (!_terminated && !_unsat_validated) usleep(1000);
        return _unsat_validated;
    }

    void terminate() {
        _terminated = true;
        _bg_worker.stopWithoutWaiting();
        _ringbuf.markExhausted();
        _bg_worker.stop();
#if MALLOB_TRUSTED_SUBPROCESSING
        _checker.terminate();
#endif
    }

private:
    void runBackgroundWorker() {

        LratOp op;
        int sigSize {16};
        u8 sig[sigSize];

        // Lrat operation processing loop
        while (_bg_worker.continueRunning()) {

            // Wait for an op, then poll it
            bool ok = _ringbuf.pollBlocking(op);
            if (!ok) continue;

            auto type = op.getType();
            if (type == LratOp::DERIVATION) {
                // only supply non-null signature ptr if shareable
                bool share = op.getGlue() > 0 && _cb_probe(op.getNbLits());
                if (share) op.sortLiterals();
                bool ok = _checker.produceClause(op.getId(), op.getLits(), op.getNbLits(), op.getHints(), op.getNbHints(), share ? sig : nullptr, sigSize);
                if (!ok) abort();
                // forward to clause export interface
                if (!share) continue;
                prepareClause(op, sig);
                _cb_learn(_clause, _local_id);
            } else if (type == LratOp::IMPORT) {
                bool ok = _checker.importClause(op.getId(), op.getLits(), op.getNbLits(), op.getSignature(), 16);
                if (!ok) abort();
            } else if (type == LratOp::DELETION) {
                bool ok = _checker.deleteClauses(op.getHints(), op.getNbHints());
                if (!ok) abort();
            } else if (type == LratOp::VALIDATION) {
                bool ok = _checker.validateUnsat(sig, sigSize);
                if (!ok) abort();
                _unsat_validated = true;
                break;
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
