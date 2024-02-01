
#pragma once

#include <cstring>
#include <algorithm>

#include "util/logger.hpp"
#include "util/assert.hpp"
#include "trusted/trusted_utils.hpp"

struct LratOp {
    size_t datalen {0};
    u8* data {nullptr};

    // Derivation / production / addition
    LratOp(u64 id, const int* lits, int nbLits, const u64* hints, int nbHints, int glue) :
            datalen(sizeof(nbLits) + sizeof(nbHints) + sizeof(id) 
                    + nbLits*sizeof(int) + nbHints*sizeof(u64) + sizeof(int)),
            data((u8*) malloc(datalen)) {
        size_t i = 0, n;
        assert(glue >= 0);
        n = sizeof(nbLits);      memcpy(data+i, &nbLits, n);  i += n;
        n = sizeof(nbHints);     memcpy(data+i, &nbHints, n); i += n;
        n = sizeof(id);          memcpy(data+i, &id, n);      i += n;
        n = sizeof(int)*nbLits;  memcpy(data+i, lits, n);     i += n;
        n = sizeof(u64)*nbHints; memcpy(data+i, hints, n);    i += n;
        n = sizeof(glue);        memcpy(data+i, &glue, n);    i += n;
        //assert(isDerivation());
    }
    // Import
    LratOp(u64 id, const int* lits, int nbLits, const u8* sig) :
            datalen(2*sizeof(int) + sizeof(id) + nbLits*sizeof(int) + SIG_SIZE_BYTES + sizeof(int)),
            data((u8*) malloc(datalen)) {
        size_t i = 0, n;
        const int nbHints = SIG_SIZE_BYTES / sizeof(u64);
        const int glue = -1;
        n = sizeof(nbLits);      memcpy(data+i, &nbLits, n);  i += n;
        n = sizeof(nbHints);     memcpy(data+i, &nbHints, n); i += n;
        n = sizeof(id);          memcpy(data+i, &id, n);      i += n;
        n = sizeof(int)*nbLits;  memcpy(data+i, lits, n);     i += n;
        n = sizeof(u64)*nbHints; memcpy(data+i, sig, n);      i += n;
        n = sizeof(glue);        memcpy(data+i, &glue, n);    i += n;
        //assert(isImport());
    }
    // Deletion
    LratOp(const u64* hints, int nbHints) :
            datalen(2*sizeof(int) + sizeof(u64) + nbHints*sizeof(u64)),
            data((u8*) malloc(datalen)) {
        size_t i = 0, n;
        const int nbLits = 0;
        const u64 id = 0;
        n = sizeof(nbLits);      memcpy(data+i, &nbLits, n);  i += n;
        n = sizeof(nbHints);     memcpy(data+i, &nbHints, n); i += n;
        n = sizeof(id);          memcpy(data+i, &id, n);      i += n;
        n = sizeof(u64)*nbHints; memcpy(data+i, hints, n);    i += n;
        //assert(isDeletion());
    }
    // UNSAT Validation (20) or termination (0)
    LratOp(char x) : datalen(1), data((u8*) malloc(1)) {
        data[0] = x;
    }
    LratOp() {}
    LratOp(LratOp&& moved) : datalen(moved.datalen), data(moved.data) {
        moved.datalen = 0;
        moved.data = nullptr;
    }
    LratOp& operator=(LratOp&& moved) {
        this->datalen = moved.datalen;
        this->data = moved.data;
        moved.datalen = 0;
        moved.data = nullptr;
        return *this;
    }

    void sortLiterals() {
        std::sort(getLits(), getLits()+getNbLits());
    }

    bool isDerivation() const {return getType() == DERIVATION;}
    bool isImport() const {return getType() == IMPORT;}
    bool isDeletion() const {return getType() == DELETION;}
    bool isUnsatValidation() const {return getType() == VALIDATION;}
    bool isTermination() const {return getType() == TERMINATION;}

    enum Type {DERIVATION, IMPORT, DELETION, VALIDATION, TERMINATION};
    Type getType() const {
        if (datalen == 1) return data[0] == 0 ? TERMINATION : VALIDATION;
        if (getId() == 0) return DELETION;
        if (getGlue() < 0) return IMPORT;
        return DERIVATION;
    }

    int getNbLits() const {return *(u64*) (data);}
    int getNbHints() const {return *(u64*) (data + sizeof(int));}
    u64 getId() const {return *(u64*) (data + 2*sizeof(int));}
    int* getLits() {return (int*) (data + 2*sizeof(int) + sizeof(u64));}
    const int* getLits() const {return (int*) (data + 2*sizeof(int) + sizeof(u64));}
    const u64* getHints() const {return (u64*) (data + 2*sizeof(int) + sizeof(u64) + getNbLits()*sizeof(int));}
    const u8* getSignature() const {return (u8*) getHints();}
    int getGlue() const {return *(int*) (data + datalen - sizeof(int));}

    std::string toStr() const {
        std::string out;
        auto type = getType();
        if (type == DERIVATION) {
            out += "a " + std::to_string(getId()) + " ";
            for (int i = 0; i < getNbLits(); i++) out += std::to_string(getLits()[i]) + " ";
            out += "0 ";
            for (int i = 0; i < getNbHints(); i++) out += std::to_string(getHints()[i]) + " ";
            out += "0";
        } else if (type == IMPORT) {
            out += "i " + std::to_string(getId()) + " ";
            for (int i = 0; i < getNbLits(); i++) out += std::to_string(getLits()[i]) + " ";
            out += "0 ";
            out += Logger::dataToHexStr(getSignature(), SIG_SIZE_BYTES) + " ";
            out += "0";
        }
        return out;
    }

    ~LratOp() {
        if (data) free(data);
    }
};
