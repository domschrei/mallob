
#pragma once

#include <atomic>
#include <fstream>
#include <ios>

#include "interface/api/rank_specific_file_fetcher.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "interface/json_interface.hpp"
#include "interface/api/api_connector.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/background_worker.hpp"
#include "util/random.hpp"
#include "interface/api/client_template.hpp"

class JobStreamer {

private:
    const Parameters& _params;
    APIConnector& _api;
    int _internal_rank;
    nlohmann::json _json_template;
    bool _valid = false;
    RankSpecificFileFetcher _rank_specific_file_fetcher;
    ClientTemplate _client_template;

    int _job_counter = 1;
    int _job_description_index = 0;
    std::vector<std::vector<std::string>> _job_descriptions;

    BackgroundWorker _bg_worker;
    std::atomic_int _num_active_jobs = 0;
    Mutex _submit_mutex;
    ConditionVariable _submit_cond_var;

    BackgroundWorker _bg_deleter;
    std::atomic_int _num_jsons_to_delete = 0;
    Mutex _delete_mutex;
    ConditionVariable _delete_cond_var;
    std::vector<nlohmann::json> _jsons_to_delete;

    std::ofstream _ofs_results;
    int _nb_introduced_jobs {0};
    int _nb_concluded_jobs {0};

public:
    JobStreamer(const Parameters& params, APIConnector& api, int internalRank) : 
            _params(params), _api(api), _internal_rank(internalRank),
            _rank_specific_file_fetcher(internalRank),
            _client_template(_params.seed()+_internal_rank,
            _rank_specific_file_fetcher.get(_params.clientTemplate())) {

        // Attempt to parse JSON from file
        std::string jsonTemplateFile = _rank_specific_file_fetcher.get(_params.jobTemplate());
        if (jsonTemplateFile.empty()) return;
        try {
            std::ifstream i(jsonTemplateFile);
            i >> _json_template;
            _valid = true;
        } catch (const nlohmann::detail::parse_error& e) {
            LOG(V1_WARN, "[WARN] Parse error on job template file %s: %s\n", 
                jsonTemplateFile.c_str(), e.what());
        }

        // Parse job description files
        if (_params.jobDescriptionTemplate.isSet()) {
            std::string descriptionTemplateFile = _rank_specific_file_fetcher.get(_params.jobDescriptionTemplate());
            std::ifstream i(descriptionTemplateFile);
            std::string line;
            while (std::getline(i, line)) {
                if (line.empty()) break;
                std::vector<std::string> filenames;
                size_t idx = 0;
                while (idx < line.size()) {
                    size_t start = idx;
                    while (idx < line.size() && line[idx] != ' ') idx++;
                    filenames.push_back(line.substr(start, idx-start));
                    idx++;
                }
                _job_descriptions.push_back(std::move(filenames));
            }
        }
        // Shuffle job description files if desired
        if (!_job_descriptions.empty() && _params.shuffleJobDescriptions()) {
            auto rng = std::mt19937(_params.seed() + _internal_rank); 
            auto dist = std::uniform_real_distribution<float>(0, 1); 
            auto rngFunc = [&]() {return dist(rng);};
            random_shuffle(_job_descriptions.data(), _job_descriptions.size(), rngFunc);
        }

        // Run concurrent submission of jobs
        _bg_worker.run([&]() {
            Proc::nameThisThread("JobStreamer");
            std::string baseJobName = _json_template["name"];
            Logger logger = Logger::getMainInstance().copy("Streamer", ".streamer");

            while (_bg_worker.continueRunning()) {

                // Wait until there is work
                _submit_cond_var.wait(_submit_mutex, [&]() {
                    return !_bg_worker.continueRunning() || 
                        _num_active_jobs < _params.activeJobsPerClient();
                });
                if (!_bg_worker.continueRunning()) break;

                // Submit new jobs
                while (_num_active_jobs < _params.activeJobsPerClient()) {
                    
                    // Already submitted enough jobs?
                    if (_params.maxJobsPerStreamer() != 0 && _nb_introduced_jobs == _params.maxJobsPerStreamer())
                        break;

                    // Prepare JSON
                    auto jsonCopy = _json_template;
                    jsonCopy["name"] = baseJobName + "-" + std::to_string(_job_counter++);
                    if (!_job_descriptions.empty()) {
                        assert(_job_description_index < _job_descriptions.size());
                        jsonCopy["files"] = _job_descriptions[_job_description_index];
                        _job_description_index = (_job_description_index+1) % _job_descriptions.size();
                    }
                    if (_client_template.valid()) {
                        jsonCopy["arrival"] = _client_template.getNextArrival();
                        jsonCopy["priority"] = _client_template.getNextPriority();
                        jsonCopy["wallclock-limit"] = std::to_string(_client_template.getNextWallclockLimit())+"s";
                        jsonCopy["max-demand"] = _client_template.getNextMaxDemand();
                    }

                    LOGGER(logger, V3_VERB, "SUBMIT %s arr=%.3f prio=%.3f wclim=%s maxdem=%i\n", 
                            jsonCopy["name"].get<std::string>().c_str(),
                            jsonCopy["arrival"].get<double>(),
                            jsonCopy["priority"].get<double>(),
                            jsonCopy["wallclock-limit"].get<std::string>().c_str(),
                            jsonCopy["max-demand"].get<int>());
                    _num_active_jobs++;
                    _api.submit(jsonCopy, [&](nlohmann::json& result) {
                        // Result for the job arrived.
                        {
                            auto lock = _submit_mutex.getLock();
                            _num_active_jobs--;
                        }
                        _submit_cond_var.notify();
                        // Garbage collect JSON
                        {
                            auto lock = _delete_mutex.getLock();
                            _jsons_to_delete.emplace_back(std::move(result));
                            _num_jsons_to_delete++;
                        }
                        _delete_cond_var.notify();
                    });
                    logger.flush();
                    _nb_introduced_jobs++;
                }
            }
        });

        _bg_deleter.run([&]() {
            Proc::nameThisThread("JobDeleter");

            bool writeResultsToFile = false;
            if (_params.streamerResultOutput.isSet()) {
                writeResultsToFile = true;
                _ofs_results.open(_params.streamerResultOutput() 
                    + (_rank_specific_file_fetcher.hasRankSpecificTemplate() ?
                        "." + std::to_string(_internal_rank) : ""));
            }

            while (_bg_deleter.continueRunning() || _num_jsons_to_delete.load(std::memory_order_relaxed) > 0) {

                _delete_cond_var.wait(_delete_mutex, [&]() {
                    return !_bg_deleter.continueRunning() ||
                        _num_jsons_to_delete > 0;
                });

                // Delete a garbage JSON
                while (_num_jsons_to_delete > 0) {
                    nlohmann::json jsonToDelete;
                    {
                        auto lock = _delete_mutex.getLock();
                        jsonToDelete = std::move(_jsons_to_delete.back());
                        _jsons_to_delete.pop_back();
                        _num_jsons_to_delete--;
                    }
                    if (writeResultsToFile) {
                        jsonToDelete["result"].erase("solution"); // potentially expensive call!
                        _ofs_results << jsonToDelete << std::endl;
                        _ofs_results.flush();
                    }
                    // JSON destructor is called here
                    _nb_concluded_jobs++;
                }
            }
        });
    }

    ~JobStreamer() {
        _bg_worker.stopWithoutWaiting();
        _bg_deleter.stopWithoutWaiting();
        {auto lock = _submit_mutex.getLock();}
        _submit_cond_var.notify();
        {auto lock = _delete_mutex.getLock();}
        _delete_cond_var.notify();
        _bg_worker.stop();
        _bg_deleter.stop();
        LOG(V2_INFO, "Streamer concluded %i/%i introduced jobs\n",
            _nb_concluded_jobs, _nb_introduced_jobs);
    }
};
