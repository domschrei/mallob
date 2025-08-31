
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
    int _revision {-1};

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

    void init() {
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
    }

    inline void submit(LratOp& op) {
        if (op.isDerivation()) submitProduceClause(op.data.produce);
        else if (op.isImport()) submitImportClause(op.data.import);
        else if (op.isDeletion()) submitDeleteClauses(op.data.remove.hints, op.data.remove.nbHints);
        else if (op.isBeginLoad()) submitBeginLoad(op.data.beginLoad.sig);
        else if (op.isLoad()) submitLoad(op.data.load.lits, op.data.load.nbLits);
        else if (op.isEndLoad()) submitEndLoad(op.data.endLoad.assumptions, op.data.endLoad.nbAssumptions);
        else if (op.isUnsatValidation()) submitValidateUnsat(op.data.concludeUnsat);
        else if (op.isSatValidation()) submitValidateSat(op.data.concludeSat);
        else if (op.isTermination()) submitTerminate();
        _op_queue.pushBlocking(op);
    }

    inline bool accept(LratOp& op, bool& res, u8* sig) {
        bool ok = _op_queue.pollBlocking(op);
        if (!ok) return false;
        if (op.isDerivation()) res = acceptProduceClause(sig, op.data.produce.glue > 0);
        else if (op.isImport()) res = acceptImportClause();
        else if (op.isDeletion()) res = acceptDeleteClauses();
        else if (op.isUnsatValidation()) res = acceptValidateUnsat();
        else if (op.isSatValidation()) res = acceptValidateSat();
        else if (op.isBeginLoad()) res = acceptGeneric("BEGIN LOAD");
        else if (op.isEndLoad()) res = acceptGeneric("END LOAD");
        else if (!op.isLoad()) res = acceptGeneric("Unspecified op");
        if (op.isEndLoad()) _revision++;
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

private:
    void handleError(const std::string& errMsg) {
        if (_error_reported) return;
        LOGGER(_logger, V0_CRIT, "[ERROR] IMPCHK rejected operation: %s\n", errMsg.c_str());
        Terminator::setTerminating();
        _error_reported = true;
    }

    inline void submitBeginLoad(const u8* formulaSignature) {
        writeDirectiveType(TRUSTED_CHK_BEGIN_LOAD);
        TrustedUtils::writeSignature(formulaSignature, _f_directives);
        UNLOCKED_IO(fflush)(_f_directives);
    }

    inline void submitLoad(const int* fData, size_t fSize) {
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

    inline void submitEndLoad(const int* asmpt, int nbAsmpt) {
        if (_buflen_lits > 0) flushLiteralBuffer();
        writeDirectiveType(TRUSTED_CHK_END_LOAD);
        TrustedUtils::writeInt(nbAsmpt, _f_directives);
        TrustedUtils::writeInts(asmpt, nbAsmpt, _f_directives);
        UNLOCKED_IO(fflush)(_f_directives);
    }

    inline void submitProduceClause(const LratOp::LratOpData::LratOpDataProduce& data) {

        writeDirectiveType(TRUSTED_CHK_CLS_PRODUCE);
        TrustedUtils::writeUnsignedLong(data.id, _f_directives);
        TrustedUtils::writeInt(data.nbLits, _f_directives);
        TrustedUtils::writeInts(data.lits, data.nbLits, _f_directives);
        TrustedUtils::writeInt(data.nbHints, _f_directives);
        TrustedUtils::writeUnsignedLongs(data.hints, data.nbHints, _f_directives);
        TrustedUtils::writeChar(data.glue>0 ? 1 : 0, _f_directives);
    }
    inline bool acceptProduceClause(u8* sig, bool readSig) {
        if (!awaitResponse()) {
            handleError("Clause derivation not accepted");
            return false;
        }
        if (readSig) TrustedUtils::readSignature(sig, _f_feedback);
        return true;
    }

    inline void submitImportClause(const LratOp::LratOpData::LratOpDataImport& data) {

        writeDirectiveType(TRUSTED_CHK_CLS_IMPORT);
        TrustedUtils::writeUnsignedLong(data.id, _f_directives);
        TrustedUtils::writeInt(data.nbLits, _f_directives);
        TrustedUtils::writeInts(data.lits, data.nbLits, _f_directives);
        TrustedUtils::writeSignature(data.sig, _f_directives);
        TrustedUtils::writeInt(data.rev, _f_directives);
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

    inline void submitValidateUnsat(const LratOp::LratOpData::LratOpDataConcludeUnsat& data) {
        writeDirectiveType(TRUSTED_CHK_VALIDATE_UNSAT);
        TrustedUtils::writeUnsignedLong(data.id, _f_directives);
        TrustedUtils::writeInt(data.nbFailed, _f_directives);
        TrustedUtils::writeInts(data.failed, data.nbFailed, _f_directives);
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
        LOGGER(_logger, V2_INFO, "IMPCHK reported UNSAT for rev. %i - sig %s\n", _revision, str.c_str());
        return true;
    }

    inline void submitValidateSat(const LratOp::LratOpData::LratOpDataConcludeSat& data) {
        writeDirectiveType(TRUSTED_CHK_VALIDATE_SAT);
        // write model
        TrustedUtils::writeInt(data.modelSize, _f_directives);
        TrustedUtils::writeInts(data.model, data.modelSize, _f_directives);
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
        LOGGER(_logger, V2_INFO, "IMPCHK reported SAT for rev. %i - sig %s\n", _revision, str.c_str());
        return true;
    }

    inline void submitTerminate() {
        writeDirectiveType(TRUSTED_CHK_TERMINATE);
        UNLOCKED_IO(fflush)(_f_directives);
    }
    inline bool acceptGeneric(const std::string& kind) {
        if (!awaitResponse()) {
            handleError(kind + " invalid!");
            return false;
        }
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
