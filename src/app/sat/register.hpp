
#pragma once

#include "app/app_registry.hpp"
#include "job/forked_sat_job.hpp"
#include "job/threaded_sat_job.hpp"
#include "parse/sat_reader.hpp"

void register_mallob_app_sat() {
    app_registry::registerApplication("SAT",
        // Job reader
        [](const std::vector<std::string>& files, JobDescription& desc) {
            return SatReader(files.front()).read(desc);
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup) -> Job* {
            if (params.applicationSpawnMode() == "fork") {
                return new ForkedSatJob(params, setup);
            } else {
                return new ThreadedSatJob(params, setup);
            }
        },
        // Job solution formatter
        [](const JobResult& result) {
            auto json = nlohmann::json::array();
            std::stringstream modelString;
            int numAdded = 0;
            auto solSize = result.getSolutionSize();
            for (size_t x = 1; x < solSize; x++) {
                if (numAdded == 0) {
                    modelString << "v ";
                }
                modelString << std::to_string(result.getSolution(x)) << " ";
                numAdded++;
                bool done = x+1 == solSize;
                if (numAdded == 20 || done) {
                    if (done) modelString << "0";
                    modelString << "\n";
                    json.push_back(modelString.str());
                    modelString = std::stringstream();
                    numAdded = 0;
                }
            }
            return json;
        }
    );
}
