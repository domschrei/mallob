
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/maxsat/maxsat_solver.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "parse/maxsat_reader.hpp"

void register_mallob_app_maxsat() {
    app_registry::registerClientSideApplication("MAXSAT",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return MaxSatReader(params, files.front()).read(desc);
        },
        // Client-side program
        [](const Parameters& params, APIConnector& api, JobDescription& desc) -> JobResult {
            MaxSatSolver solver(params, api, desc);
            return solver.solve();
        },
        // Job solution formatter
        [](const JobResult& result) {
            // TODO This is just the SAT model formatting so far. Anything else?
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
