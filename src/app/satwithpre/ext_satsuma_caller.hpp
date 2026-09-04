
#pragma once

#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <asm/termbits.h>  /* Definition of constants */
#include <sys/ioctl.h>

#include "app/satwithpre/sat_preprocess_actor.hpp"
#include "data/job_description.hpp"
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/subprocess.hpp"
#include "util/sys/thread_pool.hpp"
#include <fstream>
#include <future>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class ExtSatsumaCaller : public SatPreprocessActor {

private:
    std::string _in_path;
    std::string _out_path;

    pid_t _pid {0};

    std::future<void> _fut_in;
    std::future<void> _fut_out;

    int _orig_nb_vars;
    int _orig_nb_cls;
    volatile bool _received_empty_clause {false};
    volatile bool _simplification_achieved {false};

public:
    ExtSatsumaCaller(const Parameters& params, const JobDescription& desc, const std::string& name, std::vector<int>&& formula) :
        SatPreprocessActor(params, name, std::move(formula)) {

        std::string basePath = TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallobtermrelev."
            + std::to_string(Proc::getPid()) + "."
            + std::to_string(desc.getId()) + "." + name + ".";
        std::ostringstream oss;
        oss << static_cast<const void*>(this);
        basePath += oss.str() + ".";

        _in_path = basePath + "in.pipe.cnf";
        _out_path = basePath + "out.pipe.cnf";

        int res;
        res = mkfifo(_in_path.c_str(), 0666);
        if (res == -1) abort();
        res = mkfifo(_out_path.c_str(), 0666);
        if (res == -1) abort();
        LOG(V4_VVER, "%s pipes created\n", getName());
    }
    ~ExtSatsumaCaller() {
        if (_fut_prepro.valid()) _fut_prepro.get();
    }

    void preprocessAsync() override {

        _fut_in = ProcessWideThreadPool::get().addTask([&]() {
            loadFormulaToPipe();
        });
        _fut_out = ProcessWideThreadPool::get().addTask([&]() {
            readPreprocessedFormula();
        });

        CoreAllocator::Allocation ca(1);
        std::string cmd = std::string("run-satsuma.sh ")
            + MALLOB_SUBPROC_DISPATCH_PATH + " " + _in_path + " " + _out_path + " "
            + (_params.logDirectory.isSet() ? (_params.logDirectory() + "/satsuma.txt") : "/dev/null");
        Subprocess subSatsuma(_params, cmd, false);

        LOG(V4_VVER, "%s Calling Satsuma: %s\n", getName(), cmd.c_str());
        _pid = subSatsuma.start();
        LOG(V4_VVER, "%s started\n", getName());

        _fut_prepro = ProcessWideThreadPool::get().addTask([&]() {
            _fut_in.get();
            _fut_out.get();
            if (_pid < 0) _result = ERROR;
            else if (_received_empty_clause) _result = UNSAT;
            else if (_simplification_achieved) _result = SIMPLIFIED;
            else _result = NONE;
            LOG(V4_VVER, "%s result %i\n", getName(), _result);
        });
    }

    std::vector<int>&& getPreprocessedFormula() override {
        assert(!_output_cnf.empty());
        assert(_output_cnf[0] != 0);
        LOG(V4_VVER, "%s Returning preprocessed formula of size %lu : %s\n", getName(),
            _output_cnf.size(), StringUtils::getSummary(_output_cnf).c_str());
        return std::move(_output_cnf);
    }

    void reconstructSolution(std::vector<int>& sol) override {
        sol.resize(nbInputVars() + 1);
    }

    void interrupt() override {
        if (!_fut_prepro.valid() || _result != PENDING) return;
        if (_pid > 0) {
            LOG(V4_VVER, "%s TERMINATE\n", getName());
            Process::sendSignal(_pid, SIGTERM);
            return;
        }
    }

private:
    void loadFormulaToPipe() {

        LOG(V4_VVER, "%s Loading formula to Satsuma pipe ...\n", getName());

        assert(nbInputVars() > 0 && nbInputVars() < 1'000'000'000);

        std::ofstream ofs(_in_path.c_str());

        _orig_nb_vars = nbInputVars();
        _orig_nb_cls = nbInputClauses();
        LOG(V4_VVER, "%s Writing p cnf %i %i\n", getName(), _orig_nb_vars, _orig_nb_cls);

        ofs << "p cnf " << _orig_nb_vars << " " << _orig_nb_cls << "\n";
        for (int i = 0; i+2 < _input_cnf.size(); i++) {
            int lit = _input_cnf[i];
            ofs << lit << (lit==0 ? "\n" : " ");
        }
        // ofs << "X";
        ofs.flush();

        LOG(V4_VVER, "%s Forwarded %lu lits to Satsuma\n", getName(), _input_cnf.size()-2);
    }

    void readPreprocessedFormula() {

        LOG(V4_VVER, "%s opening ifs ...\n", getName());
        std::ifstream ifs(_out_path);
        LOG(V4_VVER, "%s ifs open\n", getName());
        
        std::string line;
        bool lastLitZero = true;
        int outputLits = 0;
        int nbVars, nbClauses;
        while (std::getline(ifs, line)) {
            if (line.empty() || line[0] == 'c') continue;

            if (line[0] == 'p') {
                std::istringstream iss(line);
                std::string p, cnf;
                iss >> p >> cnf >> nbVars >> nbClauses;
                continue;
            }

            std::istringstream iss(line);
            int lit;
            while (iss >> lit) {
                _output_cnf.push_back(lit);
                if (lit == 0 && lastLitZero) {
                    LOG(V4_VVER, "%s reported empty clause\n", getName());
                    _received_empty_clause = true;
                }
                lastLitZero = lit == 0;
                outputLits++;
            }
        }
        _output_cnf.push_back(nbVars);
        _output_cnf.push_back(nbClauses);
        ifs.close();
        _simplification_achieved = (nbVars != _orig_nb_vars || nbClauses != _orig_nb_cls
            || _output_cnf.size() != _input_cnf.size());
        LOG(V4_VVER, "%s Received %i lits from Satsuma; simplification: %s\n", getName(), outputLits,
            _simplification_achieved ? "yes" : "no");
    }
};
