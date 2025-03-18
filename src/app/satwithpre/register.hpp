
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/satwithpre/sat_preprocess_solver.hpp"
#include "data/job_description.hpp"
#include "data/job_processing_statistics.hpp"
#include "interface/api/api_connector.hpp"
#include "app/sat/parse/sat_reader.hpp"

struct ClientSideSatProgram : public app_registry::ClientSideProgram {
    std::unique_ptr<SatPreprocessSolver> solver;
    ClientSideSatProgram(const Parameters& params, APIConnector& api, JobDescription& desc) :
        app_registry::ClientSideProgram(), solver(new SatPreprocessSolver(params, api, desc)) {
        function = [&]() {return solver->solve();};
    }
    virtual ~ClientSideSatProgram() {}
};

void register_mallob_app_satwithpre() {
    app_registry::registerClientSideApplication("SATWITHPRE",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return SatReader(params, files.front()).read(desc);
        },
        // Client-side program
        [](const Parameters& params, APIConnector& api, JobDescription& desc) {
            return new ClientSideSatProgram(params, api, desc);
        },
        // Job solution formatter
        [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            auto json = nlohmann::json::array();
            auto model = result.copySolution();
            if (result.result == RESULT_SAT && params.compressModels()) {
                json = ModelStringCompressor::compress(model);
            } else {
                json = std::move(model);
            }
            return json;
        }
    );
}
