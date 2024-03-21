
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "printer.hpp"
#include "trusted_utils.hpp"
#include "trusted_solving.hpp"

#include "trusted_checker_defs.hpp"

class TrustedCheckerProcess {

private:
    FILE* _input; // named pipe
    FILE* _output; // named pipe
    int _nb_vars; // # variables in formula
    signature _formula_signature; // formula signature

    TrustedSolving* _ts;

    bool _do_logging {true};

    // Buffering.
    signature _buf_sig;
    size_t _bufcap_lits {TRUSTED_CHK_MAX_BUF_SIZE};
    int* _buf_lits;
    int _buflen_lits {0};
    size_t _bufcap_hints {TRUSTED_CHK_MAX_BUF_SIZE};
    unsigned long* _buf_hints;
    unsigned long _buflen_hints {0};

    Printer _printer;

public:
    TrustedCheckerProcess(const char* fifoIn, const char* fifoOut) {
        _input = fopen(fifoIn, "r");
        _output = fopen(fifoOut, "w");
        _buf_lits = (int*) malloc(_bufcap_lits * sizeof(int));
        _buf_hints = (u64*) malloc(_bufcap_hints * sizeof(u64));
    }
    ~TrustedCheckerProcess() {
        free(_buf_hints);
        free(_buf_lits);
        fclose(_output);
        fclose(_input);
    }

    int run() {

        clock_t start = clock();

        u64 nbProduced {0};
        u64 nbImported {0};
        u64 nbDeleted {0};

        bool reportedError {false};

        while (true) {
            int c = TrustedUtils::readChar(_input);
            if (c == TRUSTED_CHK_INIT) {

                _nb_vars = TrustedUtils::readInt(_input);
                _ts = new TrustedSolving(_nb_vars);
                TrustedUtils::readSignature(_formula_signature, _input);
                _printer.printInitDirective(_nb_vars, _formula_signature);
                _ts->init(_formula_signature);
                sayWithFlush(true);

            } else if (c == TRUSTED_CHK_LOAD) {

                const int nbInts = TrustedUtils::readInt(_input);
                TrustedUtils::readInts(_buf_lits, nbInts, _input);
                _printer.printLoadDirective(_buf_lits, nbInts);
                for (size_t i = 0; i < nbInts; i++) _ts->loadLiteral(_buf_lits[i]);
                // NO FEEDBACK

            } else if (c == TRUSTED_CHK_END_LOAD) {

                _printer.printEndLoadingDirective();
                sayWithFlush(_ts->endLoading());

            } else if (c == TRUSTED_CHK_CLS_PRODUCE) {

                // parse
                const unsigned long id = readId();
                const int nbLits = TrustedUtils::readInt(_input);
                readLiterals(nbLits);
                const int nbHints = TrustedUtils::readInt(_input);
                readHints(nbHints);
                bool share = TrustedUtils::readChar(_input);
                //TrustedUtils::doAssert(nbRemaining == 0);
                _printer.printProduceDirective(id, _buf_lits, nbLits, _buf_hints, nbHints);
                // forward to checker
                bool res = _ts->produceClause(id, _buf_lits, nbLits, _buf_hints, nbHints, share ? _buf_sig : nullptr);
                // respond
                say(res);
                if (share) TrustedUtils::writeSignature(_buf_sig, _output);
                nbProduced++;

            } else if (c == TRUSTED_CHK_CLS_IMPORT) {

                // parse
                const unsigned long id = readId();
                const int nbLits = TrustedUtils::readInt(_input);
                readLiterals(nbLits);
                TrustedUtils::readSignature(_buf_sig, _input);
                _printer.printImportDirective(id, _buf_lits, nbLits, _buf_sig);
                // forward to checker
                bool res = _ts->importClause(id, _buf_lits, nbLits, _buf_sig);
                // respond
                say(res);
                nbImported++;

            } else if (c == TRUSTED_CHK_CLS_DELETE) {
                
                // parse
                int nbHints = TrustedUtils::readInt(_input);
                readHints(nbHints);
                //TrustedUtils::doAssert(nbRemaining == 0);
                _printer.printDeleteDirective(_buf_hints, nbHints);
                //printf("PROOF?? d %lu ... (%i)\n", hints[0], nbHints);
                // forward to checker
                bool res = _ts->deleteClauses(_buf_hints, nbHints);
                // respond
                say(res);
                nbDeleted++;

            } else if (c == TRUSTED_CHK_VALIDATE) {

                const bool doLoggingPrev = _do_logging;
                _do_logging = true;
                _printer.printValidateDirective();
                bool res = _ts->validateUnsat(_buf_sig);
                _do_logging = doLoggingPrev;
                say(res);
                TrustedUtils::writeSignature(_buf_sig, _output);
                UNLOCKED_IO(fflush)(_output);

            } else if (c == TRUSTED_CHK_TERMINATE) {

                _printer.printTerminateDirective();
                sayWithFlush(TRUSTED_CHK_RES_ACCEPT);
                break;

            } else {
                log("[ERROR] Invalid directive!");
                break;
            }

            if (MALLOB_UNLIKELY(!_ts->valid())) {
                if (!reportedError) {
                    char msg[1024];
                    snprintf(msg, 1024, "[ERROR] %s", _ts->getErrorMessage());
                    log(msg);
                    reportedError = true;
                }
            }
        }

        float elapsed = (float) (clock() - start) / CLOCKS_PER_SEC;

        char msg[128];
        sprintf(msg, "cpu:%.3f prod:%lu imp:%lu del:%lu", elapsed, nbProduced, nbImported, nbDeleted);
        log(msg);

        return 0;
    }

    void log(const char* msg) {
        if (_do_logging) TrustedUtils::log(msg);
    }

private:
    inline void sayWithFlush(bool ok) {
        say(ok);
        UNLOCKED_IO(fflush)(_output);
    }
    inline void say(bool ok) {
        TrustedUtils::writeChar(ok ? TRUSTED_CHK_RES_ACCEPT : TRUSTED_CHK_RES_ERROR, _output);
    }

    inline u64 readId() {
        return TrustedUtils::readUnsignedLong(_input);
    }

    inline void readLiterals(int nbLits) {
        // parse clause
        _buflen_lits = 0;
        for (int i = 0; i < nbLits; i++) {
            const int lit = TrustedUtils::readInt(_input);
            if (MALLOB_UNLIKELY(_buflen_lits >= _bufcap_lits)) {
                // buffer exceeded - reallocate
                _bufcap_lits *= 2;
                _buf_lits = (int*) realloc(_buf_lits, _bufcap_lits * sizeof(int));
            }
            _buf_lits[_buflen_lits++] = lit;
        }
    }

    inline void readHints(int nbHints) {
        _buflen_hints = 0;
        for (int i = 0; i < nbHints; i++) {
            const u64 hint = TrustedUtils::readUnsignedLong(_input);
            if (MALLOB_UNLIKELY(_buflen_hints >= _bufcap_hints)) {
                // buffer exceeded - reallocate
                _bufcap_hints *= 2;
                _buf_hints = (u64*) realloc(_buf_hints, _bufcap_hints * sizeof(u64));
            }
            _buf_hints[_buflen_hints++] = hint;
        }
    }
};
