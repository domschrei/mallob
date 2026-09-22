
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/satwithpre/satwithpre_solver.hpp"
#include "data/job_description.hpp"
#include "data/job_processing_statistics.hpp"
#include "interface/api/api_connector.hpp"
#include "app/sat/parse/sat_reader.hpp"

struct ClientSideSatProgram : public app_registry::ClientSideProgram {
    std::unique_ptr<SatWithPreSolver> solver;
    ClientSideSatProgram(const Parameters& params, APIConnector& api, JobDescription& desc) :
        app_registry::ClientSideProgram(), solver(new SatWithPreSolver(params, api, desc)) {
        function = [&]() {return solver->solve();};
    }
    virtual ~ClientSideSatProgram() {}
};

void register_mallob_app_satwithpre() {

    app_registry::AppEntry entry;
    entry.key = "SATWITHPRE";
    entry.type = app_registry::AppEntry::CLIENT_SIDE;

    entry.copyrightInformation = "\nc Featuring Satsuma by Markus Anders\nc Featuring interface and proof production code by Anna Görth\n";

    entry.optionChecker = [](const Parameters& params, auto& vec) {
        if (params.savePreprocessingProofs() && !params.proofDirectory.isSet()) {
            vec.push_back({&params.savePreprocessingProofs,
                "Preprocessing proof saving (-prepro-proofs) requires a proof directory (-proof-dir)."
            });
        }
        return vec.empty();
    };

    entry.reader = [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
        return SatReader(params, files).read(desc);
    };

    entry.clientSideProgramCreator = [](const Parameters& params, APIConnector& api, JobDescription& desc) {
        return new ClientSideSatProgram(params, api, desc);
    };

    entry.solutionFormatter = [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
        auto json = nlohmann::json::array();
        auto model = result.copySolution();
        if (result.result == RESULT_SAT && params.compressModels()) {
            json = ModelStringCompressor::compress(model);
        } else {
            json = std::move(model);
        }
        return json;
    };

    app_registry::registerApplication(entry);
}
