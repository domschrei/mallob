
#pragma once

#include "app/app_registry.hpp"
#include "app/sat/data/definitions.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/parse/sat_reader.hpp"
#include "app/satcnc/cnc_controller.hpp"
#include "data/job_description.hpp"
#include "data/job_processing_statistics.hpp"
#include "interface/api/api_connector.hpp"

struct ClientSideSatCncProgram : public app_registry::ClientSideProgram {
    CncController cnc;
    ClientSideSatCncProgram(const Parameters& params, JobDescription& desc) :
        app_registry::ClientSideProgram(), cnc(params, desc) {
        function = [&]() {return cnc.solve();};
    }
    virtual ~ClientSideSatCncProgram() {}
};

void register_mallob_app_satcnc() {
    app_registry::registerClientSideApplication("SATCNC",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return SatReader(params, files).read(desc);
        },
        // Client-side program
        [](const Parameters& params, APIConnector& api, JobDescription& desc) {
            return new ClientSideSatCncProgram(params, desc);
        },
        // Job solution formatter
        [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            auto json = nlohmann::json::array();
            auto model = result.copySolution();
            if (result.result == SAT && params.compressModels()) {
                json = ModelStringCompressor::compress(model);
            } else {
                json = std::move(model);
            }
            return json;
        }
    );
}
