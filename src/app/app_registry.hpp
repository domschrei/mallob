
#pragma once

#include <functional>                        // for function
#include <string>                            // for string
#include <vector>                            // for vector
#include "app/app_message_subscription.hpp"  // for AppMessageTable
#include "app/job.hpp"                       // for Job
#include "interface/api/api_connector.hpp"
#include "util/json.hpp"                     // for json

class JobDescription;
class Parameters;
struct JobResult;

namespace app_registry {

    typedef std::function<bool(const Parameters&, const std::vector<std::string>&, JobDescription&)> JobReader;
    typedef std::function<Job*(const Parameters&, const Job::JobSetup&, AppMessageTable&)> JobCreator;
    typedef std::function<JobResult(const Parameters&, APIConnector&, JobDescription&)> ClientSideProgram;
    typedef std::function<nlohmann::json(const JobResult&)> JobSolutionFormatter;
    typedef std::function<void(const Parameters&)> ResourceCleaner;

    void registerApplication(const std::string& key,
        JobReader reader, 
        JobCreator creator, 
        JobSolutionFormatter resultPrinter,
        ResourceCleaner cleaner = [](const Parameters&) {}
    );
    void registerClientSideApplication(const std::string& key,
        JobReader reader,
        ClientSideProgram program,
        JobSolutionFormatter solutionFormatter,
        ResourceCleaner cleaner = [](const Parameters&) {}
    );

    int getAppId(const std::string& key);
    const std::string& getAppKey(int appId);

    JobReader getJobReader(int appId);
    JobSolutionFormatter getJobSolutionFormatter(int appId);
    bool isClientSide(int appId);
    JobCreator getJobCreator(int appId);
    ClientSideProgram getClientSideProgram(int appId);
    std::vector<ResourceCleaner> getCleaners();
}
