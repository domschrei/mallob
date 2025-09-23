
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "app/sat/proof/impcheck.hpp"
#include "app/sat/proof/lrat_op.hpp"
#include "trusted/trusted_utils.hpp"
#include "trusted/trusted_checker_defs.hpp"
#include "util/hashing.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "util/sys/subprocess.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/threading.hpp"
#include "util/sys/tmpdir.hpp"

class TrustedCheckerProcessAdapter {

private:
    int _base_seed;
    Logger& _logger;
    std::string _path_directives;
    std::string _path_feedback;
    FILE* _f_directives {nullptr};
    FILE* _f_feedback;
    Subprocess* _subproc;
    pid_t _child_pid {-1};

    const int _solver_id;
    const int _nb_vars;
    const bool _check_model;

    // buffering
    int _buf_lits[TRUSTED_CHK_MAX_BUF_SIZE];
    int _buflen_lits {0};
    SPSCBlockingRingbuffer<LratOp> _op_queue;

    bool _error_reported {false};
    bool _model_set {false};
    std::vector<int> _model;
    Mutex _mtx_model;

public:
    TrustedCheckerProcessAdapter(Logger& logger, int baseSeed, int solverId, int nbVars, bool checkModel) :
            _base_seed(baseSeed), _logger(logger), _solver_id(solverId), _nb_vars(nbVars),
            _check_model(checkModel), _op_queue(1<<14) {}

    ~TrustedCheckerProcessAdapter() {
        if (!_f_directives) return;
        if (_child_pid != -1) terminate();
        fclose(_f_feedback);
        fclose(_f_directives);
        FileUtils::rm(_path_feedback);
        FileUtils::rm(_path_directives);
        delete _subproc;
    }

    void init(const u8* formulaSignature) {

        auto basePath = TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallob." + std::to_string(Proc::getPid()) + ".slv"
            + std::to_string(_solver_id) + ".ts.";
        _path_directives = basePath + "directives";
        _path_feedback = basePath + "feedback";
        int res;
        res = mkfifo(_path_directives.c_str(), 0666);
        if (res != 0) abort();
        res = mkfifo(_path_feedback.c_str(), 0666);
        if (res != 0) abort();

        Parameters params;
        params.fifoDirectives.set(_path_directives);
        params.fifoFeedback.set(_path_feedback);
        std::string moreArgs = "-lenient";
        if (_check_model) moreArgs += " -check-model";

        unsigned long keySeed = ImpCheck::getKeySeed(_base_seed);
        moreArgs += " -key-seed=" + std::to_string(keySeed);

        _subproc = new Subprocess(params, "impcheck_check", moreArgs);
        _child_pid = _subproc->start();

        _f_directives = fopen(_path_directives.c_str(), "w");
        _f_feedback = fopen(_path_feedback.c_str(), "r");

        writeDirectiveType(TRUSTED_CHK_INIT);
        TrustedUtils::writeInt(_nb_vars, _f_directives);
        TrustedUtils::writeSignature(formulaSignature, _f_directives);
        UNLOCKED_IO(fflush)(_f_directives);
        if (!awaitResponse()) handleError("INIT failed");
    }

    inline void load(const int* fData, size_t fSize) {
        assert(_buflen_lits == 0);
        size_t offset = 0;
        while (offset < fSize) {
            const auto nbInts = std::min(fSize-offset, (size_t)TRUSTED_CHK_MAX_BUF_SIZE);
            memcpy(_buf_lits, fData+offset, nbInts*sizeof(int));
            _buflen_lits = nbInts;
            flushLiteralBuffer();
            offset += nbInts;
        }
        assert(offset == fSize);
    }

    inline bool endLoading() {

        if (_buflen_lits > 0) flushLiteralBuffer();
        writeDirectiveType(TRUSTED_CHK_END_LOAD);
        UNLOCKED_IO(fflush)(_f_directives);
        if (!awaitResponse()) {
            handleError("Loaded formula not accepted");
            return false;
        }
        return true;
    }

    inline void submit(LratOp& op) {
        auto type = op.getType();
        if (type == LratOp::DERIVATION) submitProduceClause(op.getId(), op.getLits(), op.getNbLits(), op.getHints(), op.getNbHints(), op.getGlue() > 0);
        else if (type == LratOp::IMPORT) submitImportClause(op.getId(), op.getLits(), op.getNbLits(), op.getSignature());
        else if (type == LratOp::DELETION) submitDeleteClauses(op.getHints(), op.getNbHints());
        else if (type == LratOp::VALIDATION_UNSAT) submitValidateUnsat();
        else if (type == LratOp::VALIDATION_SAT) submitValidateSat();
        else if (type == LratOp::TERMINATION) submitTerminate();
        _op_queue.pushBlocking(op);
    }

    inline bool accept(LratOp& op, bool& res, u8* sig) {
        bool ok = _op_queue.pollBlocking(op);
        if (!ok) return false;
        auto type = op.getType();
        if (type == LratOp::DERIVATION) res = acceptProduceClause(sig, op.getGlue() > 0);
        else if (type == LratOp::IMPORT) res = acceptImportClause();
        else if (type == LratOp::DELETION) res = acceptDeleteClauses();
        else if (type == LratOp::VALIDATION_UNSAT) res = acceptValidateUnsat();
        else if (type == LratOp::VALIDATION_SAT) res = acceptValidateSat();
        else if (type == LratOp::TERMINATION) res = acceptTerminate();
        return true;
    }

