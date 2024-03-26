
#pragma once

#include <functional>                        // for function
#include <string>                            // for string
#include <vector>                            // for vector
#include "app/app_message_subscription.hpp"  // for AppMessageTable
#include "app/job.hpp"                       // for Job
#include "util/json.hpp"                     // for json

class JobDescription;
class Parameters;
struct JobResult;

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
