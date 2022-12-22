
#pragma once

#include <functional>
#include <vector>
#include <string>
#include <iostream>

#include "util/params.hpp"
#include "data/job_description.hpp"
#include "app/job.hpp"
#include "util/tsl/robin_map.h"
#include "util/json.hpp"
#include "app/app_message_subscription.hpp"

namespace app_registry {

    typedef std::function<bool(const Parameters&, const std::vector<std::string>&, JobDescription&)> JobReader;
    typedef std::function<Job*(const Parameters&, const Job::JobSetup&, AppMessageTable&)> JobCreator;
    typedef std::function<nlohmann::json(const JobResult&)> JobSolutionFormatter;

    void registerApplication(const std::string& key,
        JobReader reader, 
        JobCreator creator, 
        JobSolutionFormatter resultPrinter
    );

    int getAppId(const std::string& key);
    const std::string& getAppKey(int appId);

    JobReader getJobReader(int appId);
    JobCreator getJobCreator(int appId);
    JobSolutionFormatter getJobSolutionFormatter(int appId);
}
