
#pragma once

#include "lrat_checker.hpp"
#include "siphash/siphash.hpp"
#include "trusted_utils.hpp"
#include "secret.hpp"

/*
Interface for trusted solving.
*/
class TrustedSolving {

private:
    bool _parsed_formula {false};
    signature _formula_signature;
    LratChecker _checker;
    SipHash _siphash;

    bool _valid {true};
    char _errmsg[512] = {0};

public:
    TrustedSolving(int nbVars) :
        _checker(nbVars, Secret::SECRET_KEY),
        _siphash(Secret::SECRET_KEY) {}

    void init(const u8* formulaSignature) {
        // Store formula signature to validate later after loading
        TrustedUtils::copyBytes(_formula_signature, formulaSignature, SIG_SIZE_BYTES);
    }

    inline void loadLiteral(int lit) {
        _valid &= _checker.loadLiteral(lit);
    }

    inline bool endLoading() {
        u8* sigFromChecker;
        _valid = _valid && _checker.endLoading(sigFromChecker);
        if (!_valid) return false;
		// Check against provided signature
        _valid = TrustedUtils::equalSignatures(sigFromChecker, _formula_signature);
        if (!_valid) snprintf(_errmsg, 512, "Formula signature check failed");
		return _valid;
    }

    inline bool produceClause(unsigned long id, const int* literals, int nbLiterals,
        const unsigned long* hints, int nbHints, u8* outSignatureOrNull) {
        
        // forward clause to checker
        _valid &= _checker.addClause(id, literals, nbLiterals, hints, nbHints);
        if (!_valid) return false;
        // compute signature if desired
        if (outSignatureOrNull) {
            computeClauseSignature(id, literals, nbLiterals, outSignatureOrNull);
        }
        return true;
    }

    inline bool importClause(unsigned long id, const int* literals, int nbLiterals,
        const u8* signatureData) {
        
        // verify signature
        signature computedSignature;
        computeClauseSignature(id, literals, nbLiterals, computedSignature);
        if (!TrustedUtils::equalSignatures(signatureData, computedSignature)) {
            _valid = false;
            snprintf(_errmsg, 512, "Signature check of clause %lu failed", id);
            return false;
        }

        // signature verified - forward clause to checker as an axiom
        _valid &= _checker.addAxiomaticClause(id, literals, nbLiterals);
        return _valid;
    }

    inline bool deleteClauses(const unsigned long* ids, int nbIds) {
        return _checker.deleteClause(ids, nbIds);
    }

    inline bool validateUnsat(u8* outSignature) {
        _valid &= _checker.validateUnsat();
        if (!_valid) return false;
        if (outSignature) {
            const u8 UNSAT = 20;
            auto sig = _siphash.reset().update(_formula_signature, SIG_SIZE_BYTES).update(&UNSAT, 1).digest();
            TrustedUtils::copyBytes(outSignature, sig, SIG_SIZE_BYTES);
        }
        return true;
    }

    inline void computeClauseSignature(u64 id, const int* lits, int nbLits, u8* out) {
        const u8* hashOut = _siphash.reset()
            .update((u8*) &id, sizeof(u64))
            .update((u8*) lits, nbLits*sizeof(int))
            .update(_formula_signature, SIG_SIZE_BYTES)
            .digest();
        TrustedUtils::copyBytes(out, hashOut, SIG_SIZE_BYTES);
    }

    inline void computeSignature(const u8* data, int size, u8* out) {
        u8* sipout = _siphash.reset()
            .update(data, size)
            .digest();
        TrustedUtils::copyBytes(out, sipout, SIG_SIZE_BYTES);
    }

    inline bool valid() const {return _valid;}

    const char* getErrorMessage() {
        if (_errmsg[0] != '\0') return _errmsg;
        return _checker.getErrorMessage();
    }
};
