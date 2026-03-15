
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
        std::function<JobResult()> function;
        virtual ~ClientSideProgram() {}
    };

    typedef std::function<bool(const Parameters&, const std::vector<std::string>&, JobDescription&)> JobReader;
    typedef std::function<Job*(const Parameters&, const Job::JobSetup&, AppMessageTable&)> JobCreator;
    typedef std::function<ClientSideProgram*(const Parameters&, APIConnector&, JobDescription&)> ClientSideProgramCreator;
    typedef std::function<void(const Parameters&, JobResult&)> JobResultTransformer;
    typedef std::function<void(const Parameters&, const JobResult&)> JobEpilog;
    typedef std::function<nlohmann::json(const Parameters&, const JobResult&, const JobProcessingStatistics&)> JobSolutionFormatter;
    typedef std::function<void(const Parameters&)> ResourceCleaner;

    struct AppEntry {
        std::string key;
        // Can remain empty. If not empty, end with "\n".
        // In case of multi-line content, use "\nc " for each new line.
        std::string copyrightInformation;
        enum Type {DISTRIBUTED, CLIENT_SIDE} type;

        JobReader reader;
        JobCreator creator;
        ClientSideProgramCreator clientSideProgramCreator;
        JobSolutionFormatter solutionFormatter;
        ResourceCleaner cleaner = [](const Parameters&) {};
        std::optional<JobEpilog> epilog;
        std::optional<JobResultTransformer> jobResultTransformer;
    };

    void registerApplication(const AppEntry& appEntry);

    int getAppId(const std::string& key);
    const std::string& getAppKey(int appId);

    JobReader getJobReader(int appId);
    JobSolutionFormatter getJobSolutionFormatter(int appId);
    bool isClientSide(int appId);
    JobCreator getJobCreator(int appId);
    ClientSideProgramCreator getClientSideProgramCreator(int appId);
    std::optional<JobResultTransformer> getJobResultTransformer(int appId);
    std::optional<JobEpilog> getJobEpilog(int appId);
    std::vector<ResourceCleaner> getCleaners();
    std::string getCombinedCopyrightInformation();

    void overrideProgramOptions(Parameters& params, JobDescription& desc);
}
