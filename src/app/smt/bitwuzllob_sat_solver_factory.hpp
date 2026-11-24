
#pragma once

#include "app/incsat/inc_sat_controller.hpp"
#include "app/smt/bitwuzla_sat_connector.hpp"
#include "bitwuzla/cpp/bitwuzla.h"
#include "bitwuzla/cpp/terminator.h"
#include "core/dtask_tracker.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/thread_pool.hpp"

class BitwuzllobSatSolverFactory : public bitwuzla::SatSolverFactory {
private:
    const Parameters _params;
    APIConnector& _api;
    JobDescription& _desc;
    DTaskTracker& _tracker;
    bitwuzla::Terminator& _term;
    std::string _name;

    std::future<void> _fut_solver_provider;
    std::future<void> _fut_incsat_cleaner;

    std::vector<BitwuzlaSatConnector*> solverPointers;
    std::vector<bool> solversCleanedUp;
    int solverCounter = 1;

public:
    BitwuzllobSatSolverFactory(const Parameters& params, APIConnector& api, JobDescription& desc, DTaskTracker& tracker,
        bitwuzla::Terminator& term, const std::string& name)
            : bitwuzla::SatSolverFactory(),
            _params(params), _api(api), _desc(desc), _tracker(tracker), _term(term), _name(name),
            _provision_buffer(8), _clear_buffer(1024) {

        _fut_solver_provider = ProcessWideThreadPool::get().addTask([&]() {
            runSolverProvider();
        });
        _fut_incsat_cleaner = ProcessWideThreadPool::get().addTask([&]() {
            runIncSatCleaner();
        });
    }

    virtual std::unique_ptr<bitwuzla::SatSolver> new_sat_solver() override {
        BitwuzlaSatConnector* sat {nullptr};
        bool ok = _provision_buffer.pollBlocking(sat);
        assert(ok);
        assert(sat);
        solverPointers.push_back(sat);
        solversCleanedUp.push_back(false);
        sat->setCleanupCallback([&, i = solverPointers.size()-1]() {
            solversCleanedUp[i] = true;
        });
        sat->setIncSatCleaner([&](std::unique_ptr<IncSatController>&& incSatPtr) {
            auto incsat = incSatPtr.release();
            _clear_buffer.pushBlocking(incsat);
        });
        sat->configure_ext_terminator(&_term);
        return std::unique_ptr<bitwuzla::SatSolver>(sat);
    }
    /** Determine if configured SAT solver has terminator support. */
    virtual bool has_terminator_support() override {return true;}

    ~BitwuzllobSatSolverFactory() {

        // stop providing new solver instances and flush the remaining ones
        LOG(V2_INFO, "SMT stop factory: provisioner\n");
        _provision_buffer.markTerminated();
        BitwuzlaSatConnector* solver {nullptr};
        while (!_provision_buffer.exhausted() && _provision_buffer.pollBlocking(solver)) {
            delete solver; // calls incsat cleaner above -> pushes to _clear_buffer!
        }
        _fut_solver_provider.get();

        LOG(V2_INFO, "SMT stop factory: cleaner\n");
        // delete all remaining IncSat instances
        while (!_clear_buffer.empty()) usleep(1000 * 10);
        _clear_buffer.markExhausted();
        _fut_incsat_cleaner.get();

        LOG(V2_INFO, "SMT stop factory: delete dangling solvers\n");
        // delete any dangling solver references left by Bitwuzla
        for (int i = solverPointers.size()-1; i >= 0; i--) {
            if (!solversCleanedUp[i]) delete solverPointers[i];
        }
    }

private:
    SPSCBlockingRingbuffer<BitwuzlaSatConnector*> _provision_buffer;
    void runSolverProvider() {
        while (!_provision_buffer.terminated()) {
            // will be cleaned up by Bitwuzla or in the destructor above
            auto solver = new BitwuzlaSatConnector(_params, _api, _desc, _tracker,
                    _name + ":sat" + std::to_string(solverCounter++));
            solver->preInitialize();
            _provision_buffer.pushBlocking(solver);
        }
        while (!_provision_buffer.empty()) usleep(1000 * 10);
        _provision_buffer.markExhausted();
    }

    SPSCBlockingRingbuffer<IncSatController*> _clear_buffer;
    void runIncSatCleaner() {
        IncSatController* incsat {nullptr};
        while (_clear_buffer.pollBlocking(incsat)) {
            delete incsat;
        }
        _clear_buffer.markTerminated();
    }

};
