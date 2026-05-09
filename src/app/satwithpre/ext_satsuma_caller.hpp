
#pragma once

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>

#include "app/sat/parse/serialized_formula_parser.hpp"
#include "data/job_description.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/assert.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include <fstream>
#include <future>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class ExtSatsumaCaller {

private:
    const Parameters& _params;
    std::string _in_path;
    std::string _out_path;

    volatile bool _result_available {false};

    std::vector<int> _result_cnf;
    int _result_vars {0};
    int _result_cls {0};
    int _result_code = 0;

    std::future<void> _fut_in;
    std::future<void> _fut_out;

public:
    ExtSatsumaCaller(const Parameters& params, const JobDescription& desc) : _params(params) {

        std::string basePath = TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallob."
            + std::to_string(Proc::getPid()) + "."
            + std::to_string(desc.getId()) + ".satsuma.";
        _in_path = basePath + "in.pipe.cnf";
        _out_path = basePath + "out.pipe.cnf";

        int res;
        res = mkfifo(_in_path.c_str(), 0666);
        if (res == -1) abort();
        res = mkfifo(_out_path.c_str(), 0666);
        if (res == -1) abort();
        LOG(V4_VVER, "Satsuma pipes created\n");

        _fut_in = ProcessWideThreadPool::get().addTask([&]() {
            loadFormulaToPipe(desc);
        });
        _fut_out = ProcessWideThreadPool::get().addTask([&]() {
            readPreprocessedFormula();
        });
    }
    ~ExtSatsumaCaller() {}

    enum ExtSatsumaResult {SUCCESS, UNSAT, ERROR};
    ExtSatsumaResult callBlocking() {

        std::string cmd = std::string(MALLOB_SUBPROC_DISPATCH_PATH"/satsuma")
            + " fix --add-reduced-as-unit --file " + _in_path
            + " --out-file " + _out_path
            + " > " + (_params.logDirectory.isSet() ? (_params.logDirectory() + "/satsuma.txt") : "/dev/null")
            + " 2>&1";

        LOG(V4_VVER, "Calling External Satsuma: %s\n", cmd.c_str());
        const int retval = system(cmd.c_str());
        _fut_in.get();
        _fut_out.get();

        LOG(V4_VVER, "External Satsuma returned, retval=%i\n", retval);
        if (retval != 0) return ERROR;

        _result_available = true;
        if (_result_code == 20) return UNSAT;
        return SUCCESS;
    }

    bool hasPreprocessedFormula() const {
        return _result_available;
    }

    std::vector<int>&& getPreprocessedFormula() {
        _result_cnf.push_back(_result_vars);
        _result_cnf.push_back(_result_cls);
        return std::move(_result_cnf);
    }

private:
    void loadFormulaToPipe(const JobDescription& _desc) {

        LOG(V4_VVER, "Loading formula to Satsuma pipe ...\n");
        SerializedFormulaParser parser(Logger::getMainInstance(), _desc.getFormulaPayload(0),
            _desc.getFormulaPayloadSize(0));
        if (_params.compressFormula()) parser.setCompressed();
        int numberOfVariables = _desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
        int numberOfClauses = _desc.getAppConfiguration().fixedSizeEntryToInt("__NC");

        assert(numberOfVariables > 0 && numberOfVariables < 1000000000);
        int lit;
        std::ofstream ofs(_in_path);
        ofs << "p cnf " << numberOfVariables << " " << numberOfClauses << "\n";
        int inputLits = 0;
        while (parser.getNextLiteral(lit) && ofs.is_open()) {
            ofs << lit << (lit == 0 ? "\n" : " ");
            inputLits++;
        }
        LOG(V4_VVER, "Forwarded %i lits to Satsuma\n", inputLits);
        ofs.close();
    }

    void readPreprocessedFormula() {
        std::ifstream ifs(_out_path);
        std::string line;
        bool unsat = false;
        bool lastLitZero = true;
        int outputLits = 0;
        while (std::getline(ifs, line)) {
            if (line.empty() || line[0] == 'c') continue;

            if (line[0] == 'p') {
                std::istringstream iss(line);
                std::string p, cnf;
                iss >> p >> cnf >> _result_vars >> _result_cls;
                continue;
            }

            std::istringstream iss(line);
            int lit;
            while (iss >> lit) {
                _result_cnf.push_back(lit);
                if (lit == 0 && lastLitZero) _result_code = 20;
                lastLitZero = lit == 0;
                outputLits++;
            }
        }
        LOG(V4_VVER, "Received %i lits from Satsuma\n", outputLits);
    }
};
