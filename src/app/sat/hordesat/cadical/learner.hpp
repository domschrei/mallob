#include "app/sat/hordesat/cadical/cadical.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"

struct HordeLearner : public CaDiCaL::Learner {
	HordeLearner(PortfolioSolverInterface &portfolio) : _portfolio(portfolio) {}
	~HordeLearner() override {}

  	bool learning(int size) override {
		return size <= _glueLimit;
	}

	void learn(int lit) override {
		if (lit) {
			// Received a literal
			_currClause.push_back(lit);
		} else {
			// Received a zero - clause is finished

			int glue = _currClause.size();

			// Add (glue + 1) to the front of the clause if not at unit
			if (glue != 1)
		        _currClause.insert(_currClause.begin(), glue + 1);

			_callback->processClause(_currClause, _portfolio.getLocalId());
		}
	}

    void incGlueLimit() {
        if (_glueLimit < 8) _glueLimit++;
    }

    void setCallback(LearnedClauseCallback *callback) {
        _callback = callback;
    }

	private:
        int _glueLimit = 2;
        
		LearnedClauseCallback *_callback;

        PortfolioSolverInterface &_portfolio;

		vector<int> _currClause;
};