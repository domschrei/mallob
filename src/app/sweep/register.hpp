
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "data/job_processing_statistics.hpp"
#include "sweep_reader.hpp"
#include "sweep_solver.hpp"


struct ClientSideSweepProgram : public app_registry::ClientSideProgram {
    std::unique_ptr<SweepSolver> solver;
    ClientSideSweepProgram(const Parameters& params, APIConnector& api, JobDescription& desc) :
        app_registry::ClientSideProgram(), solver(new SweepSolver(params, api, desc)) {
        function = [&]() {return solver->solve();};
    }
    virtual ~ClientSideSweepProgram() {}
};




void register_mallob_app_sweep() {
    app_registry::registerClientSideApplication(
        "SWEEP",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return SweepReader::read(files, desc);
        },
        // Client-side program
        [](const Parameters& params, APIConnector& api, JobDescription& desc) {
            return new ClientSideSweepProgram(params, api, desc);
        },
        // Job solution formatter
        [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            // format result...
            return nlohmann::json();
        }
    );
}
