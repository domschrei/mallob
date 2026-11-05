
#pragma once

#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/threading.hpp"
#include <fstream>
#include <vector>

class ClauseLogger {

private:
    std::ofstream _ofs;
    Mutex _mtx_parcelled_clauses;
    ConditionVariable _cond_var;
    std::vector<std::vector<int>> _parcelled_clauses;
    BackgroundWorker _bg_worker;

    bool _allow_retrieval {false};
    bool _raw_output {false};
    std::vector<std::vector<int>> _published_parcelled_clauses;
    Mutex _mtx_published_clauses;

public:
    ClauseLogger(const std::string& outputPath, bool allowRetrieval, bool rawOutput) :
            _ofs(outputPath), _allow_retrieval(allowRetrieval), _raw_output(rawOutput) {
        _bg_worker.run([&]() {run();});
    }

    void disableFileOutput() {
        _ofs.close();
    }

    void append(const Mallob::Clause& clause) {
        std::vector<int> vec(clause.size + !_raw_output);
        if (!_raw_output) vec[0] = clause.lbd;
        for (size_t i = 0; i < clause.size; i++) vec[i+!_raw_output] = clause.begin[i];
        {
            auto lock = _mtx_parcelled_clauses.getLock();
            _parcelled_clauses.push_back(std::move(vec));
        }
    }

    void publish() {
        {
            auto lock = _mtx_parcelled_clauses.getLock();
            _parcelled_clauses.emplace_back();
        }
        _cond_var.notify();
    }

    const std::vector<std::vector<int>>& beginAccessingPublishedClauses() {
        _mtx_published_clauses.lock();
        return _published_parcelled_clauses;
    }
    void endAccessingPublishedClauses() {
        _mtx_published_clauses.unlock();
    }

    void run() {
        std::vector<std::vector<int>> outClauses;
        while (true) {
            {
                auto lock = _mtx_parcelled_clauses.getLock();
                _cond_var.waitWithLockedMutex(lock, [&]() {
                    return !_bg_worker.continueRunning() || !_parcelled_clauses.empty();
                });
                if (!_bg_worker.continueRunning()) break;
                outClauses = std::move(_parcelled_clauses);
            }
            bool publishedSomething = false;
            if (_ofs.is_open()) for (auto& vec : outClauses) if (!vec.empty()) {
                if (!_raw_output) {
                    _ofs << vec[0] << " ";
                    if (ClauseMetadata::enabled()) {
                        uint64_t id = ClauseMetadata::readUnsignedLong(vec.data()+1);
                        _ofs << " [id=" << id << "] ";
                    }
                }
                for (size_t i = !_raw_output+ClauseMetadata::numInts(); i < vec.size(); i++)
                    _ofs << vec[i] << " ";
                _ofs << "0" << std::endl;
                publishedSomething = true;
            }
            if (_allow_retrieval) {
                auto lock = _mtx_published_clauses.getLock();
                for (auto& cls : outClauses) if (!cls.empty()) {
                    _published_parcelled_clauses.push_back(std::move(cls));
                }
            }
            outClauses.clear();
            if (publishedSomething) _ofs.flush();
        }
        if (_ofs.is_open()) _ofs.close();
    }

    ~ClauseLogger() {
        publish();
    }
};
