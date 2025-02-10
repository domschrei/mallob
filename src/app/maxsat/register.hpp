
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/maxsat/maxsat_solver.hpp"
#include "app/sat/job/sat_constants.h"
#include "data/job_description.hpp"
#include "data/job_processing_statistics.hpp"
#include "interface/api/api_connector.hpp"
#include "parse/maxsat_reader.hpp"

struct ClientSideMaxsatProgram : public app_registry::ClientSideProgram {
    std::unique_ptr<MaxSatSolver> solver;
    ClientSideMaxsatProgram(const Parameters& params, APIConnector& api, JobDescription& desc) :
        app_registry::ClientSideProgram(), solver(new MaxSatSolver(params, api, desc)) {
        function = [&]() {return solver->solve();};
    }
    virtual ~ClientSideMaxsatProgram() {}
};

void register_mallob_app_maxsat() {
    app_registry::registerClientSideApplication("MAXSAT",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return MaxSatReader(params, files.front()).read(desc);
        },
        // Client-side program
        [](const Parameters& params, APIConnector& api, JobDescription& desc) {
            return new ClientSideMaxsatProgram(params, api, desc);
        },
        // Job solution formatter
        [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            auto json = nlohmann::json::array();
            if (result.result != RESULT_SAT && result.result != RESULT_OPTIMUM_FOUND)
                return json;
            std::stringstream modelString;
            modelString << "c parse_time " << stat.parseTime << "\n";
            modelString << "c process_time " << stat.processingTime << "\n";
            modelString << "c total_response_time " << stat.totalResponseTime << "\n";
            auto solSize = result.getSolutionSize();
            assert(solSize > 0);
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
