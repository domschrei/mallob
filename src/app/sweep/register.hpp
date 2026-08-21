
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "data/job_processing_statistics.hpp"
#include "sweep_job.hpp"

// void register_mallob_app_sweep() {
    // app_registry::registerApplication(
        // "SWEEP",
        // Job reader
        // [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
	        // LOG(V2_INFO, "Calling SAT Reader for job id %i  \n", desc.getId());
            // return SatReader(params, files).read(desc);
        // },
        // Job creator
        // [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
            // return new SweepJob(params, setup, table);
        // },
        // Job solution formatter
        // [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            // nlohmann::json j = result.copySolution();
            // return j;
        // }
    // );
// }

void register_mallob_app_sweep() {
    app_registry::AppEntry entry;
    entry.key = "SWEEP";
    entry.type = app_registry::AppEntry::DISTRIBUTED;
    entry.copyrightInformation = "by Dominik Schreiber and Niccolò Rigi-Luperti\n";

    entry.optionChecker = [](const Parameters& params, auto& vec) {
        return vec.empty();
    };

    entry.reader = [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
        auto reader = SatReader(params, files, params.forceIncrementalTrustedParser());
        auto res = reader.read(desc);
        return res;
    };
    entry.creator = [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
        return new SweepJob(params, setup, table);
    };
    entry.solutionFormatter = [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
        auto json = nlohmann::json::array();
        auto model = result.copySolution();
        json = std::move(model);
        return json;
    };
    app_registry::registerApplication(entry);
}