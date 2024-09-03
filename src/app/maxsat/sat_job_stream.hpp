
#pragma once

#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/json.hpp"

class SatJobStream {

private:
    const Parameters& _params;
    APIConnector& _api;

    std::string _base_job_name;
    nlohmann::json _json_base;
    int _subjob_counter {0};
    bool _incremental {true};
    bool _pending {false};
    nlohmann::json _json_result;

public:
    SatJobStream(const Parameters& params, APIConnector& api, std::string username, int streamId, bool incremental) :
        _params(params), _api(api), _incremental(incremental) {
        
        _base_job_name = "satjob-" + std::to_string(streamId) + "-rev-";
        _json_base = nlohmann::json {
            {"user", username},
            {"incremental", incremental},
            {"files", {}},
            {"priority", 1},
            {"application", "SAT"}
        };
    }

    void submitNext(std::vector<int>&& newLiterals, std::vector<int>&& assumptions) {
        assert(!_pending);
        if (_incremental && _json_base.contains("name")) {
            _json_base["precursor"] = _json_base["name"];
        }
        _json_base["name"] = _base_job_name + std::to_string(++_subjob_counter);
        _json_base["literals"] = std::move(newLiterals);
        _json_base["assumptions"] = std::move(assumptions);
        _pending = true;
        _api.submit(_json_base, [&](nlohmann::json& result) {
            _json_result = std::move(result);
            _pending = false;
        });
    }
    void interrupt() {
        if (!_pending) return;
        _json_base["interrupt"] = true;
        _pending = true;
        _api.submit(_json_base, [&](nlohmann::json& result) {
            _json_result = std::move(result);
            _pending = false;
        });
        _json_base["interrupt"] = false;
    }
    void finalize() {
        assert(!_pending);
        if (!_incremental) return;
        if (!_json_base.contains("name")) return;
        _json_base["precursor"] = _json_base["name"];
        _json_base["name"] = _base_job_name + std::to_string(++_subjob_counter);
        _json_base["done"] = true;
        _pending = true;
        _api.submit(_json_base, [&](nlohmann::json& result) {
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
};
