
#pragma once

#include <cstdio>
#include <cstdlib>

#include "trusted_utils.hpp"
#include "trusted_solving.hpp"

// Initialize and begin the loading stage.
// IN: #vars (int); 128-bit signature of the formula
// OUT: OK
#define TRUSTED_CHK_INIT 'B'

// Load a chunk of the original problem formula.
// IN: size integer k; sequence of k literals (0 = separator).
// OUT: (none)
#define TRUSTED_CHK_LOAD 'L'

// End the loading stage; verify the signature.
// OUT: OK
#define TRUSTED_CHK_END_LOAD 'E'

// Add the derivation of a new, local clause.
// IN: total size (#ints) k; 64-bit ID; zero-terminated lits; 64-bit hints.
// OUT: OK; 128-bit signature
#define TRUSTED_CHK_CLS_PRODUCE 'a'

// Import a clause from another solver.
// IN: total size (#ints) k; 64-bit ID; zero-terminated lits; 128-bit signature.
// OUT: OK
#define TRUSTED_CHK_CLS_IMPORT 'i'

// Delete a sequence of clauses.
// IN: total size (#ints) k; 64-bit IDs.
// OUT: OK
#define TRUSTED_CHK_CLS_DELETE 'd'

// Confirm that the formula is proven unsatisfiable.
// OUT: OK
#define TRUSTED_CHK_VALIDATE 'V'

#define TRUSTED_CHK_TERMINATE 'T'

#define TRUSTED_CHK_RES_ACCEPT 'A'
#define TRUSTED_CHK_RES_ERROR 'E'

#define TRUSTED_CHK_MAX_BUF_SIZE (1<<16)

void logInCheckerProcess(void* logger, const char* msg);

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
    int* ibuf;
    int ibuflen {0};
    size_t ulbufcap {TRUSTED_CHK_MAX_BUF_SIZE};
    unsigned long* ulbuf;
    unsigned long ulbuflen {0};

public:
    TrustedCheckerProcess(const char* fifoIn, const char* fifoOut) {
        _input = fopen(fifoIn, "r");
        _output = fopen(fifoOut, "w");
        ibuf = (int*) malloc(TRUSTED_CHK_MAX_BUF_SIZE * sizeof(int));
        ulbuf = (u64*) malloc(ulbufcap * sizeof(u64));
    }
    ~TrustedCheckerProcess() {
        free(ulbuf);
        free(ibuf);
        fclose(_output);
        fclose(_input);
    }

    int run() {

        while (true) {
            int c = TrustedUtils::readChar(_input);
            if (c == TRUSTED_CHK_INIT) {

                _nb_vars = TrustedUtils::readInt(_input);
                _ts = new TrustedSolving(logInCheckerProcess, this, _nb_vars);
                readFormulaSignature();
                _ts->init(_buf_sig);
                sayWithFlush(true);

            } else if (c == TRUSTED_CHK_LOAD) {

                const int nbInts = TrustedUtils::readInt(_input);
                TrustedUtils::doAssert(nbInts > 0);
                TrustedUtils::doAssert(nbInts <= TRUSTED_CHK_MAX_BUF_SIZE);
                TrustedUtils::readInts(ibuf, nbInts, _input);
                for (size_t i = 0; i < nbInts; i++) _ts->loadLiteral(ibuf[i]);
                // NO FEEDBACK

            } else if (c == TRUSTED_CHK_END_LOAD) {

                sayWithFlush(_ts->endLoading());

            } else if (c == TRUSTED_CHK_CLS_PRODUCE) {

                // parse
                int nbRemaining = TrustedUtils::readInt(_input);
                unsigned long id;
                int nbLits;
                unsigned long* hints;
                int nbHints;
                readIdAndLiterals(nbRemaining, id, nbLits);
                readHints(nbRemaining, hints, nbHints);
                TrustedUtils::doAssert(nbRemaining == 0);
                // forward to checker
                bool res = _ts->produceClause(id, ibuf, nbLits, hints, nbHints, _buf_sig);
                // respond
                say(res);
                TrustedUtils::writeSignature(_buf_sig, _output);
                UNLOCKED_IO(fflush)(_output);

            } else if (c == TRUSTED_CHK_CLS_IMPORT) {

                // parse
                int nbRemaining = TrustedUtils::readInt(_input);
                unsigned long id;
                int nbLits;
                readIdAndLiterals(nbRemaining, id, nbLits);
                TrustedUtils::readSignature(_buf_sig, _input);
                // forward to checker
                bool res = _ts->importClause(id, ibuf, nbLits, _buf_sig);
                // respond
                sayWithFlush(res);

            } else if (c == TRUSTED_CHK_CLS_DELETE) {
                
                // parse
                int nbRemaining = TrustedUtils::readInt(_input);
                unsigned long* hints;
                int nbHints;
                readHints(nbRemaining, hints, nbHints);
                TrustedUtils::doAssert(nbRemaining == 0);
                //printf("PROOF?? d %lu ... (%i)\n", hints[0], nbHints);
                // forward to checker
                bool res = _ts->deleteClauses(hints, nbHints);
                // respond
                sayWithFlush(res);

            } else if (c == TRUSTED_CHK_VALIDATE) {

                const bool doLoggingPrev = _do_logging;
                _do_logging = true;
                bool res = _ts->validateUnsat(_buf_sig);
                _do_logging = doLoggingPrev;
                sayWithFlush(res);
                TrustedUtils::writeSignature(_buf_sig, _output);
                UNLOCKED_IO(fflush)(_output);

            } else if (c == TRUSTED_CHK_TERMINATE) {

                sayWithFlush(TRUSTED_CHK_RES_ACCEPT);
                break;

            } else {
                log("Invalid directive!");
                TrustedUtils::doAbort(); // invalid directive
            }
        }

        return 0;
    }

    void log(const char* msg) {
        if (_do_logging) printf("%s", msg);
    }

private:
    inline void sayWithFlush(bool ok) {
        say(ok);
        UNLOCKED_IO(fflush)(_output);
    }
    inline void say(bool ok) {
        TrustedUtils::writeChar(ok ? TRUSTED_CHK_RES_ACCEPT : TRUSTED_CHK_RES_ERROR, _output);
    }

    void readIdAndLiterals(int& nbRemaining, unsigned long& id, int& nbLits) {
        // parse ID
        id = TrustedUtils::readUnsignedLong(_input);
        nbRemaining -= 2;
        // parse clause
        ibuflen = 0;
        nbLits = 0;
        while (nbRemaining > 0) {
            const int lit = TrustedUtils::readInt(_input);
            nbRemaining--;
            if (lit == 0) {
                break;
            } else {
                ibuf[ibuflen++] = lit;
                nbLits++;
            }
        }
    }

    void readHints(int& nbRemaining, unsigned long*& hints, int& nbHints) {
        ulbuflen = 0;
        nbHints = 0;
        while (nbRemaining >= 2) {
            const u64 hint = TrustedUtils::readUnsignedLong(_input);
            nbRemaining -= 2;
            if (ulbuflen >= ulbufcap) {
                // buffer exceeded - reallocate
                ulbufcap *= 2;
                ulbuf = (u64*) realloc(ulbuf, ulbufcap * sizeof(u64));
            }
            ulbuf[ulbuflen++] = hint;
            nbHints++;
        }
        hints = ulbuf;
    }

    void readFormulaSignature() {
        TrustedUtils::readSignature(_buf_sig, _input);
        for (size_t i = 0; i < SIG_SIZE_BYTES; i++) {
            _formula_signature[i] = _buf_sig[i];
        }
    }
};