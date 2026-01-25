
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/proof/incremental_trusted_parser_store.hpp"
#include "data/job_processing_statistics.hpp"
#include "job/forked_sat_job.hpp"
#include "parse/sat_reader.hpp"
#include "robin_map.h"

void register_mallob_app_sat() {
    app_registry::registerApplication("SAT",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            auto reader = SatReader(params, files, params.forceIncrementalTrustedParser());
            // For incremental real-time checking, we need to inject a persisting
            // parser adapter instance into the new SatReader object or in turn
            // extract it after the first parsing.
            if (params.onTheFlyChecking() || params.forceIncrementalTrustedParser()) {
                auto lock = IncrementalTrustedParserStore::mtxMap.getLock();
                if (IncrementalTrustedParserStore::map.count(desc.getId()))
                    reader.setTrustedParser(IncrementalTrustedParserStore::map.at(desc.getId()));
            }
            auto res = reader.read(desc);
            if (params.onTheFlyChecking() || params.forceIncrementalTrustedParser()) {
                auto lock = IncrementalTrustedParserStore::mtxMap.getLock();
                if (!IncrementalTrustedParserStore::map.count(desc.getId()))
                    IncrementalTrustedParserStore::map[desc.getId()] = reader.getTrustedParser();
            }
            return res;
        },
        // Job creator
        [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
            return new ForkedSatJob(params, setup, table);
        },
        // Job solution formatter
        [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
            auto json = nlohmann::json::array();
            auto model = result.copySolution();
            if (result.result == SAT && params.compressModels()) {
                json = ModelStringCompressor::compress(model);
            } else {
                json = std::move(model);
            }
            //std::stringstream modelString;
            //modelString << "c parse_time " << stat.parseTime << "\n";
            //modelString << "c process_time " << stat.processingTime << "\n";
            //modelString << "c total_response_time " << stat.totalResponseTime << "\n";
            //json.push_back(modelString.str());
            return json;
        },
        // Resource cleaner
        [](const Parameters& params) {
            if (!params.proofDirectory().empty() && !params.injectProofData.isSet()) {
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
