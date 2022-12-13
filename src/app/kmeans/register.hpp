
#pragma once

#include "app/app_registry.hpp"
#include "kmeans_job.hpp"
#include "kmeans_reader.hpp"

void register_mallob_app_kmeans() {
    app_registry::registerApplication(
        "KMEANS",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return KMeansReader::read(files.front(), desc);
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup) -> Job* {
            return new KMeansJob(params, setup);
        },
        // Job solution formatter
        [](const JobResult& result) {
            // An actual application would nicely format the result here ...
            auto json = nlohmann::json::array();
            std::stringstream modelString;
            int numAdded = 0;
            size_t metadataOffset = 2;
            int raw = result.getSolution(0);
            int countClusters = *((float*)&raw);
            raw = result.getSolution(1);
            int dimension = *((float*)&raw);
            LOG(V5_DEBG, "0: %i\n", countClusters);
            LOG(V5_DEBG, "1: %i\n", dimension);
            size_t solSize = countClusters * dimension;
            for (size_t x = metadataOffset; x < solSize + metadataOffset; x++) {
                if (numAdded % dimension == 0) {
                    modelString << "c ";
                }
                raw = result.getSolution(x);
                modelString << std::to_string(*((float*)&raw)) << " ";
                numAdded++;
                if (numAdded % dimension == 0) {
                    modelString << "\n";
                }
            }
            //LOG(V5_DEBG, "Solution: \n%s\n",modelString.str().c_str());
            json.push_back(modelString.str());
            modelString = std::stringstream();
            return json;
        });
}
