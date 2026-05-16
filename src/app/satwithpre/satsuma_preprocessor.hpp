
#pragma once

#include <vector>

#include "app/satwithpre/sat_preprocess_actor.hpp"
#include "data/job_description.hpp"
#include "util/params.hpp"
#include "util/sys/thread_pool.hpp"
#include <future>
#include <sys/stat.h>
#include <unistd.h>

#if MALLOB_USE_SATSUMA == 1
#include "ISatsumaPreprocessor.h"
#include "ICnf2wl.h"
#endif

class SatsumaPreprocessor : public SatPreprocessActor {

public:
    SatsumaPreprocessor(const Parameters& params, const JobDescription& desc, const std::string& name, std::vector<int>&& formula) :
        SatPreprocessActor(params, name, std::move(formula)) {}
    ~SatsumaPreprocessor() {}

    void preprocessAsync() override {
        _fut_prepro = ProcessWideThreadPool::get().addTask([&]() {
#if MALLOB_USE_SATSUMA == 1
            CoreAllocator::Allocation ca(1);
            auto _satsuma_preprocessor = satsuma::create_preprocessor();
            std::unique_ptr<satsuma::ICnf2wl> formula = satsuma::create_cnf2wl();
            loadFormulaToCnf2wl(*formula);
            _satsuma_preprocessor->set_save_as_Formula(true);
            // hier vielleicht echten logger mit bestimmter verbosity
            std::ofstream dev_null("/dev/null");
            _satsuma_preprocessor->set_log_output(&dev_null);
            _satsuma_preprocessor->preprocess(*formula);
            _output_cnf = std::move(_satsuma_preprocessor->extractPreprocessedFormula());
            _result = SIMPLIFIED;
#else
            _result = NONE;
#endif
        });
    }

    void reconstructSolution(std::vector<int>& sol) override {
        sol.resize(nbInputVars() + 1);
    }

private:
#if MALLOB_USE_SATSUMA == 1
    void loadFormulaToCnf2wl(satsuma::ICnf2wl& result) {
        result.reserve(nbInputVars(), nbInputClauses());
		std::vector<int> construct_clause;
        for (int i = 0; i+2 < _input_cnf.size(); i++) {
            int lit = _input_cnf[i];
			if (lit != 0) {
				construct_clause.push_back(lit);
			} else {
				result.add_clause(construct_clause); 
				construct_clause.clear();
			}
        }
	}
#endif
};
