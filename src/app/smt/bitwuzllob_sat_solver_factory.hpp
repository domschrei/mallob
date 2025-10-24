
#pragma once

#include "app/smt/bitwuzla_sat_connector.hpp"
#include "bitwuzla/cpp/bitwuzla.h"
#include "bitwuzla/cpp/terminator.h"
#include "core/dtask_tracker.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/params.hpp"

class BitwuzllobSatSolverFactory : public bitwuzla::SatSolverFactory {
private:
    const Parameters& _params;
    APIConnector& _api;
    JobDescription& _desc;
    DTaskTracker& _tracker;
    bitwuzla::Terminator& _term;
    std::string _name;

    std::vector<BitwuzlaSatConnector*> solverPointers;
    std::vector<bool> solversCleanedUp;
    int solverCounter = 1;

public:
    BitwuzllobSatSolverFactory(const Parameters& params, APIConnector& api, JobDescription& desc, DTaskTracker& tracker,
        bitwuzla::Terminator& term, const std::string& name, const bitwuzla::Options &options)
            : bitwuzla::SatSolverFactory(options),
            _params(params), _api(api), _desc(desc), _tracker(tracker), _term(term), _name(name) {}

    virtual std::unique_ptr<bitwuzla::SatSolver> new_sat_solver() override {
        auto sat = new BitwuzlaSatConnector(_params, _api, _desc, _tracker,
            _name + ":sat" + std::to_string(solverCounter++)); // cleaned up by Bitwuzla
        solverPointers.push_back(sat);
        solversCleanedUp.push_back(false);
        sat->setCleanupCallback([&, i = solverPointers.size()-1]() {
            solversCleanedUp[i] = true;
        });
        sat->configure_terminator(&_term);
        return std::unique_ptr<bitwuzla::SatSolver>(sat);
    }
    /** Determine if configured SAT solver has terminator support. */
    virtual bool has_terminator_support() override {return true;}

    ~BitwuzllobSatSolverFactory() {
        for (int i = solverPointers.size()-1; i >= 0; i--) {
            if (!solversCleanedUp[i]) delete solverPointers[i];
        }
    }
};
