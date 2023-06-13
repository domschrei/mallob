
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "execution/qbf_job.hpp"
#include "parse/qbf_reader.hpp"

void register_mallob_app_qbf() {

    app_registry::registerApplication("QBF",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return QbfReader(params, files.front()).read(desc);
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
            return new QbfJob(params, setup, table);
        },
        // Job solution formatter
        [](const JobResult& result) {

            // TODO How does printing a solution work in QBF solving?

            auto json = nlohmann::json::array();
            std::stringstream modelString;
            int numAdded = 0;
            auto solSize = result.getSolutionSize();
            for (size_t x = 1; x < solSize; x++) {
                if (numAdded == 0) {
                    modelString << "v ";
                }
                modelString << std::to_string(result.getSolution(x)) << " ";
                numAdded++;
                bool done = x+1 == solSize;
                if (numAdded == 20 || done) {
                    if (done) modelString << "0";
                    modelString << "\n";
                    json.push_back(modelString.str());
                    modelString = std::stringstream();
                    numAdded = 0;
                }
            }
            return json;
        }
    );
}
