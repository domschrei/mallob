
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/proof/incremental_trusted_parser_store.hpp"
#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "data/job_processing_statistics.hpp"
#include "interface/api/api_registry.hpp"
#include "job/forked_sat_job.hpp"
#include "parse/sat_reader.hpp"
#include "robin_map.h"
#include "util/logger.hpp"
#include "util/static_store.hpp"
#include "util/sys/thread_pool.hpp"
#include <ios>

void register_mallob_app_sat() {
    app_registry::registerApplication("SAT",
        // Job reader
        [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
            auto reader = SatReader(params, files, params.forceIncrementalTrustedParser());
            // For incremental real-time checking, we need to inject a persisting
            // parser adapter instance into the new SatReader object or in turn
            // extract it after the first parsing.
            bool incParser = (params.onTheFlyChecking() && params.onTheFlyCheckIncremental())
                || params.forceIncrementalTrustedParser();
            if (incParser) {
                auto lock = IncrementalTrustedParserStore::mtxMap.getLock();
                if (IncrementalTrustedParserStore::map.count(desc.getId()))
                    reader.setTrustedParser(IncrementalTrustedParserStore::map.at(desc.getId()));
            }
            auto res = reader.read(desc);
            if (incParser) {
                auto lock = IncrementalTrustedParserStore::mtxMap.getLock();
                if (!IncrementalTrustedParserStore::map.count(desc.getId()))
                    IncrementalTrustedParserStore::map[desc.getId()] = reader.getTrustedParser();
            }
            if (params.palRup())
                StaticStore<std::string>::insert("cnf-#" + std::to_string(desc.getId()), files[0]);
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
        },
        // Epilog
        [](const Parameters& params, const JobResult& result) {
            auto cnfPathOpt = StaticStore<std::string>::extractMaybe("cnf-#" + std::to_string(result.id));
            if (cnfPathOpt.has_value() && params.palRupCheck() && result.result == UNSAT) {
                auto cnfPath = cnfPathOpt.value();
                nlohmann::json jsonJob = {
                    {"user", "internal"},
                    {"name", "palrupchk-" + std::to_string(result.id)},
                    {"files",
                        {cnfPath, 
                        params.proofDirectory() + "/proof#" + std::to_string(result.id) + "/"}},
                    {"priority", 1.000},
                    {"application", "PALRUPCHECK"},
                    {"incremental", false}
                };
                auto jsonPalrupResult = APIRegistry::get().processBlocking(jsonJob);
            }
        },
        // Job result transformer
        [](const Parameters& params, JobResult& res) {
            auto sol = res.copySolution();
            Witness w = Witness::extractWitnessFromSolutionVector(sol);
            res.setSolutionToSerialize(sol.data(), sol.size());
            if (w.valid() && params.logDirectory.isSet()) {
                std::ofstream ofs(params.logDirectory() + "/witness.#" + std::to_string(res.id), ios_base::app);
                ofs << w.cidx << " " << w.result << " "
                    << Logger::dataToHexStr((const u8*) w.data, SIG_SIZE_BYTES);
                if (!w.asmpt.empty()) {
                    for (int a : w.asmpt) ofs << " " << a;
                }
                ofs << "\n";
            }
        }
    );
}
