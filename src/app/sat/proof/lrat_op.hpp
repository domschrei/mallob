
#pragma once

#include <climits>
#include <cstring>
#include <algorithm>

#include "util/logger.hpp"
#include "util/assert.hpp"
#include "trusted/trusted_utils.hpp"

struct LratOp {
    size_t datalen {0};
    u8* data {nullptr};

    // LRAT-style derivation / production / addition
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
    // DRAT-style derivation / production / addition
    LratOp(const int* lits, int nbLits, int glue) :
            datalen(2*sizeof(int) + sizeof(u64) + nbLits*sizeof(int) + sizeof(int)),
            data((u8*) malloc(datalen)) {
        size_t i = 0, n;
        const u64 id = ULONG_MAX;
        const int nbHints = 0;
        assert(glue >= 0);
        n = sizeof(nbLits);      memcpy(data+i, &nbLits, n);  i += n;
        n = sizeof(nbHints);     memcpy(data+i, &nbHints, n); i += n;
        n = sizeof(id);          memcpy(data+i, &id, n);      i += n;
        n = sizeof(int)*nbLits;  memcpy(data+i, lits, n);     i += n;
        //n = sizeof(u64)*nbHints; memcpy(data+i, hints, n);    i += n; // always zero
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
    // LRAT-style deletion (list of hints)
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
    // DRAT-style deletion (list of clause literals)
    LratOp(const int* lits, int nbLits) :
            datalen(2*sizeof(int) + sizeof(u64) + nbLits*sizeof(int)),
            data((u8*) malloc(datalen)) {
        size_t i = 0, n;
        const int nbHints = 0;
        const u64 id = 0;
        n = sizeof(nbLits);      memcpy(data+i, &nbLits, n);  i += n;
        n = sizeof(nbHints);     memcpy(data+i, &nbHints, n); i += n;
        n = sizeof(id);          memcpy(data+i, &id, n);      i += n;
        n = sizeof(int)*nbLits;  memcpy(data+i, lits, n);     i += n;
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
        auto lits = getLits();
        std::sort(lits, lits+getNbLits());
    }

    bool isDerivation() const {return getType() == DERIVATION;}
    bool isImport() const {return getType() == IMPORT;}
    bool isDeletion() const {return getType() == DELETION;}
    bool isUnsatValidation() const {return getType() == VALIDATION_UNSAT;}
    bool isSatValidation() const {return getType() == VALIDATION_SAT;}
    bool isTermination() const {return getType() == TERMINATION;}

    enum Type {DERIVATION, IMPORT, DELETION, VALIDATION_SAT, VALIDATION_UNSAT, TERMINATION};
    Type getType() const {
        if (datalen == 1) {
            switch (data[0]) {
            case 0:
                return TERMINATION;
            case 10:
                return VALIDATION_SAT;
            case 20:
                return VALIDATION_UNSAT;
            default:
                abort();
            }
        }
        if (getId() == 0) return DELETION;
        if (getGlue() < 0) return IMPORT;
        return DERIVATION;
    }
    bool isDratDerivation() const {
        if (!isDerivation()) return false;
        return getId() == ULONG_MAX && getNbHints() == 0;
    }
    bool isDratDeletion() const {
        if (!isDeletion()) return false;
        return getNbLits() > 0 && getNbHints() == 0;
    }

    int getNbLits() const {return *(u64*) (data);}
    int getNbHints() const {return *(u64*) (data + sizeof(int));}
    u64& getId() {return *(u64*) (data + 2*sizeof(int));}
    u64 getId() const {return *(u64*) (data + 2*sizeof(int));}
    int* getLits() {return (int*) (data + 2*sizeof(int) + sizeof(u64));}
    const int* getLits() const {return (int*) (data + 2*sizeof(int) + sizeof(u64));}
    u64* getHints() {return (u64*) (data + 2*sizeof(int) + sizeof(u64) + getNbLits()*sizeof(int));}
    const u64* getHints() const {return (u64*) (data + 2*sizeof(int) + sizeof(u64) + getNbLits()*sizeof(int));}
    const u8* getSignature() const {return (u8*) getHints();}
    u8* signature() {return (u8*) getHints();}
    int getGlue() const {return *(int*) (data + datalen - sizeof(int));}
    int& glue() {return *(int*) (data + datalen - sizeof(int));}

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
