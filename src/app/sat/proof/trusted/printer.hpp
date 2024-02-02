
#pragma once

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unistd.h>

#include "trusted_utils.hpp"

class Printer {

private:
    std::ofstream _ofs;

public:
    Printer() : _ofs("lrat-directives-" + std::to_string(getpid())) {}

    void printInitDirective(int nbVars, signature sig) {
        _ofs << "B " + std::to_string(nbVars) + " " + signatureToString(sig) + "\n";
    }

    void printLoadDirective(const int* lits, int nbLits) {
        _ofs << "L " + literalsToString(lits, nbLits) + "\n";
    }

    void printEndLoadingDirective() {
        _ofs << "E\n";
    }

    void printProduceDirective(u64 id, const int* lits, int nbLits, const u64* hints, int nbHints) {
        _ofs << "a " + std::to_string(id) + " " + literalsToString(lits, nbLits) + "0 " + hintsToString(hints, nbHints) + "\n";
    }

    void printImportDirective(u64 id, const int* lits, int nbLits, signature sig) {
        _ofs << "i " + std::to_string(id) + " " + literalsToString(lits, nbLits) + "0 " + signatureToString(sig) + "\n";
    }

    void printDeleteDirective(const u64* hints, int nbHints) {
        _ofs << "d " + hintsToString(hints, nbHints) + "\n";
    }

    void printValidateDirective() {
        _ofs << "V\n";
    }

    void printTerminateDirective() {
        _ofs << "T\n";
    }

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

};
