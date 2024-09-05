
#pragma once

#include "app/maxsat/maxsat_instance.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/json.hpp"

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
    nlohmann::json _json_result;

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
        for (auto key : {"__NV", "__NC", "__NO"})
            _json_base["configuration"][key] = desc.getAppConfiguration().map.at(key);
    }

    void submitNext(const std::vector<int>& newLiterals, const std::vector<int>& assumptions) {
        assert(!_pending);
        if (_incremental && _json_base.contains("name")) {
            _json_base["precursor"] = _username + std::string(".") + _json_base["name"].get<std::string>();
        }
        _json_base["name"] = _base_job_name + std::to_string(++_subjob_counter);
        _json_base["literals"] = newLiterals;
        _json_base["assumptions"] = assumptions;
        _pending = true;
        nlohmann::json copy(_json_base);
        _api.submit(copy, [&](nlohmann::json& result) {
            _json_result = std::move(result);
            _pending = false;
        });
    }
    void interrupt() {
        if (!_pending) return;
        _pending = true;
        nlohmann::json copy(_json_base);
        copy["interrupt"] = true;
        _api.submit(copy, [&](nlohmann::json& result) {
            _json_result = std::move(result);
            _pending = false;
        });
    }
    void finalize() {
        assert(!_pending);
        if (!_incremental) return;
        if (!_json_base.contains("name")) return;
        _json_base["precursor"] = _json_base["name"];
        _json_base["name"] = _base_job_name + std::to_string(++_subjob_counter);
        _pending = true;
        nlohmann::json copy(_json_base);
        copy["done"] = true;
        _api.submit(copy, [&](nlohmann::json& result) {
            _json_result = std::move(result);
            _pending = false;
        });
    }

    bool isPending() const {
        return _pending;
    }
    nlohmann::json& getResult() {
        assert(!_pending);
        return _json_result;
    }
    int getSubjobCount() const {
        return _subjob_counter;
    }
};
