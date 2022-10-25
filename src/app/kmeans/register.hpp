
#pragma once

#include "app/app_registry.hpp"
#include "kmeans_job.hpp"
#include "kmeans_reader.hpp"

void register_mallob_app_kmeans() {
    app_registry::registerApplication("KMEANS",
        // Job reader
        [](const std::vector<std::string>& files, JobDescription& desc) {
            return KMeansReader::read(files.front(), desc);
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup) -> Job* {
            return new KMeansJob(params, setup);
        },
        // Job solution formatter
        [](const JobResult& result) {
            // An actual application would nicely format the result here ...
            return nlohmann::json();
        }
    );
}
