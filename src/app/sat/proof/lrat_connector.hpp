
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
#include "app/sat/proof/impcheck_program_lookup.hpp"
#include "app/sat/proof/lrat_op_tamperer.hpp"
#include "app/sat/proof/trusted/trusted_checker_defs.hpp"
#include "app/sat/proof/trusted_checker_process_adapter.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "data/serializable.hpp"
#include "robin_map.h"
#include "trusted/trusted_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/background_worker.hpp"

class LratConnector {

public:
    struct Witness : public Serializable {
        int cidx = 0;
        int result = 0;
        char data[SIG_SIZE_BYTES];
        std::vector<int> asmpt;

        bool valid() const {return cidx != 0 || result != 0;}
        virtual std::vector<uint8_t> serialize() const {
            std::vector<uint8_t> out(sizeof(int)*2 + SIG_SIZE_BYTES + sizeof(int)*asmpt.size());
            int i = 0, n;
            n = sizeof(int); memcpy(out.data()+i, &cidx, n); i += n;
            n = sizeof(int); memcpy(out.data()+i, &result, n); i += n;
            n = SIG_SIZE_BYTES; memcpy(out.data()+i, data, n); i += n;
            n = sizeof(int)*asmpt.size(); memcpy(out.data()+i, asmpt.data(), n); i += n;
            return out;
        }
        virtual Serializable& deserialize(const std::vector<uint8_t>& in) {
            int i = 0, n;
            n = sizeof(int); memcpy(&cidx, in.data()+i, n); i += n;
            n = sizeof(int); memcpy(&result, in.data()+i, n); i += n;
            n = SIG_SIZE_BYTES; memcpy(data, in.data()+i, n); i += n;
            int remainingBytes = in.size() - i;
            int asmptSize = remainingBytes / sizeof(int);
            asmpt.resize(asmptSize);
            n = remainingBytes; memcpy(asmpt.data(), in.data()+i, n); i += n;
            assert(i == in.size());
            return *this;
        }
    };

private:
    Logger& _logger;
    const int _base_seed;
    const int _local_id;
    const int _global_id;
    const int _job_id;
    const int _orig_nb_vars;
    bool _incremental;
    std::string _out_path;

    Mutex _mtx_submit;
    SPSCBlockingRingbuffer<LratOp> _ringbuf;

    TrustedCheckerProcessAdapter _checker;
    BackgroundWorker _bg_acceptor;
    BackgroundWorker _bg_emitter;

    ProbingLearnedClauseCallback _cb_probe;
    LearnedClauseCallback _cb_learn;

    volatile bool _launched {false};

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

    Mutex _mtx_witnesses;
    tsl::robin_map<int, Witness> _witness_by_revision;

public:
    LratConnector(TrustedCheckerProcessAdapter::TrustedCheckerProcessSetup& setup) :
        _logger(setup.logger), _base_seed(setup.baseSeed), _local_id(setup.localSolverId),
        _global_id(setup.globalSolverId), _job_id(setup.jobId), _orig_nb_vars(setup.origNumVars),
        _incremental(setup.incremental), _ringbuf(1<<14), _checker(setup) {}

    void init(const std::string& witnessSuffix = "") {
        assert(!_launched);
        _checker.init();
        if (_incremental) {
            std::string outPath = (_logger.getLogDir().empty() ? "." : _logger.getLogDir())
                + "/witness-trace." + std::to_string(_job_id) + "." + std::to_string(_global_id) + witnessSuffix + ".txt";
            _out_path = outPath;
        }
        _bg_emitter.run([&]() {runEmitter();});
        _bg_acceptor.run([&]() {runAcceptor();});
        _launched = true;
    }

    inline auto& getChecker() {
        return _checker;
    }

