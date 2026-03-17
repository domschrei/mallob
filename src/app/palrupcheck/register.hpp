
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/app_registry.hpp"
#include "app/palrupcheck/palrupcheck_job.hpp"
#include "data/job_processing_statistics.hpp"

void register_mallob_app_palrupcheck() {

    app_registry::AppEntry entry;
    entry.key = "PALRUPCHECK";
    entry.type = app_registry::AppEntry::DISTRIBUTED;
    entry.copyrightInformation = "by Dominik Schreiber and Ruben Götz\n";

    entry.optionChecker = [](const Parameters& params, auto& vec) {
        if (!params.regularProcessDistribution() || params.processesPerHost() == 0) {
            vec.push_back({
                &params.regularProcessDistribution,
                "PalRUP-check requires to specify a valid number of processes per host (-rpa=1 -pph=<k>)."
            });
        }
        if (!params.logDirectory.isSet()) {
            vec.push_back({
                &params.logDirectory,
                "PalRUP-check requires to specify a log directory."
            });
        }
        if (!params.proofDirectory.isSet()) {
            vec.push_back({
                &params.proofDirectory,
                "PalRUP-check requires to specify a proof directory."
            });
        }
        if (!params.palRupCheckWorkdir.isSet()) {
            vec.push_back({
                &params.palRupCheckWorkdir,
                "PalRUP-check requires to specify a shared working directory."
            });
        }
        return vec.empty();
    };

    entry.reader = [](const Parameters& params, const std::vector<std::string>& files, JobDescription& desc) {
        const std::string NC_DEFAULT_VAL = "BMMMKKK111";
        desc.setAppConfigurationEntry("__NV", NC_DEFAULT_VAL);
        desc.setAppConfigurationEntry("__NC", NC_DEFAULT_VAL);
        desc.setAppConfigurationEntry("__chkcnf", files[0]);
        desc.setAppConfigurationEntry("__chkproofdir", files[1]);
        desc.beginInitialization(0);
        desc.endInitialization();
        return true;
    };
    entry.creator = [](const Parameters& params, const Job::JobSetup& setup, AppMessageTable& table) -> Job* {
        return new PalrupCheckJob(params, setup, table);
    };
    entry.solutionFormatter = [](const Parameters& params, const JobResult& result, const JobProcessingStatistics& stat) {
        auto json = nlohmann::json::array();
        auto model = result.copySolution();
        json = std::move(model);
        //std::stringstream modelString;
        //modelString << "c parse_time " << stat.parseTime << "\n";
        //modelString << "c process_time " << stat.processingTime << "\n";
        //modelString << "c total_response_time " << stat.totalResponseTime << "\n";
        //json.push_back(modelString.str());
        return json;
    };

    app_registry::registerApplication(entry);
}
