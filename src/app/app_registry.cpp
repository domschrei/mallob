
#include "app_registry.hpp"

#include <stdlib.h>
#include <iostream>
#include <memory>
#include <utility>

#include "robin_hash.h"
#include "robin_map.h"

namespace app_registry {

    // Anonymous / private namespace for implementation stuff
    namespace {
        struct AppEntry {
            std::string key;
            JobReader reader;
            bool isClientSide {false};
            JobCreator creator;
            ClientSideProgram clientSideProgram;
            JobSolutionFormatter solutionFormatter;
            ResourceCleaner cleaner;
        };

        std::vector<AppEntry> _app_entries;
        tsl::robin_map<std::string, int> _app_key_to_app_id;
    }

    // Registers an application engine for Mallob.
    // key: the identifier which users need to supply in the "application" field of JSON submissions.
    // reader: a lambda which reads a number of description files into a JobDescription object.
    // creator: a lambda which returns a new instance of a particular subclass of Job.
    // solutionFormatter: a lambda which transforms a found job result into 
    void registerApplication(const std::string& key,
        JobReader reader, 
        JobCreator creator,
        JobSolutionFormatter solutionFormatter,
        ResourceCleaner cleaner
    ) {
        int appId = _app_entries.size();
        _app_key_to_app_id[key] = appId;
        //std::cout << "Registered application id=" << appId << " key=" << key << std::endl;

        AppEntry entry;
        entry.key = key;
        entry.reader = reader;
        entry.creator = creator;
        entry.solutionFormatter = solutionFormatter;
        entry.cleaner = cleaner;
        _app_entries.push_back(std::move(entry));
    }

    void registerClientSideApplication(const std::string& key,
        JobReader reader,
        ClientSideProgram program,
        JobSolutionFormatter solutionFormatter,
        ResourceCleaner cleaner
    ) {
        int appId = _app_entries.size();
        _app_key_to_app_id[key] = appId;
        //std::cout << "Registered application id=" << appId << " key=" << key << std::endl;

        AppEntry entry;
        entry.key = key;
        entry.reader = reader;
        entry.isClientSide = true;
        entry.clientSideProgram = program;
        entry.solutionFormatter = solutionFormatter;
        entry.cleaner = cleaner;
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
        return _app_entries.at(appId).isClientSide;
    }
    JobCreator getJobCreator(int appId) {
        getAppKey(appId); // check existence
        return _app_entries.at(appId).creator;
    }
    ClientSideProgram getClientSideProgram(int appId) {
        getAppKey(appId); // check existence
        return _app_entries.at(appId).clientSideProgram;
    }

    std::vector<ResourceCleaner> getCleaners() {
        std::vector<ResourceCleaner> cleaners;
        for (auto& entry : _app_entries) cleaners.push_back(entry.cleaner);
        return cleaners;
    }
}

