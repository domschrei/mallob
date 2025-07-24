
#pragma once

#include "app/app_registry.hpp"
#include "app/sat/job/sat_constants.h"
#include "data/job_description.hpp"
#include "data/job_processing_statistics.hpp"
#include "interface/api/api_connector.hpp"

#include "app/smt/bitwuzla_solver.hpp"
#include "util/static_store.hpp"

struct ClientSideSMTProgram : public app_registry::ClientSideProgram {
    std::unique_ptr<BitwuzlaSolver> solver;
    ClientSideSMTProgram(const Parameters& params, APIConnector& api, JobDescription& desc, const std::string& problemFile) :
        app_registry::ClientSideProgram(), solver(new BitwuzlaSolver(params, api, desc, problemFile)) {
        function = [&]() {return solver->solve();};
    }
    virtual ~ClientSideSMTProgram() {}
};

void register_mallob_app_smt() {
    app_registry::registerClientSideApplication("SMT",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            const std::string NC_DEFAULT_VAL = "BMMMKKK111";
            desc.setAppConfigurationEntry("__NV", NC_DEFAULT_VAL);
            desc.setAppConfigurationEntry("__NC", NC_DEFAULT_VAL);
            desc.beginInitialization(0);
            StaticStore<std::string>::insert("smt-jobdesc-#" + std::to_string(desc.getId()), files[0]);
            desc.endInitialization();
            return true;
        },
        // Client-side program
        [](const Parameters& params, APIConnector& api, JobDescription& desc) {
            std::string problemFile = StaticStore<std::string>::extract("smt-jobdesc-#" + std::to_string(desc.getId()));
            return new ClientSideSMTProgram(params, api, desc, problemFile);
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
