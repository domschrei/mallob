
#pragma once

#include "secret.hpp"
#include "lrat_checker.hpp"
#include "siphash/siphash.hpp"
#include <cstdlib>

/*
Interface for trusted solving.
*/
class TrustedSolving {

public:
    TrustedSolving(void (*logFunction)(void*, const char*), void* logger, int nbVars) :
        _log_function(logFunction), _logger(logger),
        _checker(nbVars, Secret::SECRET_KEY),
        _siphash(Secret::SECRET_KEY) {}

#define SIG_SIZE_BYTES 16

private:

    void (*_log_function)(void*, const char*);
    void* _logger;

    bool _parsed_formula {false};
    uint8_t _formula_signature[SIG_SIZE_BYTES];
    LratChecker _checker;
    SipHash _siphash;


public:
    void signParsedFormula(const int* lits, unsigned long nbLits, uint8_t* outSig, int& inOutSigSize) {
        if (_parsed_formula) {
            _log_function(_logger, "[ERROR] TS - attempt to sign multiple formulas");
            TrustedUtils::doAbort();
        }
        // Sign the read data
        if (inOutSigSize < SIG_SIZE_BYTES) {
            TrustedUtils::doAbort();
        }
        inOutSigSize = SIG_SIZE_BYTES;
        uint8_t* out = _siphash.reset()
            .update((uint8_t*) lits, sizeof(int) * nbLits)
            .digest();
        copyBytes(outSig, out, SIG_SIZE_BYTES);
        _parsed_formula = true;
    }

    void init(const uint8_t* formulaSignature) {
        // Store formula signature to validate later after loading
        copyBytes(_formula_signature, formulaSignature, SIG_SIZE_BYTES);
    }

    inline void loadLiteral(int lit) {
        bool ok = _checker.loadLiteral(lit);
        if (!ok) abortWithCheckerError();
    }

    inline bool endLoading() {
        uint8_t* sigFromChecker;
        bool ok = _checker.endLoading(sigFromChecker);
        if (!ok) abortWithCheckerError();
		// Check against provided signature
        ok = equalSignatures(sigFromChecker, _formula_signature); 
		if (!ok) {
			_log_function(_logger, "[ERROR] TS - formula signature does not match");
			TrustedUtils::doAbort();
		}
        return ok;
    }

    inline bool produceClause(unsigned long id, const int* literals, int nbLiterals,
        const unsigned long* hints, int nbHints,
        uint8_t* outSignatureOrNull, int& inOutSigSize) {
        
        // forward clause to checker
        bool ok = _checker.addClause(id, literals, nbLiterals, hints, nbHints);
        if (!ok) abortWithCheckerError();
        // compute signature if desired
        if (outSignatureOrNull) {
            computeClauseSignature(id, literals, nbLiterals, outSignatureOrNull, inOutSigSize);
        }
        return ok;
    }

    inline bool importClause(unsigned long id, const int* literals, int nbLiterals,
        const uint8_t* signatureData, int signatureSize) {
        
        // verify signature
        int computedSigSize = SIG_SIZE_BYTES;
        uint8_t computedSignature[computedSigSize];
        computeClauseSignature(id, literals, nbLiterals, computedSignature, computedSigSize);
        if (computedSigSize != signatureSize) {
            _log_function(_logger, "[ERROR] TS - supplied clause signature has wrong size");
            TrustedUtils::doAbort();
        }
        for (int i = 0; i < signatureSize; i++) if (computedSignature[i] != signatureData[i]) {
            _log_function(_logger, "[ERROR] TS - clause signature does not match");
            TrustedUtils::doAbort();
        }

        // signature verified - forward clause to checker as an axiom
        bool ok = _checker.addAxiomaticClause(id, literals, nbLiterals);
        if (!ok) abortWithCheckerError();
        return ok;
    }

    inline bool deleteClauses(const unsigned long* ids, int nbIds) {
        bool ok = _checker.deleteClause(ids, nbIds);
        if (!ok) abortWithCheckerError();
        return ok;
    }

    inline bool validateUnsat(uint8_t* outSignature, int& inOutSigSize) {
        bool ok = _checker.validateUnsat();
        if (!ok) abortWithCheckerError();
        _log_function(_logger, "TS - UNSAT VALIDATED");
        if (outSignature) {
            u8 UNSAT = 20;
            if (inOutSigSize < SIG_SIZE_BYTES) abort();
            auto sig = _siphash.reset().update(_formula_signature, SIG_SIZE_BYTES).update(&UNSAT, 1).digest();
            for (size_t i = 0; i < 16; i++) outSignature[i] = sig[i];
            inOutSigSize = SIG_SIZE_BYTES;
        }
        return ok;
    }

    inline void abortWithCheckerError(const char* errmsgOrNull = nullptr) {
        _log_function(_logger, "[ERROR] TS - LRAT checker error:");
        if (errmsgOrNull) {
            _log_function(_logger, errmsgOrNull);
        } else {
            _log_function(_logger, _checker.getErrorMessage());
        }
        TrustedUtils::doAbort();
    }

    inline void computeClauseSignature(uint64_t id, const int* lits, int nbLits, uint8_t* out, int& inOutSize) {
        if (inOutSize < SIG_SIZE_BYTES) TrustedUtils::doAbort();
        inOutSize = SIG_SIZE_BYTES;
        const uint8_t* hashOut = _siphash.reset()
            .update((uint8_t*) &id, sizeof(uint64_t))
            .update((uint8_t*) lits, nbLits*sizeof(int))
            .update(Secret::SECRET_KEY, SIG_SIZE_BYTES)
            .digest();
        copyBytes(out, hashOut, inOutSize);
    }

    inline void computeSignature(const uint8_t* data, int size, uint8_t* out, int& inOutSize) {
        if (inOutSize < SIG_SIZE_BYTES) TrustedUtils::doAbort();
        inOutSize = SIG_SIZE_BYTES;
        uint8_t* sipout = _siphash.reset()
            .update(data, size)
            .digest();
        copyBytes(out, sipout, SIG_SIZE_BYTES);
    }

    void copyBytes(uint8_t* to, const uint8_t* from, size_t nbBytes) {
        for (size_t i = 0; i < nbBytes; i++) {
            to[i] = from[i];
        }
    }

    bool equalSignatures(const uint8_t* left, const uint8_t* right) {
        for (size_t i = 0; i < SIG_SIZE_BYTES; i++) {
            if (left[i] != right[i]) return false;
        }
        return true;
    }

#undef SIG_SIZE_BYTES
};
