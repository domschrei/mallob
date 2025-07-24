
#pragma once

#include "app/sat/data/model_string_compressor.hpp"
#include "util/sys/timer.hpp"
#include "util/assert.hpp"

#include <fstream>
#include <vector>

class SolutionWriter {

private:
    int _nb_vars;
    std::ofstream _ofs;
    bool _compress_models {false};

public:
    SolutionWriter(int nbVars, const std::string& path, bool compressModels) :
            _nb_vars(nbVars), _compress_models((compressModels)) {
        if (!path.empty()) _ofs = std::ofstream(path);
    }

    void appendSolution(unsigned long cost, const std::vector<int>& model) {
        _ofs << "c " << Timer::elapsedSeconds() << " MAXSAT NEW SOLUTION FOUND " << std::endl
             << "o " << cost << std::endl;
        if (_compress_models) {
            _ofs << "v " << ModelStringCompressor::compress(model) << std::endl;
        } else {
            int v = 1;
            while (v <= _nb_vars) {
                _ofs << "v ";
                for (size_t i = 1; i <= 30; i++) {
                    assert(v < model.size());
                    _ofs << model[v] << " ";
                    v++;
                    if (v > _nb_vars) {
                        _ofs << "0";
                        break;
                    }
                }
                _ofs << std::endl;
            }
        }
        _ofs.flush();
    }

    void concludeOptimal() {
        _ofs << "s OPTIMUM FOUND" << std::endl;
        _ofs.flush();
    }
};
