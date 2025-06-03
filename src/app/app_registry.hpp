
#pragma once

#include <functional>                        // for function
#include <string>                            // for string
#include <vector>                            // for vector
#include "app/app_message_subscription.hpp"  // for AppMessageTable
#include "app/job.hpp"                       // for Job
#include "data/job_processing_statistics.hpp"
#include "interface/api/api_connector.hpp"
#include "util/json.hpp"                     // for json

class JobDescription;
class Parameters;
struct JobResult;

namespace app_registry {

    struct ClientSideProgram {
        //function is often mapped to solver->solve()
        std::function<JobResult()> function;
        virtual ~ClientSideProgram() {}
    };

    typedef std::function<bool(const Parameters&, const std::vector<std::string>&, JobDescription&)> JobReader;
    typedef std::function<Job*(const Parameters&, const Job::JobSetup&, AppMessageTable&)> JobCreator;
    typedef std::function<ClientSideProgram*(const Parameters&, APIConnector&, JobDescription&)> ClientSideProgramCreator;
    typedef std::function<nlohmann::json(const Parameters&, const JobResult&, const JobProcessingStatistics&)> JobSolutionFormatter;
    typedef std::function<void(const Parameters&)> ResourceCleaner;

    void registerApplication(const std::string& key,
        JobReader reader, 
        JobCreator creator, 
        JobSolutionFormatter resultPrinter,
        ResourceCleaner cleaner = [](const Parameters&) {}
    );
    void registerClientSideApplication(const std::string& key,
        JobReader reader,
        ClientSideProgramCreator programCreator,
        JobSolutionFormatter solutionFormatter,
        ResourceCleaner cleaner = [](const Parameters&) {}
    );

    int getAppId(const std::string& key);
    const std::string& getAppKey(int appId);

    JobReader getJobReader(int appId);
    JobSolutionFormatter getJobSolutionFormatter(int appId);
    bool isClientSide(int appId);
    JobCreator getJobCreator(int appId);
    ClientSideProgramCreator getClientSideProgramCreator(int appId);
    std::vector<ResourceCleaner> getCleaners();
}
