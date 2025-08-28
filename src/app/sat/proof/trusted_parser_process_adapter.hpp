
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>

#include "app/sat/proof/impcheck.hpp"
#include "trusted/trusted_utils.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/subprocess.hpp"
#include "util/params.hpp"

class TrustedParserProcessAdapter {

private:
    int _base_seed;

    int _id;
    std::string _path_parsed_formula;
    FILE* _f_parsed_formula;
    Subprocess* _subproc {nullptr};
    pid_t _child_pid;

    std::vector<int> _f_buf;
    int _f_buf_size {0};
    int _f_buf_idx {0};

    enum ParsingStage {LIT, ASMPT, SIG} _stage;
    int _sig_nb_read {0};

    signature _sig;
    int _nb_vars {0};
    int _nb_cls {0};
    unsigned long _f_size {0};

public:
    TrustedParserProcessAdapter(int baseSeed, int id) : _base_seed(baseSeed), _id(id), _f_buf(1<<14) {}
    ~TrustedParserProcessAdapter() {
        fclose(_f_parsed_formula);
        FileUtils::rm(_path_parsed_formula);
        if (_subproc) delete _subproc;
    }

    void setup(const char* source) {
        auto basePath = TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallob." + std::to_string(Proc::getPid())
            + ".tsparse." + std::to_string(_id);
        _path_parsed_formula = basePath + ".parsedformula";
        mkfifo(_path_parsed_formula.c_str(), 0666);

        Parameters params;
        params.formulaInput.set(source);
        params.fifoParsedFormula.set(_path_parsed_formula);

        unsigned long keySeed = ImpCheck::getKeySeed(_base_seed);
        std::string moreArgs = "-key-seed=" + std::to_string(keySeed);
        moreArgs += " -input-log=parser-input.txt";

        _subproc = new Subprocess(params, "impcheck_parse", moreArgs);
        _child_pid = _subproc->start();
        // Non-blocking reading so that we can read until the end of an increment
        int fd = open(_path_parsed_formula.c_str(), O_RDONLY | O_NONBLOCK);
        _f_parsed_formula = fdopen(fd, "r");
    }

    inline bool processNextIntAndCheckDone(int& x) {

        while (_f_buf_idx == _f_buf_size) {
            // Read next chunk
            _f_buf_size = UNLOCKED_IO(fread)(_f_buf.data(),
                sizeof(int), _f_buf.size(), _f_parsed_formula);
            assert(_f_buf_size >= 0);
            _f_buf_idx = 0;
        }

        x = _f_buf[_f_buf_idx++];
        _f_size++;

        if (_stage == LIT && x == INT32_MAX) {
            _stage = ASMPT;
            return false;
        }
        if (_stage == ASMPT && x == 0) {
            _stage = SIG;
            return false;
        }
        if (_stage == SIG) {
            if (_sig_nb_read == 4) {
                assert(x == INT32_MIN);
                return true;
            }
            _sig_nb_read++;
            return false;
        }

        if (_stage == LIT && x == 0) _nb_cls++;
        if (x != 0) {
            assert(x != INT32_MAX && x != INT32_MIN);
            _nb_vars = std::max(_nb_vars, std::abs(x));
        }
        return false;
    }

    template <typename T>
    bool parseAndSign(std::vector<T>& out, uint8_t*& outSignature) {
        _f_size = 0;
        _stage = LIT;
        _sig_nb_read = 0;

        // Parse formula
        size_t fSizeBytes = out.size() * sizeof(T);
        assert(fSizeBytes % sizeof(int) == 0); // aligned!
        //out.resize(out.size() + (_nb_cls*2*sizeof(int))/sizeof(T));
        size_t capacityBytes = out.size() * sizeof(T);
        const size_t outSizeBytesBefore = fSizeBytes;

        bool done = false;
        while (!done) {
            // Read next chunk of data.
            if (fSizeBytes + sizeof(int) > capacityBytes) {
                capacityBytes = std::max((unsigned long) (1.25*capacityBytes),
                    fSizeBytes + _f_buf_size*sizeof(int));
                out.resize(capacityBytes / sizeof(T));
            }
            int x;
            done = processNextIntAndCheckDone(x);
            memcpy(((u8*)out.data())+fSizeBytes, &x, sizeof(int));
            fSizeBytes += sizeof(int);
        }
        out.resize(fSizeBytes / sizeof(T));

        // Read signature from the end of the data
        memcpy(_sig, ((u8*)out.data()) + fSizeBytes - sizeof(int) - SIG_SIZE_BYTES, SIG_SIZE_BYTES);
        outSignature = _sig;
        LOG(V3_VERB, "TPPA %lu lits\n", _f_size);
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
