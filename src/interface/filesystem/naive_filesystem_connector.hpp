
#pragma once

#include <iomanip>
#include <fstream>
#include <dirent.h>

#include "interface/connector.hpp"
#include "interface/json_interface.hpp"
#include "util/sys/file_watcher.hpp"

class NaiveFilesystemConnector : public Connector {

private:
    JsonInterface& _interface;
    const Parameters& _params;
    Logger _logger;

    std::string _base_path;

    Logger _watch_logger;
    BackgroundWorker _watch_worker;

public:

    NaiveFilesystemConnector(JsonInterface& interface, const Parameters& params, Logger&& logger, const std::string& basePath) :
        _interface(interface), _params(params), _logger(std::move(logger)), _base_path(basePath),
        _watch_logger(_logger.copy("T", ".watcher")) {

        FileUtils::mkdir(_base_path + "/in/");
        FileUtils::mkdir(_base_path + "/out/");

        runWatcher();

        LOGGER(_logger, V2_INFO, "operational at %s\n", _base_path.c_str());
    }
    ~NaiveFilesystemConnector() {
        _watch_worker.stop();
    }

    void handleEvent(const std::string& filename, Logger& log) {

        std::string eventFile = _base_path + "/in/" + filename;

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
                auto intermediateOutput = _base_path + "/~" + jobName;
                auto finalOutput = _base_path + "/out/" + jobName;
                std::ofstream o(intermediateOutput);
                o << std::setw(4) << result << std::endl;
                std::rename(intermediateOutput.c_str(), finalOutput.c_str());
            };

            // Handle JSON file
            auto res = _interface.handle(j, cb);
            // TODO handle res

            // Remove JSON file from "in" directory
            FileUtils::rm(eventFile);

        } catch (const nlohmann::detail::parse_error& e) {
            LOGGER(log, V1_WARN, "[WARN] Rejecting submission %s - reason: Parse error: %s\n", filename.c_str(), e.what());
            FileUtils::rm(eventFile);
            return;
        } catch (const nlohmann::detail::type_error& e) {
            LOGGER(log, V1_WARN, "[WARN] Rejecting submission %s - reason: Type error: %s\n", filename.c_str(), e.what());
            FileUtils::rm(eventFile);
            return;
        } catch (const std::exception& e) {
            LOGGER(log, V1_WARN, "[WARN] Rejecting submission %s - reason: Unknown error: %s\n", filename.c_str(), e.what());
            FileUtils::rm(eventFile);
            return;
        }
    }

private:
    void runWatcher() {

        _watch_worker.run([&]() {
            Proc::nameThisThread("DirWatcher");

            std::string watchDir = _base_path + "/in";
            // create directory if it doesn't exist
            if (!FileUtils::isDirectory(watchDir)) {
                LOGGER(_watch_logger, V2_INFO, "Create watch directory %s\n", watchDir.c_str());
                FileUtils::mkdir(watchDir);
            }
            
            while (_watch_worker.continueRunning()) {

                DIR *dir;
                struct dirent *ent;
                if ((dir = opendir(watchDir.c_str())) != NULL) {
                    while ((ent = readdir(dir)) != NULL) {
                        
                        std::string filename = ent->d_name;

                        // regular files only
                        if (ent->d_type != DT_REG) continue;
                        // skip ("unfinished") files beginning with a tilde
                        if (filename.empty() || filename[0] == '~')
                            continue;

                        LOGGER(_watch_logger, V2_INFO, "Found %s\n", filename.c_str());
                        handleEvent(filename, _watch_logger);
                    }
                    closedir(dir);
                } else {
                    LOGGER(_watch_logger, V1_WARN, "[WARN] Could not open directory at %s!\n", 
                        watchDir.c_str());
                }

                usleep(1000 * 10); // 10 milliseconds
            }
        });
    }

};
