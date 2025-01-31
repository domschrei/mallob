
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
        [](const Parameters& params, const JobResult& result) {
            auto json = nlohmann::json::array();
            std::stringstream modelString;
            auto solSize = result.getSolutionSize();
            int modelSize = result.getSolution(0);
            assert(modelSize > 0);
            assert(solSize == modelSize + 2);
            int costAsTwoInts[2] = {result.getSolution(modelSize), result.getSolution(modelSize+1)};
            unsigned long cost = * (unsigned long*) costAsTwoInts;
            modelString << "o " << cost << "\nv ";
            for (size_t x = 1; x < modelSize; x++) {
                int lit = result.getSolution(x);
                assert(std::abs(lit) == x);
                modelString << std::to_string(lit > 0 ? 1 : 0);
            }
            modelString << "\n";
            json.push_back(modelString.str());
            return json;
        }
    );
}
