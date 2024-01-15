
#pragma once

#include <vector>
#include <string>

#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/proof/lrat_checker.hpp"
#include "data/job_description.hpp"
#include "util/hashing.hpp"
#include "util/hmac_sha256/sha256.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/tsl/robin_hash.h"

/*
Interface for trusted solving.
*/
class TrustedSolving {

// ******************************** INTERFACE ********************************

public:
    TrustedSolving(Logger& logger, const SolverSetup& setup, int nbVars) :
        _logger(logger), _setup(setup), _checker(nbVars) {}

    // Parse formula, return its signature. Single use and at one process only.
    static std::string signParsedFormula(JobDescription& desc, bool hmac) {return doSignParsedFormula(desc, hmac);}

    void init(const std::string& formulaSignature) {doInit(formulaSignature);}
    void loadLiteral(int lit) {doLoadLiteral(lit);}
    bool endLoading() {return doEndLoading();}

    bool produceClause(unsigned long id, const int* literals, int nbLiterals,
        const unsigned long* hints, int nbHints,
        uint8_t* outSignatureOrNull, int& inOutSigSize) {
        return doProduceClause(id, literals, nbLiterals, hints, nbHints, outSignatureOrNull, inOutSigSize);
    }
    bool importClause(unsigned long id, const int* literals, int nbLiterals,
        const uint8_t* signatureData, int signatureSize) {
        return doImportClause(id, literals, nbLiterals, signatureData, signatureSize);
    }
    bool deleteClauses(const unsigned long* ids, int nbIds) {return doDeleteClauses(ids, nbIds);}

    bool validateUnsat() {return doValidateUnsat();}

// **************************** END OF INTERFACE *****************************



private:
    Logger& _logger;
    const SolverSetup _setup;

    static unsigned long _orig_key; // secret
    static bool _parsed_formula;

    unsigned long _modi_key; // secret
    std::string _formula_signature;
    LratChecker _checker;

#define MAX_SIG_SIZE_BYTES 16

    inline void doInit(const std::string& formulaSignature) {
        // Store formula signature to validate later after loading
        _formula_signature = formulaSignature;
        // Compute updated key via SHA256 based on the original key and the formula's signature
        auto modified = Sha256Builder()
            .update((uint8_t*) &_orig_key, sizeof(unsigned long))
            .update((uint8_t*) _formula_signature.data(), _formula_signature.size())
            .get();
        memcpy(&_modi_key, modified.data(), sizeof(unsigned long));
    }

    inline void doLoadLiteral(int lit) {
        _checker.loadLiteral(lit);
    }

    inline bool doEndLoading() {
        std::vector<uint8_t> sha256;
        bool ok = _checker.endLoading(&sha256);
        if (!ok) abortWithCheckerError();
        // Compute HMAC signature based on the hash
        int sigSize = 256 / 8;
		std::vector<uint8_t> sigData(sigSize);
        computeSignature(_setup.hmacSignatures, sha256.data(), sha256.size(), sigData.data(), sigSize, _orig_key);
		const auto sigStr = dataToHexStr(sigData.data(), sigSize);
		// Check against provided signature
        ok = sigStr == _formula_signature; 
		if (!ok) {
			LOGGER(_logger, V0_CRIT, "[ERROR] TS - formula signature does not match: %s vs. %s\n", sigStr.c_str(), _setup.sigFormula.c_str());
			abort();
		}
        return ok;
    }

    inline bool doProduceClause(unsigned long id, const int* literals, int nbLiterals,
        const unsigned long* hints, int nbHints,
        uint8_t* outSignatureOrNull, int& inOutSigSize) {
        
        // forward clause to checker
        bool ok = _checker.addClause(id, literals, nbLiterals, hints, nbHints);
        if (!ok) abortWithCheckerError();
        // compute signature if desired
        if (outSignatureOrNull) {
            computeClauseSignature(_setup.hmacSignatures, id, literals, nbLiterals, outSignatureOrNull, inOutSigSize, _modi_key);
        }
        return ok;
    }

    inline bool doImportClause(unsigned long id, const int* literals, int nbLiterals,
        const uint8_t* signatureData, int signatureSize) {
        
        // verify signature
        int computedSigSize = MAX_SIG_SIZE_BYTES;
        uint8_t computedSignature[computedSigSize];
        computeClauseSignature(_setup.hmacSignatures, id, literals, nbLiterals, computedSignature, computedSigSize, _modi_key);
        if (computedSigSize != signatureSize) {
            LOGGER(_logger, V0_CRIT, "[ERROR] TS - supplied clause signature has wrong size\n");
            abort();
        }
        for (int i = 0; i < signatureSize; i++) if (computedSignature[i] != signatureData[i]) {
            LOGGER(_logger, V0_CRIT, "[ERROR] TS - clause signature does not match: %s vs %s\n",
                dataToHexStr(computedSignature, computedSigSize).c_str(), dataToHexStr(signatureData, signatureSize).c_str());
            abort();
        }

        // signature verified - forward clause to checker as an axiom
        bool ok = _checker.addAxiomaticClause(id, literals, nbLiterals);
        if (!ok) abortWithCheckerError();
        return ok;
    }

