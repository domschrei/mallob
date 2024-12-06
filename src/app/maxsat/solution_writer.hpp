
#pragma once

#include <fstream>
#include <vector>

class SolutionWriter {

private:
    int _nb_vars;
    std::ofstream _ofs;

public:
    SolutionWriter(int nbVars, const std::string& path) : _nb_vars(nbVars) {
        if (!path.empty()) _ofs = std::ofstream(path);
    }

    void appendSolution(unsigned long cost, const std::vector<int>& model) {
        _ofs << "c " << Timer::elapsedSeconds() << " MAXSAT NEW SOLUTION FOUND " << std::endl
             << "o " << cost << std::endl;
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
        _ofs.flush();
    }

    void concludeOptimal() {
        _ofs << "s OPTIMUM FOUND" << std::endl;
        _ofs.flush();
    }
};