    void setLearnedClauseCallback(const LearnedClauseCallback& cb) {_cb_learn = cb;}
    void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& cb) {_cb_probe = cb;}
    void setTamperingChancePerMille(float chance) {
        _tampering_chance_per_mille = chance;
    }

    void initiateRevision(int revision, SerializedFormulaParser& fParser) {

        auto lock = _mtx_submit.getLock();
        if (revision != _arrived_revision+1) {
            LOGGER(_logger, V4_VVER, "LRAT Connector: reject rev. %i (in rev. %i now)\n", revision, _arrived_revision);
            return;
        }
        _f_parsers.emplace_back(new SerializedFormulaParser(fParser));
        _arrived_revision++;
        _do_parse = true;

        // summary of formula for debugging
        //std::string summary;
        //for (size_t i = 0; i < std::min(5UL, _f_size); i++) summary += std::to_string(_f_data[i]) + " ";
        //if (_f_size > 10) summary += " ... ";
        //for (size_t i = std::max(5UL, _f_size-5); i < _f_size; i++) summary += std::to_string(_f_data[i]) + " ";
        //LOG(V2_INFO, "PROOF> got formula with %lu lits: %s\n", _f_size, summary.c_str());
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
            if (_arrived_revision < revision || _next_rev_to_conclude < revision) {
                LOGGER(_logger, V4_VVER, "IMPCHK defer conclusion for rev. %i (rev. %i arrived, %i next to validate)\n",
                    revision, _arrived_revision, _next_rev_to_conclude);
                if (!_deferred_conclusion_ops.count(revision))
                    _deferred_conclusion_ops[revision] = std::shared_ptr<LratOp>(new LratOp(std::move(op)));
                if (acquireLock) _mtx_submit.unlock();
                return true;
            }
            if (_arrived_revision > revision || _next_rev_to_conclude > revision) {
                LOGGER(_logger, V4_VVER, "IMPCHK discard conclusion for rev. %i (rev. %i arrived, %i next to validate)\n",
                    revision, _arrived_revision, _next_rev_to_conclude);
                if (acquireLock) _mtx_submit.unlock();
                return false;
            }
            // revision == _next_rev_to_conclude == _arrived_revision
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
            bool share = glue > 0 && _cb_probe && _cb_probe(data.nbLits);
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
            _next_rev_to_conclude++; // prevent repeated validation of a result
        }
        _ringbuf.pushBlocking(op);

        if (acquireLock) _mtx_submit.unlock();
        return true;
    }
    Witness waitForConclusion(int revision) {

        // Wait for revision to conclude
        _mtx_submit.lock();
        u64 sleepMicrosecs = 100;
        while (_last_concluded_rev < revision) {
            _mtx_submit.unlock();
            usleep(sleepMicrosecs);
            sleepMicrosecs = std::min((u64) (1.2 * sleepMicrosecs), 3000UL);
            _mtx_submit.lock();
        }
        _mtx_submit.unlock();

        // Try to retrieve witness
        Witness w;
        _mtx_witnesses.lock();
        auto it = _witness_by_revision.find(revision);
        if (it != _witness_by_revision.end()) {
            w = std::move(it->second);
            _witness_by_revision.erase(it);
        }
        return w;
    }
    bool checkForConclusion(int revision) {
        auto lock = _mtx_submit.getLock();
        return _last_concluded_rev >= revision;
    }

    void prepareStop() {
        if (!_launched) return;

        // Tell solver and emitter thread to stop inserting/processing statements
        _ringbuf.markExhausted();
        _ringbuf.markTerminated();

        // Terminate the emitter thread
        _bg_emitter.stopWithoutWaiting();
    }

    void stop() {
        if (!_launched) return;
        prepareStop();
        _bg_emitter.stop();

        // In order not to interfere with a concurrent initiateRevision call
        auto lock = _mtx_submit.getLock();

        // Manually submit a termination sentinel to the checker process.
        LratOp end(TRUSTED_CHK_TERMINATE);
        _checker.submit(end); // ignored if checker was never initialized

        // Wait for the sentinel to make the round through the process
        // and back to the acceptor (if they were initialized)
        _bg_acceptor.join(); // NOT stop()! We want it to finish on its own!
        // Terminate signal arrived at the checker process, threads joined (if any)

        // Wait until any checker process did in fact exit
        _checker.terminate();
        _launched = false;
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
            LOGGER(_logger, V4_VVER, "IMPCHK re-push deferred solution for rev. %i\n",
                _next_rev_to_conclude);
            push(std::move(*ptr), false, _next_rev_to_conclude);
        }
        if (acquireLock) _mtx_submit.unlock();
    }

    void runEmitter() {
        Proc::nameThisThread("LRATEmitter");

        // Lrat operation emission loop
        LratOp op;
        int revision = -1;
        while (_bg_emitter.continueRunning()) {

            if (_do_parse && _mtx_submit.tryLock()) {
                // Flush all prior operations
                while (trySubmitFromRingbuf()) {}
                // Process all new revisions
                while (!_f_parsers.empty()) {
                    // Load formula
                    auto parser = std::move(_f_parsers.front()); _f_parsers.pop_front();
                    // use the *old* revision index since this op may be considered
                    // a "conclusion" operation for the previous revision
                    _checker.submit(LratOp(_orig_nb_vars, parser->getSignature()));
                    revision++; // and *then* update the revision.
                    assert(_next_rev_to_conclude <= revision);
                    _next_rev_to_conclude = revision;
                    int lit;
                    std::vector<int> buf;
                    std::vector<int> assumptions;
                    assumptions.reserve(1); // suppress nullptr passing for empty assumptions
                    while (parser->getNextLiteral(lit)) {
                        do {
                            buf.push_back(lit);
                        } while (buf.size() < (1UL<<14) && parser->getNextLiteral(lit));
                        _checker.submit(LratOp(TRUSTED_CHK_LOAD, buf.data(), buf.size()));
                        buf.clear();
                    }
                    while (parser->getNextAssumption(lit)) {
                        do {
                            assumptions.push_back(lit);
                        } while (parser->getNextAssumption(lit));
                    }
                    // End loading, check signature
                    _checker.submit(LratOp(TRUSTED_CHK_END_LOAD, assumptions.data(), assumptions.size()));
                    tryPushDeferredOp(false);
                }
                _do_parse = false;
                _mtx_submit.unlock();
            }

            if (!trySubmitFromRingbuf()) usleep(1000*1); // 1ms
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
            if (!ok || op.isTermination()) break; // terminated
            if (!res) {
                continue; // error in checker - nonetheless, wait for proper termination
            }
            //LOG(V2_INFO, "PROOF> accept (%i) %s\n", (int)res, op.toStr().c_str());
            if (op.isDerivation()) {
                auto& data = op.data.produce;
                bool share = data.glue > 0 && _cb_learn;
                if (share) {
                    prepareClause(op, sig, cidx);
                    _cb_learn(_clause, _local_id);
                }
            } else if (op.isUnsatValidation() || op.isSatValidation()) {
                // SAT / UNSAT result
                _last_concluded_rev = _accepted_revision;
                u32 code = op.isSatValidation() ? 10 : 20;
                if (op.isUnsatValidation())
                    assumptions = std::vector(op.data.concludeUnsat.failed, op.data.concludeUnsat.failed + op.data.concludeUnsat.nbFailed);
                std::string litStr;
                for (int lit : assumptions) litStr += " " + std::to_string(lit);
                if (_out_path.empty() || _last_concluded_rev == 0) {
                    LOGGER(_logger, V0_CRIT, "IMPCHK_CONFIRM %u %u %s%s\n", cidx, code,
                        Logger::dataToHexStr(sig, SIG_SIZE_BYTES).c_str(), litStr.c_str());
                    if (_last_concluded_rev == 0)
                        LOGGER(_logger, V2_INFO, "Use %s -key-seed=%lu to confirm fingerprint\n",
                            ImpCheckProgramLookup::getConfirmerExecutablePath(_incremental).c_str(),
                            ImpCheck::getKeySeed(_base_seed));
                }
                if (!_out_path.empty()) {
                    std::ofstream ofs(_out_path, std::ios_base::app);
                    ofs << cidx << " " << code << " " << Logger::dataToHexStr(sig, SIG_SIZE_BYTES) << litStr << std::endl;
                }
                Witness w;
                w.cidx = cidx;
                w.result = code;
                memcpy(w.data, sig, SIG_SIZE_BYTES);
                w.asmpt = assumptions;
                auto lock = _mtx_witnesses.getLock();
                _witness_by_revision[(int) _last_concluded_rev] = std::move(w);
            } else if (op.isBeginLoad()) {
                if (cidx == 0) continue;
                // obsolete "unknown" result (not actually unknown)
                if (_last_concluded_rev == _accepted_revision) continue;
                // UNKNOWN result
                _last_concluded_rev = _accepted_revision;
                if (_out_path.empty()) {
                    LOGGER(_logger, V0_CRIT, "IMPCHK_CONFIRM %u %u\n", cidx, 0);
                } else {
                    std::ofstream ofs(_out_path, std::ios_base::app);
                    ofs << cidx << " 0" << std::endl;
                }
            } else if (op.isEndLoad()) {
                _accepted_revision++; // next revision reached
                LOGGER(_logger, V4_VVER, "IMPCHK in rev. %i\n", _accepted_revision);
                // store assumptions for later
                assumptions = std::vector(op.data.endLoad.assumptions, op.data.endLoad.assumptions + op.data.endLoad.nbAssumptions);
            }
        }
    }

    inline void prepareClause(LratOp& op, const u8* sig, u32 cidx) {
        assert(op.isDerivation());
        auto& data = op.data.produce;
        assert(ClauseMetadata::numInts() == 2+4+(_incremental ? 1 : 0));
        auto id = data.id;
        int offset = 0;
        memcpy(_clause.begin+offset, &id, sizeof(u64)); offset += 2; // ID
        memcpy(_clause.begin+offset, sig, SIG_SIZE_BYTES); offset += 4; // Signature
        assert(_accepted_revision >= 0);
        if (_incremental) {
            memcpy(_clause.begin+offset, &cidx, sizeof(u32)); offset += 1; // Revision
        }
        _clause.size = offset+data.nbLits;
        assert(_clause.size <= MAX_CLAUSE_LENGTH);
        memcpy(_clause.begin+offset, data.lits, data.nbLits*sizeof(int)); // Literals
        _clause.lbd = data.glue;
        if (data.nbLits == 1) _clause.lbd = 1;
        else _clause.lbd = std::min(_clause.lbd, _clause.size);
    }
};
