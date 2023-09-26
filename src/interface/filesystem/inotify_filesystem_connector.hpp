
#pragma once

#include <iomanip>
#include <fstream>

#include "interface/connector.hpp"
#include "interface/json_interface.hpp"
#include "util/sys/file_watcher.hpp"

class InotifyFilesystemConnector : public Connector {

private:
    JsonInterface& _interface;
    const Parameters& _params;
    Logger _logger;

    FileWatcher _watcher;

    std::string _base_path;

public:

    InotifyFilesystemConnector(JsonInterface& interface, const Parameters& params, Logger&& logger, const std::string& basePath) :
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
    ~InotifyFilesystemConnector() {}

    void handleEvent(const FileWatcher::Event& event, Logger& log) {

        if (event.type != IN_CLOSE_WRITE && event.type != IN_MOVED_TO)
            return;

        std::string eventFile = _base_path + "/in/" + event.name;

        // Valid file?
        if (!FileUtils::isRegularFile(eventFile)) {
            LOGGER(log, V3_VERB, "Job file %s does not exist (any more)\n", eventFile.c_str());        
            return; // File does not exist (any more)
        }

        // Skip files beginning with "~" ("unfinished" / temporary files)
        if (event.name.empty() || event.name[0] == '~') {
            return;
        }

        // Attempt to parse JSON from file
        try {
            nlohmann::json j;
            std::ifstream i(eventFile);
            i >> j;

            // Callback for receiving a result for the request at hand
            auto cb = [&](nlohmann::json& result) {
                // Find path for .api file with qualified job name
                std::string user = result["user"].get<std::string>();
                std::string name = result["name"].get<std::string>();
                std::string jobName = user + "." + name + ".json";
                std::string outFilepath = _base_path + "/~" + jobName;
                std::string finalOutFilepath = _base_path + "/out/" + jobName;
                if (result.contains("piped-response") && result["piped-response"].get<bool>()) {
                    // Do not write response to a separate file first;
                    // directly write into the final output.
                    outFilepath = finalOutFilepath;
                    finalOutFilepath.clear();
                }
                // Write JSON to destination
                {
                    std::ofstream o(outFilepath);
                    o << std::setw(4) << result << std::endl;
                }
                if (!finalOutFilepath.empty())
                    std::rename(outFilepath.c_str(), finalOutFilepath.c_str());
            };

            // Handle JSON file
            auto res = _interface.handle(j, cb);
            // TODO handle res

            // Remove JSON file from "in" directory
            FileUtils::rm(eventFile);

        } catch (const nlohmann::detail::parse_error& e) {
            LOGGER(log, V1_WARN, "[WARN] Rejecting submission %s - reason: Parse error: %s\n", event.name.c_str(), e.what());
            return;
        } catch (const nlohmann::detail::type_error& e) {
            LOGGER(log, V1_WARN, "[WARN] Rejecting submission %s - reason: Type error: %s\n", event.name.c_str(), e.what());
            return;
        } catch (const std::exception& e) {
            LOGGER(log, V1_WARN, "[WARN] Rejecting submission %s - reason: Unknown error: %s\n", event.name.c_str(), e.what());
            return;
        }
    }
};
