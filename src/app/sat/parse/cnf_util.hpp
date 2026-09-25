
#pragma once

#include "app/sat/parse/serialized_formula_parser.hpp"
#include "data/job_description.hpp"

class CnfUtil {

public:
    static std::vector<int> getCnfFromJobDescription(const JobDescription& _desc, bool addAssumptionUnits) {

        SerializedFormulaParser parser(Logger::getMainInstance(), _desc.getFormulaPayload(0),
            _desc.getFormulaPayloadSize(0));
        int nbVars = _desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
        int nbCls = 0;

        std::vector<int> cnf;
        int lit;
        while (parser.getNextLiteral(lit)) {
            cnf.push_back(lit);
            nbVars = std::max(nbVars, std::abs(lit));
            nbCls += (lit == 0);
        }
        if (addAssumptionUnits) {
            while (parser.getNextAssumption(lit)) {
                cnf.push_back(lit);
                cnf.push_back(0);
                nbVars = std::max(nbVars, std::abs(lit));
                nbCls++;
            }
        }
        cnf.push_back(nbVars);
        cnf.push_back(nbCls);
        return cnf;
    }

    static std::vector<std::vector<int>> getClausesFromJobDescription(const JobDescription& _desc, bool addAssumptionUnits) {

        SerializedFormulaParser parser(Logger::getMainInstance(), _desc.getFormulaPayload(0),
            _desc.getFormulaPayloadSize(0));

        std::vector<std::vector<int>> clauses;
        int lit;
        std::vector<int> clause;
        while (parser.getNextLiteral(lit)) {
            if (lit == 0) clauses.push_back(std::move(clause));
            else clause.push_back(lit);
        }
        if (addAssumptionUnits)
            while (parser.getNextAssumption(lit))
                clauses.push_back({lit, 0});
        return clauses;
    }

    static void checkModel(const std::vector<int>& formula, const std::vector<int>& model) {
        bool clauseSatisfied = false;
        int clauseNo = 1;
	    std::ostringstream oss;
        for (int i = 0; i < formula.size()-2; i++) {
            int lit = formula[i];
            if (lit == 0) {
                // LOG(V3_VERB, "cl.%i: %s\n", clauseNo, oss.str().c_str());
                oss.str("");
                if (!clauseSatisfied) {
                    LOG(V0_CRIT, "[ERROR] Clause # %i at position %i not satisfied by model!\n", clauseNo, i);
                    abort();
                }
                clauseNo++;
                clauseSatisfied = false;
                continue;
            } 
            oss << lit << " ";
            assert(std::abs(lit) < model.size());
            int modelLit = model[std::abs(lit)];
            assert(modelLit == lit || modelLit == -lit);
            if (modelLit == lit) clauseSatisfied = true;
        }
        assert(formula[formula.size()-3] == 0);
    }

    static void writeFormula(const std::vector<int>& formula, const std::string& path) {
        std::ofstream ofsF(path);
        ofsF << "p cnf " << formula[formula.size() - 2] << " "
            << formula[formula.size() - 1] << "\n";
        for (int i = 0; i < formula.size()-2; i++) {
            ofsF << formula[i] << (formula[i] == 0 ? "\n" : " ");
        }
    }

    static void writeModel(const std::vector<int>& model, const std::string& path) {
        std::ofstream ofsM(path);
        ofsM << "v";
        for (int i = 1; i < model.size(); i++) ofsM << " " << model[i];
        ofsM << " 0\n";
    }
};
