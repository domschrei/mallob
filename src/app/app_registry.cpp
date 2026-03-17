
#include "app_registry.hpp"

#include <stdlib.h>
#include <iostream>
#include <memory>
#include <utility>

#include "robin_hash.h"
#include "robin_map.h"
#include "util/logger.hpp"
#include "util/string_utils.hpp"

namespace app_registry {

    // Anonymous / private namespace for implementation stuff
    namespace {
        std::vector<AppEntry> _app_entries;
        tsl::robin_map<std::string, int> _app_key_to_app_id;
    }

    // Registers an application engine for Mallob.
    // key: the identifier which users need to supply in the "application" field of JSON submissions.
    // reader: a lambda which reads a number of description files into a JobDescription object.
    // creator: a lambda which returns a new instance of a particular subclass of Job.
    // solutionFormatter: a lambda which transforms a found job result into 
    void registerApplication(const AppEntry& entry) {

        int appId = _app_entries.size();
        _app_key_to_app_id[entry.key] = appId;
        //std::cout << "Registered application id=" << appId << " key=" << key << std::endl;

        _app_entries.push_back(std::move(entry));
    }

    int getAppId(const std::string& key) {
        if (!_app_key_to_app_id.count(key)) {
            return -1;
        }
        return _app_key_to_app_id.at(key);
    }
    const std::string& getAppKey(int appId) {
        if (appId < 0 || appId >= _app_entries.size()) {
            std::cout << "CRITICAL ERROR: No application with ID \"" << appId << "\" registered!" << std::endl;
            abort();
        }
        return _app_entries.at(appId).key;
    }

    OptionChecker getOptionChecker(int appId) {
        getAppKey(appId); // check existence
        return _app_entries.at(appId).optionChecker;
    }
    JobReader getJobReader(int appId) {
        getAppKey(appId); // check existence
        return _app_entries.at(appId).reader;
    }
    JobSolutionFormatter getJobSolutionFormatter(int appId) {
        getAppKey(appId); // check existence
        return _app_entries.at(appId).solutionFormatter;
    }

    bool isClientSide(int appId) {
        getAppKey(appId); // check existence
        return _app_entries.at(appId).type == AppEntry::CLIENT_SIDE;
    }
    JobCreator getJobCreator(int appId) {
        getAppKey(appId); // check existence
        return _app_entries.at(appId).creator;
    }
    ClientSideProgramCreator getClientSideProgramCreator(int appId) {
        getAppKey(appId); // check existence
        return _app_entries.at(appId).clientSideProgramCreator;
    }
    std::optional<JobResultTransformer> getJobResultTransformer(int appId) {
        getAppKey(appId);
        return _app_entries.at(appId).jobResultTransformer;
    }
    std::optional<JobEpilog> getJobEpilog(int appId) {
        getAppKey(appId);
        return _app_entries.at(appId).epilog;
    }

    std::vector<ResourceCleaner> getCleaners() {
        std::vector<ResourceCleaner> cleaners;
        for (auto& entry : _app_entries) cleaners.push_back(entry.cleaner);
        return cleaners;
    }

    std::string getCombinedCopyrightInformation() {
        std::string out;
        for (auto& entry : _app_entries) if (!entry.copyrightInformation.empty()) {
            out += "c \nc Application module " + entry.key + ": " + entry.copyrightInformation;
        }
        out += "c \n";
        return out;
    }

    void checkAndOverrideProgramOptions(Parameters& params, JobDescription& desc) {

        int appId = desc.getApplicationId();
        OptionChecker chk = getOptionChecker(appId);
        std::vector<std::pair<const Option*, std::string>> problems;
        if (!chk(params, problems)) {
            LOG(V0_CRIT, "[ERROR] Invalid configuration for #%i (application %s):\n",
                desc.getId(), getAppKey(appId).c_str());
            for (auto& [opt, msg] : problems) {
                LOG(V0_CRIT, "[ERROR]  * Option -%s%s : %s\n",
                    opt->id.c_str(), opt->hasLongOption() ? (", -" + opt->longid).c_str() : "",
                    msg.c_str());
            }
            LOG(V0_CRIT, "[ERROR] Mallob will now abort.\n");
            abort();
        }

        const auto appConf = desc.getAppConfiguration();
        if (!appConf.map.count("options")) return;

        std::string optOverrides = appConf.map.at("options");
        std::replace(optOverrides.begin(), optOverrides.end(), '&', ' ');
        std::istringstream buffer(optOverrides);
        std::vector<std::string> args {std::istream_iterator<std::string>(buffer),
                                std::istream_iterator<std::string>()};
        params.init(args); // override
    }
}

