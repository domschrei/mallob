
#pragma once

#include "app/maxsat/maxsat_instance.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "interface/json_interface.hpp"
#include "util/json.hpp"
#include "util/logger.hpp"
#include "util/static_store.hpp"
#include "util/params.hpp"

class SatJobStream {

private:
    const Parameters& _params;
    APIConnector& _api;
    JobDescription& _desc;

    bool _incremental {true};
    const std::string _username;
    std::string _base_job_name;
    nlohmann::json _json_base;
    int _subjob_counter {0};
    bool _pending {false};
    bool _interrupt_set {false};
    nlohmann::json _json_result;
    bool _rejected {false};
    std::string _expected_result_job_name;

public:
    SatJobStream(const Parameters& params, APIConnector& api, JobDescription& desc,
            int streamId, bool incremental) :
        _params(params), _api(api), _desc(desc), _incremental(incremental), 
        _username("maxsat#" + std::to_string(_desc.getId())) {

        _base_job_name = "satjob-" + std::to_string(streamId) + "-rev-";
        _json_base = nlohmann::json {
            {"user", _username},
            {"incremental", incremental},
            {"priority", 1},
            {"application", "SAT"}
        };
        _json_base["files"] = std::vector<std::string>();
        _json_base["configuration"]["__XL"] = -1;
        _json_base["configuration"]["__XU"] = -1;
    }

    void setGroupId(const std::string& groupId, int minVar = -1, int maxVar = -1) {
        _json_base["group-id"] = groupId;
        _json_base["configuration"]["__XL"] = std::to_string(minVar);
        _json_base["configuration"]["__XU"] = std::to_string(maxVar);
    }
    void setInnerObjective(const std::string& objective) {
        _json_base["configuration"]["__OBJ"] = objective;
    }

    void submitNext(std::vector<int>&& newLiterals, const std::vector<int>& assumptions,
            const std::string& descriptionLabel = "", float priority = 0) {
        assert(!_pending);
        assert(newLiterals.empty() || newLiterals.front() != 0);
        assert(newLiterals.empty() || newLiterals.back() == 0);

        for (auto key : {"__NV", "__NC", "__NO"})
            _json_base["configuration"][key] = _desc.getAppConfiguration().map.at(key);

        if (_incremental && _json_base.contains("name")) {
            _json_base["precursor"] = _username + std::string(".") + _json_base["name"].get<std::string>();
        }
        _json_base["priority"] = priority > 0 ? priority : 1;
        const int subjob = _subjob_counter++;
        _json_base["name"] = _base_job_name + std::to_string(subjob);
        if (_params.maxSatWriteJobLiterals()) {
            std::ofstream ofs(_params.logDirectory() + "/maxsat.joblits." + _json_base["name"].get<std::string>());
            for (int lit : newLiterals) {
                ofs << lit << " ";
                if (lit == 0) ofs << std::endl;
            }
            ofs = std::ofstream(_params.logDirectory() + "/maxsat.jobassumptions." + _json_base["name"].get<std::string>());
            for (int lit : assumptions) {
                ofs << lit << " 0" << std::endl;
            }
        }
        nlohmann::json copy(_json_base);
        StaticStore<std::vector<int>>::insert(_json_base["name"].get<std::string>(), std::move(newLiterals));
        copy["internalliterals"] = _json_base["name"].get<std::string>();
        copy["assumptions"] = assumptions;
        if (!descriptionLabel.empty()) {
            copy["description-id"] = descriptionLabel;
        }
        _expected_result_job_name = copy["name"].get<std::string>();
        _pending = true;
        _rejected = false;
        _interrupt_set = false;
        auto response = _api.submit(copy, [&](nlohmann::json& result) {
            if (result["name"].get<std::string>() != _expected_result_job_name) {
                LOG(V0_CRIT, "[ERROR] MAXSAT Result for unexpected job \"%s\" (expected: %s)!\n",
                    result["name"].get<std::string>().c_str(), _expected_result_job_name.c_str());
                abort();
            }
            _json_result = std::move(result);
            _pending = false;
        });
        if (response == JsonInterface::Result::DISCARD) {
            _rejected = true;
            _pending = false;
        }
    }
    bool interrupt() {
        if (!_pending || _interrupt_set) return false;
        _interrupt_set = true;
        nlohmann::json jsonInterrupt {
            {"name", _json_base["name"]},
            {"user", _json_base["user"]},
            {"application", _json_base["application"]},
            {"incremental", _json_base["incremental"]},
            {"interrupt", true}
        };
        // In this particular case, the callback is never called.
        // Instead, the callback of the job's original submission is called.
        auto response = _api.submit(jsonInterrupt, [&](nlohmann::json& result) {assert(false);});
        if (response == JsonInterface::Result::DISCARD) {
            _rejected = true;
            _pending = false;
        }
        return true;
    }
    void finalize() {
        if (_pending) return; // if a job is still running, then we must be terminating alltogether.
        if (!_incremental) return;
        if (!_json_base.contains("name")) return;
        _json_base["precursor"] = _username + std::string(".") + _json_base["name"].get<std::string>();
        _json_base["name"] = _base_job_name + std::to_string(_subjob_counter++);
        nlohmann::json copy(_json_base);
        copy["done"] = true;
        // The callback is never called.
        _api.submit(copy, [&](nlohmann::json& result) {assert(false);});
    }

    bool isPending() {
        if (_pending && !_api.active()) {
            _rejected = true;
            _pending = false;
        }
        return _pending;
    }
    bool isRejected() const {
        return _rejected;
    }
    nlohmann::json& getResult() {
        assert(!_pending);
        return _json_result;
    }
};
