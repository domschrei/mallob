#ifndef MALLOB_MUSENGINE_HPP
#define MALLOB_MUSENGINE_HPP

#include "app/sat/execution/scatterengine.hpp"

class MusEngine : public ScatterEngine {
public:
	static constexpr Jobtype KICK_CANDIDATE_CLAUSE{0};
    static constexpr Jobtype ROTATE_MODEL_AROUND_CLAUSE{1};

public:
	MusEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface);
	~MusEngine() = default;

    int getNumSolvers(int numThreads) override;
	std::string getSolverChoicesStr() override;
	std::vector<JobToSolverMapping> getJobToSolverMappingDefinitions() override;

};

#endif //MALLOB_MUSENGINE_HPP
