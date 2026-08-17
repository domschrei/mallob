
#pragma once

#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include "app/sat/proof/impcheck.hpp"
#include "app/sat/proof/impcheck_program_lookup.hpp"
#include "trusted/trusted_utils.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/subprocess.hpp"
#include "util/params.hpp"

class TrustedNonincParserProcessAdapter {

private:
    int _base_seed;

    int _id;
    FILE* _f_parsed_formula;
    Subprocess* _subproc {nullptr};
    pid_t _child_pid;

    signature _sig;
    int _nb_vars;
    int _nb_cls;
    unsigned long _f_size {0};

public:
    TrustedNonincParserProcessAdapter(int baseSeed, int id) : _base_seed(baseSeed), _id(id) {}
    ~TrustedNonincParserProcessAdapter() {
        if (_subproc) delete _subproc;
    }

    template <typename T>
    bool parseAndSign(const char* source, std::vector<T>& out, uint8_t*& outSignature) {
        auto basePath = TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallob." + std::to_string(Proc::getPid())
            + ".tsparse." + std::to_string(_id);
        auto pathParsedFormula = basePath + ".parsedformula";
        mkfifo(pathParsedFormula.c_str(), 0666);

        unsigned long keySeed = ImpCheck::getKeySeed(_base_seed);
        std::string moreArgs = "-key-seed=" + std::to_string(keySeed)
            + " -formula=" + source
            + " -output=" + pathParsedFormula;

        LOG(V3_VERB, "TPPA Parsing ...\n");
        _subproc = new Subprocess(Parameters(), 
            ImpCheckProgramLookup::getParserExecutablePath(false),
            moreArgs, false);
        _child_pid = _subproc->start();

        _f_parsed_formula = fopen(pathParsedFormula.c_str(), "r");

        // Parse # vars and # clauses
        ImpCheckIO io;
        _nb_vars = io.readInt(_f_parsed_formula);
        _nb_cls = io.readInt(_f_parsed_formula);
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
        fSizeBytes -= 4*sizeof(int);
        outSignature = _sig;

        // Insert three i32: "INT32MAX | 0 | INT32MIN"
        out.resize((fSizeBytes + 7*sizeof(int)) / sizeof(T));
        for (int x : {
                // End of formula, empty set of assumptions
                INT32_MAX, 0,
                // Signature
                sigOutIntPtr[0], sigOutIntPtr[1], sigOutIntPtr[2], sigOutIntPtr[3],
                // End of payload
                INT32_MIN
            }) {
            memcpy(out.data() + fSizeBytes, &x, sizeof(int));
            fSizeBytes += sizeof(int);
        }

        _f_size = (out.size()*sizeof(T) - outSizeBytesBefore) / sizeof(int);
        LOG(V3_VERB, "TPPA %lu lits\n", _f_size);

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
