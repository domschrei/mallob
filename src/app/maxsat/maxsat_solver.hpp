
#pragma once

#include "app/maxsat/sat_job_stream.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include <list>
#include <unistd.h>

class MaxSatSolver {

private:
    const Parameters& _params;
    APIConnector& _api;
    JobDescription& _desc;

    std::string _username;

    std::list<std::unique_ptr<SatJobStream>> _job_streams;

public:
    MaxSatSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc), _username("maxsat#" + std::to_string(_desc.getId())) {}

    JobResult solve() {

        // Little bit of boilerplate to initialize a new incremental SAT job stream
        _job_streams.emplace_back(new SatJobStream(_params, _api, _username, (int)_job_streams.size(), true));
        auto stream = _job_streams.back().get();

        // We can now submit a sequence of increments of this job to Mallob
        int lb = 0, ub; // bounds
        while (lb != ub) {
            // TODO
            std::vector<int> clauses; // whitespace-separated and -terminated
            std::vector<int> assumptions;

            // Submit a SAT job increment and wait until it returns
            stream->submitNext(std::move(clauses), std::move(assumptions));
            while (stream->isPending()) usleep(1000 * 10); // 10 ms

            // Retrieve the result
            auto result = std::move(stream->getResult());
            const int resultCode = result["result"]["resultcode"];
            if (resultCode == 20) {
                // UNSAT
                // TODO update lb (and prepare for next encoding if needed)
            } else if (resultCode == 10) {
                // SAT
                // TODO update ub (and prepare for next encoding if needed)
                // TODO store the solution if it is the best one so far
                std::vector<int> solution = std::move(result["result"]["solution"]);
            } else {
                // UNKNOWN or something else - an error in this case since we didn't cancel the job
                abort();
            }
        }

        // TODO construct proper job result
        JobResult res;
        return res;
    }
};
