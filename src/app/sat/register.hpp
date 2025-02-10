
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "data/job_processing_statistics.hpp"
#include "job/forked_sat_job.hpp"
#include "parse/sat_reader.hpp"

void register_mallob_app_sat() {
    app_registry::registerApplication("SAT",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            return SatReader(params, files.front()).read(desc);
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
            return new ForkedSatJob(params, setup, table);
        },
        // Job solution formatter
        [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            auto json = nlohmann::json::array();
            /*std::stringstream modelString;
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
            */
            auto model = result.copySolution();
            if (result.result == SAT && params.compressModels()) {
                json = ModelStringCompressor::compress(model);
            } else {
                json = std::move(model);
            }
            return json;
        },
        // Resource cleaner
        [](const Parameters& params) {
            if (!params.proofDirectory().empty()) {
                for (auto file : FileUtils::glob(params.proofDirectory() + "/proof#*/")) {
                    FileUtils::rmrf(file);
                }
            }
            if (!params.extMemDiskDirectory().empty()) {
                for (auto file : FileUtils::glob(params.extMemDiskDirectory() + "/disk.*.*")) {
                    FileUtils::rmrf(file);
                }
            }
        }
    );
}
