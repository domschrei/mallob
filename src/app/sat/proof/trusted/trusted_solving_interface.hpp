
#pragma once

#include "trusted_utils.hpp"

struct TrustedSolvingInterface {

    virtual void init(const u8* formulaSignature) = 0;
    virtual void loadLiteral(int lit) = 0;
    virtual bool endLoading() = 0;

    virtual bool produceClause(u64 id, const int* literals, int nbLiterals,
        const u64* hints, int nbHints,
        u8* outSignatureOrNull, int& inOutSigSize) = 0;
    virtual bool importClause(u64 id, const int* literals, int nbLiterals,
        const u8* signatureData, int signatureSize) = 0;
    virtual bool deleteClauses(const u64* ids, int nbIds) = 0;

    virtual bool validateUnsat(u8* outSignature, int& inOutSigSize) = 0;
};
