
#pragma once

#include "util/assert.hpp"
#include "util/params.hpp"
#include "util/sys/thread_pool.hpp"
#include <filesystem>
#include <fstream>
#include <future>
#include <vector>

class SatPreprocessActor {

public:
    SatPreprocessActor(const Parameters& params, const std::string& name, std::vector<int>&& inputCnf) :
        _params(params), _name(name), _input_cnf(std::move(inputCnf)) {}

    virtual void preprocessAsync() = 0;
    enum PreprocessActorResult {PENDING, SAT, UNSAT, SIMPLIFIED, ERROR, NONE};
    virtual bool isDonePreprocessing() const {return _result != PENDING;}
    PreprocessActorResult getPreprocessingResult() const {
        if (_result == UNSAT && _params.savePreprocessingProofs() && _proof_format.empty())
            return PreprocessActorResult::NONE;
        return _result;
    }
    std::string getPreprocessingResultAsString() const {
        switch (_result) {
        case NONE: return "NONE";
        case PENDING: return "PENDING";
        case SAT: return "SAT";
        case UNSAT: return "UNSAT";
        case SIMPLIFIED: return "SIMPLIFIED";
        default: return "ERROR";
        }
    }
    virtual std::vector<int>&& getPreprocessedFormula() {
        return std::move(_output_cnf);
    }
    virtual std::vector<int>&& getModel() {
        return std::move(_model);
    }

    virtual void interrupt() {}
    virtual void join() {if (_fut_prepro.valid()) _fut_prepro.get();}
    virtual void reconstructSolution(std::vector<int>& sol) = 0;

    const std::string& getProofFormat() const {return _proof_format;}

    int nbInputVars() const {
        assert(_input_cnf.size() >= 2);
        return _input_cnf[_input_cnf.size() - 2];
    }
    int nbInputClauses() const {
        assert(_input_cnf.size() >= 2);
        return _input_cnf[_input_cnf.size() - 1];
    }
    const std::vector<int>& getInputCnf() const {
        return _input_cnf;
    }
    const char* getName() const {return _name.c_str();}

    virtual bool rename_proof(int i, std::string cnfSrc = "",
            std::string proofSrc = "") {
        if (_fut_cnf.valid()) _fut_cnf.get();

        if (cnfSrc.empty()) cnfSrc = _params.proofDirectory() + "/tmp/" + _name + ".cnf";
        if (proofSrc.empty()) proofSrc = _params.proofDirectory() + "/tmp/" + _name + "." + _proof_format;

        if (std::filesystem::exists(cnfSrc)) {
            std::error_code ec;
            std::filesystem::rename(cnfSrc, _params.proofDirectory() + "/post" + std::to_string(i) + ".cnf", ec);
        }
        try {
            std::filesystem::rename(
                proofSrc,
                _params.proofDirectory() + "/step" + std::to_string(i) + "." + _proof_format
            );
            return true;
        } catch (const std::filesystem::filesystem_error& e) {
            return false;
        }
    }

    virtual void writeCnf(const std::vector<int>& cnf, std::string path = "") {
        if (path.empty()) path = _params.proofDirectory() + "/tmp/" + _name + ".cnf";
        _fut_cnf = ProcessWideThreadPool::get().addTask([path, &cnf]() {
            std::ofstream ofs(path);
            int nbVars = cnf[cnf.size() - 2];
            int nbClauses = cnf[cnf.size() - 1];
            ofs << "p cnf " << nbVars << " " << nbClauses << "\n";
            for (size_t i = 0; i + 2 < cnf.size(); i++) {
                int lit = cnf[i];
                ofs << lit << (lit == 0 ? "\n" : " ");
            }
        });
    }

protected:
    const Parameters _params;
    std::string _name;
    std::string _proof_format;
    const std::vector<int> _input_cnf;
    std::vector<int> _output_cnf;
    std::vector<int> _model;
    volatile PreprocessActorResult _result {PENDING};
    std::future<void> _fut_prepro;
    std::future<void> _fut_cnf;
};
