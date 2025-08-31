
#pragma once

#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "app/sat/proof/trusted/trusted_checker_defs.hpp"
#include "util/logger.hpp"
#include "util/assert.hpp"
#include "trusted/trusted_utils.hpp"

struct LratOp {
    char type = '\0';
    union LratOpData {
        struct LratOpDataBeginLoad {
            u8 sig[SIG_SIZE_BYTES];
        } beginLoad;
        struct LratOpDataLoad {
            int* lits;
            int nbLits;
        } load;
        struct LratOpDataEndLoad {
            int* assumptions;
            int nbAssumptions;
        } endLoad;
        struct LratOpDataProduce {
            u64 id;
            int* lits;
            int nbLits;
            u64* hints;
            int nbHints;
            int glue;
        } produce;
        struct LratOpDataImport {
            u64 id;
            int* lits;
            int nbLits;
            u8 sig[SIG_SIZE_BYTES];
            int rev;
        } import;
        struct LratOpDataRemove {
            u64* hints;
            int nbHints;
        } remove;
        struct LratOpDataConcludeSat {
            int* model;
            int modelSize;
        } concludeSat;
        struct LratOpDataConcludeUnsat {
            u64 id;
            int* failed;
            int nbFailed;
        } concludeUnsat;
        struct LratOpDataTerminate {} terminate;
    } data;

    // Derivation / production / addition
    LratOp(u64 id, const int* lits, int nbLits, const u64* hints, int nbHints, int glue) {
        type = TRUSTED_CHK_CLS_PRODUCE;
        data.produce.id = id;
        data.produce.nbLits = nbLits;
        data.produce.nbHints = nbHints;
        data.produce.glue = glue;

        u8* litsAndHints = (u8*) malloc(sizeof(int) * nbLits + sizeof(u64) * nbHints);
        memcpy(litsAndHints, lits, sizeof(int) * nbLits);
        memcpy(litsAndHints + sizeof(int) * nbLits, hints, sizeof(u64) * nbHints);
        data.produce.lits = (int*) litsAndHints;
        data.produce.hints = (u64*) (litsAndHints + sizeof(int) * nbLits);
    }
    // Import
    LratOp(u64 id, const int* lits, int nbLits, const u8* sig, int rev) {
        type = TRUSTED_CHK_CLS_IMPORT;
        data.import.id = id;
        data.import.nbLits = nbLits;
        data.import.rev = rev;
        memcpy(data.import.sig, sig, SIG_SIZE_BYTES);

        data.import.lits = (int*) malloc(sizeof(int) * nbLits);
        memcpy(data.import.lits, lits, sizeof(int) * nbLits);
    }
    // Deletion
    LratOp(const u64* hints, int nbHints) {
        type = TRUSTED_CHK_CLS_DELETE;
        data.remove.nbHints = nbHints;

        data.remove.hints = (u64*) malloc(sizeof(u64) * nbHints);
        memcpy(data.remove.hints, hints, sizeof(u64) * nbHints);
    }
    // Begin load
    LratOp(const u8* sig) {
        type = TRUSTED_CHK_BEGIN_LOAD;
        memcpy(data.beginLoad.sig, sig, SIG_SIZE_BYTES);
    }
    // Load and end-load
    LratOp(char c, const int* lits, int nbLits) {
        type = c;
        if (type == TRUSTED_CHK_LOAD) {
            data.load.nbLits = nbLits;
            data.load.lits = (int*) malloc(sizeof(int) * nbLits);
            memcpy(data.load.lits, lits, sizeof(int) * nbLits);
        } else if (type == TRUSTED_CHK_END_LOAD) {
            data.endLoad.nbAssumptions = nbLits;
            data.endLoad.assumptions = (int*) malloc(sizeof(int) * nbLits);
            memcpy(data.endLoad.assumptions, lits, sizeof(int) * nbLits);
        }
    }
    // Terminate
    LratOp(char x) {
        type = x;
        assert(type == TRUSTED_CHK_TERMINATE);
    }
    // SAT Validation (10)
    LratOp(int* model, int modelSize) {
        type = TRUSTED_CHK_VALIDATE_SAT;
        data.concludeSat.modelSize = modelSize;

        data.concludeSat.model = (int*) malloc(sizeof(int) * modelSize);
        memcpy(data.concludeSat.model, model, sizeof(int) * modelSize);
    }
    // UNSAT Validation (20)
    LratOp(u64 id, const int* failed, int nbFailed) {
        type = TRUSTED_CHK_VALIDATE_UNSAT;
        data.concludeUnsat.id = id;
        data.concludeUnsat.nbFailed = nbFailed;

        data.concludeUnsat.failed = (int*) malloc(sizeof(int) * nbFailed);
        memcpy(data.concludeUnsat.failed, failed, sizeof(int) * nbFailed);
    }
    LratOp() {}
    LratOp(LratOp&& moved) {
        *this = std::move(moved);
    }
    LratOp& operator=(LratOp&& moved) {
        type = moved.type;
        data = std::move(moved.data);
        moved.type = '\0';
        return *this;
    }
    LratOp(const LratOp&) = delete;
    LratOp& operator=(const LratOp&) = delete;

    void sortLiterals() {
        assert(isDerivation());
        auto lits = data.produce.lits;
        std::sort(lits, lits+data.produce.nbLits);
    }

    bool isBeginLoad() const {return type == TRUSTED_CHK_BEGIN_LOAD;}
    bool isLoad() const {return type == TRUSTED_CHK_LOAD;}
    bool isEndLoad() const {return type == TRUSTED_CHK_END_LOAD;}
    bool isDerivation() const {return type == TRUSTED_CHK_CLS_PRODUCE;}
    bool isImport() const {return type == TRUSTED_CHK_CLS_IMPORT;}
    bool isDeletion() const {return type == TRUSTED_CHK_CLS_DELETE;}
    bool isUnsatValidation() const {return type == TRUSTED_CHK_VALIDATE_UNSAT;}
    bool isSatValidation() const {return type == TRUSTED_CHK_VALIDATE_SAT;}
    bool isTermination() const {return type == TRUSTED_CHK_TERMINATE;}

    std::string toStr() const {
        std::string out;
        if (isDerivation()) {
            auto& prod = data.produce;
            out += "a " + std::to_string(prod.id) + " ";
            for (int i = 0; i < prod.nbLits; i++) out += std::to_string(prod.lits[i]) + " ";
            out += "0 ";
            for (int i = 0; i < prod.nbHints; i++) out += std::to_string(prod.hints[i]) + " ";
            out += "0";
        } else if (isImport()) {
            auto& imp = data.import;
            out += "i " + std::to_string(imp.id) + " ";
            for (int i = 0; i < imp.nbLits; i++) out += std::to_string(imp.lits[i]) + " ";
            out += "0 ";
            out += Logger::dataToHexStr(imp.sig, SIG_SIZE_BYTES) + " ";
            out += "0";
        }
        return out;
    }

    ~LratOp() {
        if (type == '\0') return;
        if (isLoad()) free(data.load.lits);
        if (isEndLoad()) free(data.endLoad.assumptions);
        if (isDerivation()) free(data.produce.lits);
        if (isImport()) free(data.import.lits);
        if (isDeletion()) free(data.remove.hints);
        if (isSatValidation()) free(data.concludeSat.model);
        if (isUnsatValidation()) free(data.concludeUnsat.failed);
    }
};
