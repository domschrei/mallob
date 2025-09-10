
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <unistd.h>

#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/parse/serialized_formula_parser.hpp"
#include "app/sat/proof/impcheck.hpp"
#include "app/sat/proof/lrat_op_tamperer.hpp"
#include "app/sat/proof/trusted/trusted_checker_defs.hpp"
#include "app/sat/proof/trusted_checker_process_adapter.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "robin_map.h"
#include "trusted/trusted_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/terminator.hpp"

class LratConnector {

private:
    Logger& _logger;
    const int _base_seed;
    const int _local_id;
    std::string _out_path;

    Mutex _mtx_submit;
    SPSCBlockingRingbuffer<LratOp> _ringbuf;

    TrustedCheckerProcessAdapter _checker;
    BackgroundWorker _bg_acceptor;
    BackgroundWorker _bg_emitter;

    ProbingLearnedClauseCallback _cb_probe;
    LearnedClauseCallback _cb_learn;

    bool _launched {false};

    // buffering
    static constexpr int MAX_CLAUSE_LENGTH {512};
    int _clause_lits[MAX_CLAUSE_LENGTH];
    Mallob::Clause _clause {_clause_lits, 0, 0};

    std::list<std::unique_ptr<SerializedFormulaParser>> _f_parsers;
    volatile bool _do_parse {false};
    volatile int _arrived_revision {-1};
    volatile int _accepted_revision {-1};
    volatile int _last_concluded_rev {-1};
    volatile int _next_rev_to_conclude {-1};

    float _tampering_chance_per_mille {0};

    tsl::robin_map<int, std::shared_ptr<LratOp>> _deferred_conclusion_ops;

public:
    LratConnector(TrustedCheckerProcessAdapter::TrustedCheckerProcessSetup& setup) :
        _logger(setup.logger), _base_seed(setup.baseSeed), _local_id(setup.localSolverId),
        _ringbuf(1<<14), _checker(setup) {}

    inline auto& getChecker() {
        return _checker;
    }

    void setLearnedClauseCallback(const LearnedClauseCallback& cb) {_cb_learn = cb;}
    void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& cb) {_cb_probe = cb;}
    void setTamperingChancePerMille(float chance) {
        _tampering_chance_per_mille = chance;
    }
    void setWitnessOutputPath(const std::string& outputPath) {_out_path = outputPath;}

    void initiateRevision(int revision, SerializedFormulaParser& fParser) {
        {
            auto lock = _mtx_submit.getLock();
            if (revision != _arrived_revision+1) {
                LOGGER(_logger, V1_WARN, "[WARN] LRAT Connector: unexpected rev. %i (in rev. %i now)!\n", revision, _arrived_revision);
                return;
            }
            _f_parsers.emplace_back(new SerializedFormulaParser(fParser));
            _arrived_revision++;
            _do_parse = true;
        }

        if (_launched) return;
        _launched = true;

        // summary of formula for debugging
        //std::string summary;
        //for (size_t i = 0; i < std::min(5UL, _f_size); i++) summary += std::to_string(_f_data[i]) + " ";
        //if (_f_size > 10) summary += " ... ";
        //for (size_t i = std::max(5UL, _f_size-5); i < _f_size; i++) summary += std::to_string(_f_data[i]) + " ";
        //LOG(V2_INFO, "PROOF> got formula with %lu lits: %s\n", _f_size, summary.c_str());

        _bg_emitter.run([&]() {runEmitter();});
    }

    inline bool push(LratOp&& op, bool acquireLock = true, int revision = -1) {

        if (acquireLock) {
            _mtx_submit.lock();
            // Wait until parsing is done!
            while (_do_parse) {
                _mtx_submit.unlock();
                usleep(1000*5);
                _mtx_submit.lock();
            }
        }

        // Currently unsuitable validation operation?
        if (op.isSatValidation() || op.isUnsatValidation()) {
            if (_next_rev_to_conclude < revision) {
                LOGGER(_logger, V2_INFO, "IMPCHK defer conclusion for rev. %i (rev. %i next to validate)\n",
                    revision, _next_rev_to_conclude);
                if (!_deferred_conclusion_ops.count(revision))
                    _deferred_conclusion_ops[revision] = std::shared_ptr<LratOp>(new LratOp(std::move(op)));
                if (acquireLock) _mtx_submit.unlock();
                return true;
            }
            if (_next_rev_to_conclude > revision) {
                LOGGER(_logger, V2_INFO, "IMPCHK discard conclusion for rev. %i (rev. %i arrived, rev. %i next to validate)\n",
                    revision, _arrived_revision, _next_rev_to_conclude);
                if (acquireLock) _mtx_submit.unlock();
                return false;
            }
        }

        if (op.isDerivation()) {
            // Clauses which will potentially be shared need to be sorted.
            // We use the glue value as an indicator for sharing (0 = no sharing)
            // and probe if the clause is currently eligible for sharing.
            // If the clause is not eligible, we set the glue to zero.
            // This skips the computation of a clause signature in the checker
            // and signals to the acceptor thread that the clause is NOT to be shared.
            auto& data = op.data.produce;
            int& glue = data.glue;
            bool share = glue > 0 && _cb_probe(data.nbLits);
            if (share) op.sortLiterals();
            else glue = 0;

            if (MALLOB_UNLIKELY(_tampering_chance_per_mille > 0)) {
                if (1000*Random::rand() <= _tampering_chance_per_mille) {
                    // Tampering with this derivation!
                    data.glue = 0;
                    LratOpTamperer(_logger).tamper(op);
                }
            }
        }
        if (MALLOB_UNLIKELY(op.isImport() && _tampering_chance_per_mille > 0)) {
            if (1000*Random::rand() <= _tampering_chance_per_mille) {
                // Tampering with this import!
                LratOpTamperer(_logger).tamper(op);
            }
        }
        if (op.isConcluding() && _next_rev_to_conclude == revision) {
            _next_rev_to_conclude++;
        }
        _ringbuf.pushBlocking(op);

        tryPushDeferredOp(false);

        if (acquireLock) _mtx_submit.unlock();
        return true;
    }
    void waitForConclusion(int revision) {
        _mtx_submit.lock();
        while (_last_concluded_rev < revision) {
            _mtx_submit.unlock();
            usleep(1000 * 3);
            _mtx_submit.lock();
        }
        _mtx_submit.unlock();
        return;
    }

    void stop() {

        // Tell solver and emitter thread to stop inserting/processing statements
        _ringbuf.markExhausted();
        _ringbuf.markTerminated();

        // Terminate the emitter thread
        _bg_emitter.stop();
        // Now _mtx_submit is no longer needed

        // Manually submit a termination sentinel to the checker process.
        LratOp end(TRUSTED_CHK_TERMINATE);
        _checker.submit(end);

        // Wait for the sentinel to make the round through the process
        // and back to the acceptor
        _bg_acceptor.join(); // NOT stop()! We want it to finish on its own!
        // Terminate signal arrived at the checker process, threads joined

        // Wait until the checker process did in fact exit
        _checker.terminate();
    }
    ~LratConnector() {
        stop();
    }

