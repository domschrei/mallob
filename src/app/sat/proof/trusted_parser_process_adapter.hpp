
#pragma once

#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include "trusted/trusted_utils.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/subprocess.hpp"
#include "util/params.hpp"

class TrustedParserProcessAdapter {

private:
    int _id;
    FILE* _f_parsed_formula;
    Subprocess* _subproc {nullptr};
    pid_t _child_pid;

    signature _sig;
    int _nb_vars;
    int _nb_cls;
    unsigned long _f_size {0};

public:
    TrustedParserProcessAdapter(int id) : _id(id) {}
    ~TrustedParserProcessAdapter() {
        if (_subproc) delete _subproc;
    }

    template <typename T>
    bool parseAndSign(const char* source, std::vector<T>& out, uint8_t*& outSignature) {
        auto basePath = TmpDir::get() + "/mallob." + std::to_string(Proc::getPid())
            + ".tsparse." + std::to_string(_id);
        auto pathParsedFormula = basePath + ".parsedformula";
        mkfifo(pathParsedFormula.c_str(), 0666);

        Parameters params;
        params.formulaInput.set(source);
        params.fifoParsedFormula.set(pathParsedFormula);
        _subproc = new Subprocess(params, "impcheck_parse");
        _child_pid = _subproc->start();

        _f_parsed_formula = fopen(pathParsedFormula.c_str(), "r");

        // Parse # vars and # clauses
        _nb_vars = TrustedUtils::readInt(_f_parsed_formula);
        _nb_cls = TrustedUtils::readInt(_f_parsed_formula);
        LOG(V3_VERB, "TPPA Parsed %i vars, %i cls\n", _nb_vars, _nb_cls);
        // Parse formula
        size_t fSizeBytes = out.size() * sizeof(T);
        out.resize(out.size() + (_nb_cls*2*sizeof(int))/sizeof(T));
        size_t capacityBytes = out.size() * sizeof(T);
        const size_t outSizeBytesBefore = fSizeBytes;
        const size_t maxBytesToRead = 1<<14;
        while (true) {
            if (fSizeBytes + maxBytesToRead > capacityBytes) {
                capacityBytes = std::max((unsigned long) (1.25*capacityBytes), fSizeBytes+maxBytesToRead);
                out.resize(capacityBytes / sizeof(T));
            }
            const size_t nbReadInts = UNLOCKED_IO(fread)(((uint8_t*) out.data())+fSizeBytes,
                sizeof(int), maxBytesToRead/sizeof(int), _f_parsed_formula);
            const size_t nbReadBytes = nbReadInts * sizeof(int);
            fSizeBytes += nbReadBytes;
            if (nbReadBytes < maxBytesToRead) break;
        }
        // Pop signature from the end of the data
        int* sigOutIntPtr = (int*) _sig;
        int* fIntPtr = (int*) (((u8*)out.data()) + fSizeBytes);
        for (int i = 0; i < 4; i++) sigOutIntPtr[i] = *(fIntPtr-4+i);
        out.resize((fSizeBytes - 4*sizeof(int)) / sizeof(T));
        outSignature = _sig;

        _f_size = (out.size()*sizeof(T) - outSizeBytesBefore) / sizeof(int);
        const int* _f_data = (int*) (((u8*) out.data()) + outSizeBytesBefore);
        std::string summary;
        for (size_t i = 0; i < std::min(5UL, _f_size); i++) summary += std::to_string(_f_data[i]) + " ";
        if (_f_size > 10) summary += " ... ";
        for (size_t i = std::max(5UL, _f_size-5); i < _f_size; i++) summary += std::to_string(_f_data[i]) + " ";
        LOG(V3_VERB, "TPPA %lu lits: %s\n", _f_size, summary.c_str());

        fclose(_f_parsed_formula);
        FileUtils::rm(pathParsedFormula);
        return true;
    }

    int getNbVars() const {return _nb_vars;}
    int getNbClauses() const {return _nb_cls;}
    size_t getFSize() const {return _f_size;}

private:
    void doAbort() {
        printf("ERROR ERROR ERROR\n");
        while (true) {}
    }
};
