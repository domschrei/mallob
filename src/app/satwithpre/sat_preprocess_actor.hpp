
#pragma once

#include "app/sat/parse/serialized_formula_parser.hpp"
#include "data/job_description.hpp"
#include "util/params.hpp"
#include <vector>

class SatPreprocessActor {

public:
    SatPreprocessActor(const Parameters& params, const std::string& name, std::vector<int>&& inputCnf) :
        _params(params), _name(name), _input_cnf(std::move(inputCnf)) {}

    virtual void preprocessAsync() = 0;
    enum PreprocessActorResult {PENDING, SAT, UNSAT, SIMPLIFIED, ERROR, NONE};
    virtual bool isDonePreprocessing() const {return _result != PENDING;}
    virtual PreprocessActorResult getPreprocessingResult() const {return _result;}
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

protected:
    const Parameters& _params;
    std::string _name;
    const std::vector<int> _input_cnf;
    std::vector<int> _output_cnf;
    std::vector<int> _model;
    volatile PreprocessActorResult _result {PENDING};
    std::future<void> _fut_prepro;
};
