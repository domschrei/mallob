
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/dummy/collectives_example_job.hpp"
#include "app/dummy/pointtopoint_example_job.hpp"
#include "data/job_processing_statistics.hpp"
#include "dummy_reader.hpp"

void register_mallob_app_dummy() {
    app_registry::registerApplication("DUMMY",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return DummyReader::read(files, desc);
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
            return new CollectivesExampleJob(params, setup, table);
            //return new PointToPointExampleJob(params, setup, table);
        },
        // Job solution formatter
        [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            nlohmann::json j = result.copySolution();
            return j;
        }
    );
}
