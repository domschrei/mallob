
#pragma once

#include "app/sat/data/clause.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/threading.hpp"
#include <fstream>
#include <vector>

class ClauseLogger {

private:
    BackgroundWorker _bg_worker;
    std::ofstream _ofs;
    Mutex _mtx_parcelled_clauses;
    ConditionVariable _cond_var;
    std::vector<std::vector<int>> _parcelled_clauses;

public:
    ClauseLogger(const std::string& outputPath) : _ofs(outputPath) {
        _bg_worker.run([&]() {run();});
    }

    void append(const Mallob::Clause& clause) {
        std::vector<int> vec(clause.size+1);
        vec[0] = clause.lbd;
        for (size_t i = 0; i < clause.size; i++) vec[i+1] = clause.begin[i];
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
            for (auto& vec : outClauses) if (!vec.empty()) {
                _ofs << (vec[0]-1);
                for (size_t i = 1; i < vec.size(); i++) _ofs << " " << vec[i];
                _ofs << std::endl;
            }
            outClauses.clear();
            _ofs << std::endl;
            _ofs.flush();
        }
        _ofs.close();
    }
};
