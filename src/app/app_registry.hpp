
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

    typedef std::function<bool(const Parameters&, std::vector<std::pair<const Option*, std::string>>&)> OptionChecker;
    typedef std::function<bool(const Parameters&, const std::vector<std::string>&, JobDescription&)> JobReader;
    typedef std::function<Job*(const Parameters&, const Job::JobSetup&, AppMessageTable&)> JobCreator;
    typedef std::function<ClientSideProgram*(const Parameters&, APIConnector&, JobDescription&)> ClientSideProgramCreator;
    typedef std::function<void(const Parameters&, const JobResult&)> JobEpilog;
    typedef std::function<void(const Parameters&, JobResult&)> JobResultTransformer;
    typedef std::function<nlohmann::json(const Parameters&, const JobResult&, const JobProcessingStatistics&)> JobSolutionFormatter;
    typedef std::function<void(const Parameters&)> ResourceCleaner;

    // Each instance of AppEntry describes a certain application engine in Mallob, including
    // all relevant meta data and all of its internal logic.
    struct AppEntry {
        // Mandatory: All-caps, unique key for this application. 
        std::string key;
        // Optional. If not empty, end with "\n".
        // In case of multi-line content, use "\nc " for each new line.
        std::string copyrightInformation;
        // Mandatory: either DISTRIBUTED or CLIENT_SIDE.
        enum Type {DISTRIBUTED, CLIENT_SIDE} type;

        // Mandatory: Take a (possibly empty) number of input file paths and initialize
        // the specified job description. Return whether this was successful.
        JobReader reader;
        // Only for DISTRIBUTED apps - mandatory: Create an instance of a subclass of Job
        // that executes your distributed application.
        JobCreator creator;
        // Only for CLIENT_SIDE apps - mandatory: Create an instance of a subclass of ClientSideProgram
        // that executes your client-side application locally.
        ClientSideProgramCreator clientSideProgramCreator;
        // Mandatory: Return a JSON file as formatting of the provided job result (and statistics).
        JobSolutionFormatter solutionFormatter;

        // Optional: Check incoming parameters and return whether the configuration is viable.
        // For each issue, the checker can push an item to the provided vector, whereas each
        // item is a pair of a pointer to the offending option in the provided Parameters object
        // and a string describing the issue (single line, no line break at the end).
        OptionChecker optionChecker = [&](const Parameters&, auto& vec) {return true;};
        // Optional: Remove / delete / clean up any resources created by this application engine.
        ResourceCleaner cleaner = [](const Parameters&) {};
        // Optional: Hook to execute right before a result is reported for a certain job,
        // which could, e.g., encompass spawning a sub-job that does something with it.
        // The original job's result is reported only *after* the epilog has returned.
        std::optional<JobEpilog> epilog;
        // Optional: Hook that transforms an incoming result for a certain job before it is reported.
        std::optional<JobResultTransformer> jobResultTransformer;
    };

    void registerApplication(const AppEntry& appEntry);

    int getAppId(const std::string& key);
    const std::string& getAppKey(int appId);

    OptionChecker getOptionChecker(int appId);
    JobReader getJobReader(int appId);
    JobSolutionFormatter getJobSolutionFormatter(int appId);
    bool isClientSide(int appId);
    JobCreator getJobCreator(int appId);
    ClientSideProgramCreator getClientSideProgramCreator(int appId);
    std::optional<JobResultTransformer> getJobResultTransformer(int appId);
    std::optional<JobEpilog> getJobEpilog(int appId);
    std::vector<ResourceCleaner> getCleaners();
    std::string getCombinedCopyrightInformation();

    void checkAndOverrideProgramOptions(Parameters& params, JobDescription& desc);
}