private:

    void tryPushDeferredOp(bool acquireLock) {

        if (acquireLock) _mtx_submit.lock();
        if (_do_parse || _arrived_revision < _next_rev_to_conclude) {
            if (acquireLock) _mtx_submit.unlock();
            return;
        }
        assert(!_do_parse);

        auto it = _deferred_conclusion_ops.find((int) _next_rev_to_conclude);
        if (it != _deferred_conclusion_ops.end()) {
            auto ptr = it->second;
            _deferred_conclusion_ops.erase(it);
            LOGGER(_logger, V2_INFO, "IMPCHK re-push deferred solution for rev. %i\n",
                _next_rev_to_conclude);
            push(std::move(*ptr), false, _next_rev_to_conclude);
        }
        if (acquireLock) _mtx_submit.unlock();
    }

    void runEmitter() {
        Proc::nameThisThread("LRATEmitter");

        // *Always* start acceptor to ensure sound termination
        _bg_acceptor.run([&]() {runAcceptor();});

        // Lrat operation emission loop
        LratOp op;
        int revision = -1;
        while (_bg_emitter.continueRunning()) {

            if (_do_parse && _mtx_submit.tryLock()) {
                while (!_f_parsers.empty()) {
                    // Load formula
                    auto parser = std::move(_f_parsers.front()); _f_parsers.pop_front();
                    // use the *old* revision index since this op may be considered
                    // a "conclusion" operation for the previous revision
                    push(LratOp(parser->getSignature()), false, revision);
                    trySubmitFromRingbuf();
                    revision++; // and *then* update the revision.
                    int lit;
                    std::vector<int> buf;
                    std::vector<int> assumptions;
                    assumptions.reserve(1); // suppress nullptr passing for empty assumptions
                    while (parser->getNextLiteral(lit)) {
                        do {
                            buf.push_back(lit);
                        } while (buf.size() < (1UL<<14) && parser->getNextLiteral(lit));
                        push(LratOp(TRUSTED_CHK_LOAD, buf.data(), buf.size()), false);
                        trySubmitFromRingbuf();
                        buf.clear();
                    }
                    while (parser->getNextAssumption(lit)) {
                        do {
                            assumptions.push_back(lit);
                        } while (parser->getNextAssumption(lit));
                    }
                    if (_bg_emitter.continueRunning()) {
                        // End loading, check signature
                        push(LratOp(TRUSTED_CHK_END_LOAD, assumptions.data(), assumptions.size()), false);
                        trySubmitFromRingbuf();
                    }
                }
                _do_parse = false;
                _mtx_submit.unlock();
            }

            if (!trySubmitFromRingbuf())
                usleep(1000*1); // 1ms
        }
    }

    bool trySubmitFromRingbuf() {
        LratOp op;
        if (_ringbuf.pollNonblocking(op)) { // poll for an op
            //LOG(V2_INFO, "PROOF> submit %s\n", op.toStr().c_str());
            _checker.submit(op);
            return true;
        }
        return false;
    }

    void runAcceptor() {
        Proc::nameThisThread("LRATAcceptor");

        LratOp op;
        signature sig;
        std::vector<int> assumptions;
        while (true) { // This thread can only be terminated with a "termination" LratOp.
            bool res;
            u32 cidx = 0;
            bool ok = _checker.accept(op, res, sig, cidx);
            if (!ok) break; // terminated
            if (!res) {
                continue; // error in checker - nonetheless, wait for proper termination
            }
            //LOG(V2_INFO, "PROOF> accept (%i) %s\n", (int)res, op.toStr().c_str());
            if (op.isDerivation()) {
                auto& data = op.data.produce;
                bool share = data.glue > 0;
                if (share) {
                    prepareClause(op, sig, cidx);
                    _cb_learn(_clause, _local_id);
                }
            } else if (op.isUnsatValidation() || op.isSatValidation()) {
                // SAT / UNSAT result
                _last_concluded_rev = _accepted_revision;
                LOGGER(_logger, V2_INFO, "Use impcheck_confirm -key-seed=%lu to confirm the fingerprint\n",
                    ImpCheck::getKeySeed(_base_seed));
                u32 code = op.isSatValidation() ? 10 : 20;
                if (op.isUnsatValidation())
                    assumptions = std::vector(op.data.concludeUnsat.failed, op.data.concludeUnsat.failed + op.data.concludeUnsat.nbFailed);
                std::string litStr;
                for (int lit : assumptions) litStr += " " + std::to_string(lit);
                LOGGER(_logger, V0_CRIT, "IMPCHK_CONFIRM_TRACE %u %u %s %s\n", cidx, code,
                    Logger::dataToHexStr(sig, SIG_SIZE_BYTES).c_str(), litStr.c_str());
                if (!_out_path.empty()) {
                    std::ofstream ofs(_out_path, std::ios_base::app);
                    ofs << cidx << " " << code << " " << Logger::dataToHexStr(sig, SIG_SIZE_BYTES) << litStr << std::endl;
                }
            } else if (op.isBeginLoad()) {
                if (cidx == 0) continue;
                // obsolete "unknown" result (not actually unknown)
                if (_last_concluded_rev == _accepted_revision) continue;
                // UNKNOWN result
                _last_concluded_rev = _accepted_revision;
                LOGGER(_logger, V0_CRIT, "IMPCHK_CONFIRM_TRACE %u %u\n", cidx, 0);
                if (!_out_path.empty()) {
                    std::ofstream ofs(_out_path, std::ios_base::app);
                    ofs << cidx << " 0" << std::endl;
                }
            } else if (op.isEndLoad()) {
                _accepted_revision++; // next revision reached
                LOGGER(_logger, V2_INFO, "IMPCHK revision %i reached\n", _accepted_revision);
                // store assumptions for later
                assumptions = std::vector(op.data.endLoad.assumptions, op.data.endLoad.assumptions + op.data.endLoad.nbAssumptions);
            } else if (op.isTermination()) {
                break; // end
            }
        }
    }

    inline void prepareClause(LratOp& op, const u8* sig, u32 cidx) {
        assert(op.isDerivation());
        auto& data = op.data.produce;
        assert(ClauseMetadata::numInts() == 2+4+1);
        _clause.size = 2+4+1+data.nbLits;
        assert(_clause.size <= MAX_CLAUSE_LENGTH);
        auto id = data.id;
        memcpy(_clause.begin, &id, sizeof(u64)); // ID
        memcpy(_clause.begin+2, sig, SIG_SIZE_BYTES); // Signature
        assert(_accepted_revision >= 0);
        memcpy(_clause.begin+2+4, &cidx, sizeof(u32)); // Revision
        memcpy(_clause.begin+2+4+1, data.lits, data.nbLits*sizeof(int)); // Literals
        _clause.lbd = data.glue;
        if (data.nbLits == 1) _clause.lbd = 1;
        else _clause.lbd = std::min(_clause.lbd, _clause.size);
    }
};
