
#pragma once

#include "app/mus/mus_solver.hpp"
#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/sat/parse/sat_reader.hpp"
#include "data/job_description.hpp"
#include "data/job_processing_statistics.hpp"
#include "interface/api/api_connector.hpp"


struct ClientSideMusProgram : public app_registry::ClientSideProgram {
    std::unique_ptr<MusSolver> solver;
    ClientSideMusProgram(const Parameters& params, APIConnector& api, JobDescription& desc) :
        app_registry::ClientSideProgram(), solver(new MusSolver(params, api, desc)) {
        function = [&]() {return solver->solve();};
    }
    virtual ~ClientSideMusProgram() {}
};

void register_mallob_app_mus() {

    app_registry::AppEntry entry;
    entry.key = "MUS";
    entry.type = app_registry::AppEntry::CLIENT_SIDE;
    entry.copyrightInformation = "by Niccolò Rigi-Luperti and Dominik Schreiber\n";

    entry.reader = [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
        auto reader = SatReader(params, files);
        auto res = reader.read(desc);
        return res;
    };
    entry.clientSideProgramCreator = [](const Parameters& params, APIConnector& api, JobDescription& desc) {
        return new ClientSideMusProgram(params, api, desc);
    };
    entry.solutionFormatter = [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
        auto json = nlohmann::json::array();
        auto model = result.copySolution();
        json = std::move(model);
        return json;
    };

    app_registry::registerApplication(entry);
}
