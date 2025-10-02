
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "data/job_processing_statistics.hpp"
#include "sweep_job.hpp"
#include "sweep_reader.hpp"

void register_mallob_app_sweep() {
    app_registry::registerApplication(
        "SWEEP",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return SweepReader::read(files, desc);
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
            return new SweepJob(params, setup, table);
        },
        // Job solution formatter
        [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            nlohmann::json j = result.copySolution();
            return j;
        }
    );
}
