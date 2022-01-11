
#include "util/assert.hpp"

#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
using namespace Mallob;

struct HordeLearner : public CaDiCaL::Learner {

private:
	const SolverSetup& _setup;
	LearnedClauseCallback _callback;
	std::vector<int> _curr_clause;
	int _glue_limit;
	unsigned long _num_produced;
	
public:
	HordeLearner(const SolverSetup& setup) : _setup(setup), _glue_limit(_setup.strictLbdLimit) {}
	~HordeLearner() override {}

  	bool learning(int size) override {
		return size > 0 && size <= _setup.strictClauseLengthLimit;
	}

	void learn(int lit) override {
		if (lit != 0) {
			// Received a literal
			_curr_clause.push_back(lit);
		} else {
			// Received a zero - clause is finished
			_num_produced++;

			if (_curr_clause.empty()) {
				_setup.logger->log(V4_VVER, "Received empty clause!\n");
				return;
			}

			bool eligible = true;
			if (_curr_clause.size() > 1) {
				assert(_curr_clause.size() >= 3); // glue value plus at least two literals
				// Non-unit clause: First integer is glue value
				int& glue = _curr_clause[0];
				if (glue > _glue_limit) eligible = false;
				assert(glue > 0);
			}
			
			// Export clause (if eligible), reset current clause
			if (eligible) {
				if (_curr_clause.size() > 1) {
					_callback(Clause{_curr_clause.data()+1, (int)_curr_clause.size()-1, _curr_clause[0]}, 
						_setup.localId);
				} else {
					_callback(Clause{_curr_clause.data(), 1, 1}, 
						_setup.localId);
				}
			} 
			_curr_clause.clear();
		}
	}

    void setCallback(const LearnedClauseCallback& callback) {
        _callback = callback;
    }

	unsigned long getNumProduced() const {
		return _num_produced;
	}
};