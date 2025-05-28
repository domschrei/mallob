//
// Created by nicco on 5/28/25.
//

#pragma once

#include "util/params.hpp"
#include "interface/api/api_connector.hpp"




class SweepSolver {
private:
	const Parameters& _params; // configuration, cmd line arguments
	APIConnector& _api; // for submitting jobs to Mallob
	JobDescription& _desc; // contains our instance to solve and all metadata

public:
	// Initializes the solver instance and parses the description's formula.
	SweepSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
		_params(params), _api(api), _desc(desc) {

		LOG(V2_INFO, "Mallob client-side parallel Kissat-Equivalence-Sweeping, by Niccolò Rigi-Luperti & Dominik Schreiber\n");
		//do things...
	}

	// Perform parallelized equivalence sweeping
    JobResult solve() {
		JobResult r;
		//do things...
		return r;
	}
};
