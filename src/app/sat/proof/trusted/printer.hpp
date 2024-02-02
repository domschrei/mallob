
#pragma once

#define MALLOB_PRINT_TRUSTED_DIRECTIVES 0

#if MALLOB_PRINT_TRUSTED_DIRECTIVES
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unistd.h>
#endif

#include "trusted_utils.hpp"


class Printer {

private:
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
    std::ofstream _ofs;
#endif

public:
    Printer()
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        : _ofs("lrat-directives-" + std::to_string(getpid()))
#endif
        {}

    inline void printInitDirective(int nbVars, signature sig) {
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        _ofs << "B " + std::to_string(nbVars) + " " + signatureToString(sig) + "\n";
#endif
    }

    inline void printLoadDirective(const int* lits, int nbLits) {
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        _ofs << "L " + literalsToString(lits, nbLits) + "\n";
#endif
    }

    inline void printEndLoadingDirective() {
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        _ofs << "E\n";
#endif
    }

    inline void printProduceDirective(u64 id, const int* lits, int nbLits, const u64* hints, int nbHints) {
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        _ofs << "a " + std::to_string(id) + " " + literalsToString(lits, nbLits) + "0 " + hintsToString(hints, nbHints) + "\n";
#endif
    }

    inline void printImportDirective(u64 id, const int* lits, int nbLits, signature sig) {
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        _ofs << "i " + std::to_string(id) + " " + literalsToString(lits, nbLits) + "0 " + signatureToString(sig) + "\n";
#endif
    }

    inline void printDeleteDirective(const u64* hints, int nbHints) {
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        _ofs << "d " + hintsToString(hints, nbHints) + "\n";
#endif
    }

    inline void printValidateDirective() {
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        _ofs << "V\n";
#endif
    }

    inline void printTerminateDirective() {
#if MALLOB_PRINT_TRUSTED_DIRECTIVES
        _ofs << "T\n";
#endif
    }

#if MALLOB_PRINT_TRUSTED_DIRECTIVES
private:
    std::string signatureToString(signature sig) {
        std::stringstream stream;
        for (int i = 0; i < SIG_SIZE_BYTES; i++) {
            stream << std::hex << std::setfill('0') << std::setw(2) << (int) sig[i];
        }
        return stream.str();
    }
    std::string literalsToString(const int* lits, int nbLits) {
        std::string out;
        for (int i = 0; i < nbLits; i++) out += std::to_string(lits[i]) + " ";
        return out;
    }
    std::string hintsToString(const u64* hints, int nbHints) {
        std::string out;
        for (int i = 0; i < nbHints; i++) out += std::to_string(hints[i]) + " ";
        return out;
    }
#endif

};
