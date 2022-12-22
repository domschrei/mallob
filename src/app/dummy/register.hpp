
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "dummy_job.hpp"
#include "dummy_reader.hpp"

void register_mallob_app_dummy() {
    app_registry::registerApplication("DUMMY",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return DummyReader::read(files, desc);
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
            return new DummyJob(params, setup, table);
        },
        // Job solution formatter
        [](const JobResult& result) {
            // An actual application would nicely format the result here ...
            return nlohmann::json();
        }
    );
}
