
#pragma once

#include <iomanip>
#include <fstream>

#include "interface/connector.hpp"
#include "interface/json_interface.hpp"
#include "util/sys/file_watcher.hpp"

class FilesystemConnector : public Connector {

private:
    JsonInterface& _interface;
    const Parameters& _params;
    Logger _logger;

    FileWatcher _watcher;

    std::string _base_path;

public:

    FilesystemConnector(JsonInterface& interface, const Parameters& params, Logger&& logger, const std::string& basePath) :
        _interface(interface), _params(params), _logger(std::move(logger)),
        _watcher(basePath + "/in/", (int) (IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE), 
            [&](const FileWatcher::Event& event, Logger& log) {
                // Receiving a certain file event
                handleEvent(event, log);
            },
            _logger, FileWatcher::InitialFilesHandling::TRIGGER_CREATE_EVENT),
        _base_path(basePath) {

        FileUtils::mkdir(_base_path + "/in/");
        FileUtils::mkdir(_base_path + "/out/");

        LOGGER(_logger, V2_INFO, "operational at %s\n", _base_path.c_str());
    }
    ~FilesystemConnector() {}

    void handleEvent(const FileWatcher::Event& event, Logger& log) {

        if (event.type != IN_CLOSE_WRITE && event.type != IN_MOVED_FROM)
            return;

        std::string eventFile = _base_path + "/in/" + event.name;

        // Valid file?
        if (!FileUtils::isRegularFile(eventFile)) {
            LOGGER(log, V3_VERB, "Job file %s does not exist (any more)\n", eventFile.c_str());        
            return; // File does not exist (any more)
        }

        // Attempt to parse JSON from file
        try {
            nlohmann::json j;
            std::ifstream i(eventFile);
            i >> j;

            // Callback for receiving a result for the request at hand
            auto cb = [&](nlohmann::json& result) {
                // Get qualified job name
                std::string user = result["user"].get<std::string>();
                std::string name = result["name"].get<std::string>();
                std::string jobName = user + "." + name + ".json";
                // Write JSON to "out" directory
                std::ofstream o(_base_path + "/out/" + jobName);
                o << std::setw(4) << result << std::endl;
            };

            // Handle JSON file
            auto res = _interface.handle(j, cb);
            // TODO handle res

            // Remove JSON file from "in" directory
            FileUtils::rm(eventFile);

        } catch (const nlohmann::detail::parse_error& e) {
            LOGGER(log, V1_WARN, "[WARN] Parse error on %s: %s\n", eventFile.c_str(), e.what());
            return;
        }
    }
};