    void terminate() {
        _op_queue.markExhausted();
        if (_child_pid == -1) return;
        while (isSubprocessRunning()) usleep(1000);
        LOGGER(_logger, V4_VVER, "Checker process %i exited\n", _child_pid);
        _child_pid = -1;
    }

    bool isSubprocessRunning() {
        return _child_pid != -1 && !Process::didChildExit(_child_pid);
    }

    bool setModel(const int* modelData, size_t modelSize) {
        auto lock = _mtx_model.getLock();
        if (_model_set) return false;
        _model = std::vector(modelData, modelData + modelSize);
        _model_set = true;
        return true;
    }

private:
    void handleError(const std::string& errMsg) {
        if (_error_reported) return;
        LOGGER(_logger, V0_CRIT, "[ERROR] IMPCHK rejected operation: %s\n", errMsg.c_str());
        Terminator::setTerminating();
        _error_reported = true;
    }

    inline void submitProduceClause(unsigned long id, const int* literals, int nbLiterals,
        const unsigned long* hints, int nbHints, bool share) {

        writeDirectiveType(TRUSTED_CHK_CLS_PRODUCE);
        TrustedUtils::writeUnsignedLong(id, _f_directives);
        TrustedUtils::writeInt(nbLiterals, _f_directives);
        TrustedUtils::writeInts(literals, nbLiterals, _f_directives);
        TrustedUtils::writeInt(nbHints, _f_directives);
        TrustedUtils::writeUnsignedLongs(hints, nbHints, _f_directives);
        TrustedUtils::writeChar(share ? 1 : 0, _f_directives);
    }
    inline bool acceptProduceClause(u8* sig, bool readSig) {
        if (!awaitResponse()) {
            handleError("Clause derivation not accepted");
            return false;
        }
        if (readSig) TrustedUtils::readSignature(sig, _f_feedback);
        return true;
    }

    inline void submitImportClause(unsigned long id, const int* literals, int nbLiterals,
        const uint8_t* signatureData) {

        writeDirectiveType(TRUSTED_CHK_CLS_IMPORT);
        TrustedUtils::writeUnsignedLong(id, _f_directives);
        TrustedUtils::writeInt(nbLiterals, _f_directives);
        TrustedUtils::writeInts(literals, nbLiterals, _f_directives);
        TrustedUtils::writeSignature(signatureData, _f_directives);
    }
    inline bool acceptImportClause() {
        if (!awaitResponse()) {
            handleError("Imported clause not accepted");
            return false;
        }
        return true;
    }

    inline void submitDeleteClauses(const unsigned long* ids, int nbIds) {

        writeDirectiveType(TRUSTED_CHK_CLS_DELETE);
        TrustedUtils::writeInt(nbIds, _f_directives);
        TrustedUtils::writeUnsignedLongs(ids, nbIds, _f_directives);
    }
    inline bool acceptDeleteClauses() {
        if (!awaitResponse()) {
            handleError("Error in deletion of clauses");
            return false;
        }
        return true;
    }

    inline void submitValidateUnsat() {
        writeDirectiveType(TRUSTED_CHK_VALIDATE_UNSAT);
        UNLOCKED_IO(fflush)(_f_directives);
    }
    inline bool acceptValidateUnsat() {
        if (!awaitResponse()) {
            handleError("UNSAT NOT valid");
            return false;
        }
        signature sig;
        TrustedUtils::readSignature(sig, _f_feedback);
        auto str = Logger::dataToHexStr(sig, SIG_SIZE_BYTES);
        LOGGER(_logger, V2_INFO, "IMPCHK reported UNSAT - sig %s\n", str.c_str());
        return true;
    }

    inline void submitValidateSat() {
        writeDirectiveType(TRUSTED_CHK_VALIDATE_SAT);
        // write model
        TrustedUtils::writeInt(_model.size(), _f_directives);
        TrustedUtils::writeInts(_model.data(), _model.size(), _f_directives);
        UNLOCKED_IO(fflush)(_f_directives);
    }
    inline bool acceptValidateSat() {
        if (!awaitResponse()) {
            handleError("SAT NOT valid");
            return false;
        }
        signature sig;
        TrustedUtils::readSignature(sig, _f_feedback);
        auto str = Logger::dataToHexStr(sig, SIG_SIZE_BYTES);
        LOGGER(_logger, V2_INFO, "IMPCHK reported SAT - sig %s\n", str.c_str());
        return true;
    }

    inline void submitTerminate() {
        writeDirectiveType(TRUSTED_CHK_TERMINATE);
        UNLOCKED_IO(fflush)(_f_directives);
    }
    inline bool acceptTerminate() {
        awaitResponse();
        return true;
    }

    void flushLiteralBuffer() {
        writeDirectiveType(TRUSTED_CHK_LOAD);
        TrustedUtils::writeInt(_buflen_lits, _f_directives);
        TrustedUtils::writeInts(_buf_lits, _buflen_lits, _f_directives);
        _buflen_lits = 0;
    }

    void writeDirectiveType(char type) {
        TrustedUtils::writeChar(type, _f_directives);
    }
    bool awaitResponse() {
        int res = TrustedUtils::readChar(_f_feedback);
        return (char)res == TRUSTED_CHK_RES_ACCEPT;
    }
};