    inline bool doDeleteClauses(const unsigned long* ids, int nbIds) {
        bool ok = _checker.deleteClause(ids, nbIds);
        if (!ok) abortWithCheckerError();
        return ok;
    }

    inline bool doValidateUnsat() {
        bool ok = _checker.validateUnsat();
        if (!ok) abortWithCheckerError();
        uint8_t yes = 1;
        int sigSize {16};
        std::vector<uint8_t> signature(sigSize);
        computeSignature(_setup.hmacSignatures, &yes, 1, signature.data(), sigSize, _modi_key);
        std::string sigStr = dataToHexStr(signature.data(), sigSize);
        LOGGER(_logger, V2_INFO, "TS - UNSAT of formula %s checked on-the-fly; confirmation: %s\n",
            _formula_signature.c_str(), sigStr.c_str());
        return ok;
    }

    inline void abortWithCheckerError() {
        LOGGER(_logger, V0_CRIT, "[ERROR] TS - LRAT checker error: %s\n", _checker.getErrorMessage().c_str());
        abort();
    }

    static inline std::string doSignParsedFormula(JobDescription& desc, bool hmac) {
        if (_parsed_formula) {
            LOG(V0_CRIT, "[ERROR] TS - attempt to sign multiple formulas\n");
            abort();
        }
        // Sign the read data with HMAC-SHA256
		Sha256Builder sigBuilder;
		sigBuilder.update(
			(uint8_t*) desc.getFormulaPayload(desc.getRevision()),
			sizeof(int) * desc.getNumFormulaLiterals()
		);
		std::vector<uint8_t> sha256 = sigBuilder.get();
        int sigSize = 16;
		std::vector<uint8_t> sigData(sigSize);
        computeSignature(hmac, sha256.data(), sha256.size(), sigData.data(), sigSize, _orig_key);
		// Put the signature into the description as a config entry
		const auto sigStr = dataToHexStr(sigData.data(), sigSize);
		LOG(V2_INFO, "TS - Signed parsed formula #%i with signature %s\n", desc.getId(), sigStr.c_str());
        _parsed_formula = true;
        return sigStr;
    }

    static inline void computeClauseSignature(bool hmac, uint64_t id, const int* lits, int nbLits, uint8_t* out, int& inOutSize, unsigned long key) {

        const int MAX_CLAUSE_LENGTH = 1<<14;
        int data[MAX_CLAUSE_LENGTH];
        if (nbLits+2 >= MAX_CLAUSE_LENGTH) abort();
        memcpy(data, lits, sizeof(int) * nbLits);
        memcpy(data + nbLits, (uint8_t*) &id, sizeof(unsigned long));

        computeSignature(hmac, (uint8_t*) data, sizeof(int) * (nbLits+2), out, inOutSize, key);
    }

    static inline void computeSignature(bool hmac, const uint8_t* data, int size, uint8_t* out, int& inOutSize, unsigned long key) {
        if (hmac) {
            computeHmacSignature(data, size, out, inOutSize, key);
        } else {
            computeSimpleSignature(data, size, out, inOutSize, key);
        }
    }

    static inline void computeHmacSignature(const uint8_t* data, int size, uint8_t* out, int& inOutSize, unsigned long key) {
        if (inOutSize < 16) abort();
        inOutSize = HMAC::sign_data_128bit(data, size, key, out);
        return;
    }

    static inline void computeSimpleSignature(const uint8_t* data, int size, uint8_t* out, int& inOutSize, unsigned long key) {
        if (inOutSize < sizeof(unsigned long)) abort();
        memcpy(out, &key, sizeof(unsigned long));
        for (int i = 0; i < size; i++) hash_combine((unsigned long&) *out, data[i]);
        inOutSize = sizeof(unsigned long);
    }

    static inline std::string dataToHexStr(const uint8_t* data, unsigned long size) {
        std::stringstream stream;
		for (int i = 0; i < size; i++) {
			stream << std::hex << std::setfill('0') << std::setw(2) << (int) data[i];
		}
		return stream.str();
    }

#undef MAX_SIG_SIZE_BYTES
};
