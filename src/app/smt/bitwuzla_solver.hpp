
#pragma once

#include "app/smt/bitwuzla_sat_connector.hpp"
#include "core/job_slot_registry.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"

#include "bitwuzla/cpp/parser.h"
#include "bitwuzla/cpp/bitwuzla.h"
#include "bitwuzla/cpp/sat_solver_factory.h"
#include "bitwuzla/cpp/main.h"

#include <cstdint>
#include <cstdio>

class BitwuzlaSolver {

private:
    const Parameters& _params;
    APIConnector& _api;
    JobDescription& _desc;
    std::string _problem_file;
    float _start_time = (float) INT32_MAX;

    std::string _name;

    struct BzllobTerminator : public bitwuzla::Terminator {
        std::function<bool()> cb;
        BzllobTerminator(std::function<bool()> cb) : cb(cb) {}
        virtual bool terminate() {
            return cb();
        }
    } _terminator;

public:
    BitwuzlaSolver(const Parameters& params, APIConnector& api, JobDescription& desc, const std::string& problemFile) :
            _params(params), _api(api), _desc(desc), _problem_file(problemFile),
            _name("#" + std::to_string(desc.getId()) + "(SMT)"),
            _terminator([&]() {return isTimeoutHit(&_params, &_desc, _start_time);}) {

        LOG(V2_INFO,"SMT Bitwuzla+Mallob %s\n", _name.c_str());

        if (!JobSlotRegistry::isInitialized()) JobSlotRegistry::init(params);
    }
    ~BitwuzlaSolver() {
        LOG(V2_INFO, "Deleting SMT Bitwuzla+Mallob #%i\n", _desc.getId());
    }

    JobResult solve() {
        _start_time = Timer::elapsedSeconds();

        bitwuzla::Options options;
        bitwuzla::TermManager tm;

        std::vector<std::string> argVec;
        argVec.push_back("./bitwuzla");
        argVec.push_back((char*) _problem_file.c_str());
        char wcl[64];
        if (_params.jobWallclockLimit.isNonzero() || _params.timeLimit.isNonzero()) {
            unsigned long limitMillis = INT32_MAX;
            if (_params.jobWallclockLimit.isNonzero())
                limitMillis = std::min(limitMillis, (unsigned long) (1000 * _params.jobWallclockLimit()));
            if (_params.timeLimit.isNonzero())
                limitMillis = std::min(limitMillis, (unsigned long) (1000 * (_params.timeLimit() - Timer::elapsedSeconds())));
            snprintf(wcl, 63, "%lu", limitMillis);
            // Unfortunately we can't give the timeout to Bitwuzla directly right now
            // because Bitwuzla acknowledges timeouts via process exit, which we can't
            // do as a job within a Mallob MPI process.
            //argVec.push_back("--time-limit");
            //argVec.push_back(wcl);
        }
        if (_params.bitwuzlaArgs.isSet()) {
            stringstream ss(_params.bitwuzlaArgs());
            string str;
            while (getline(ss, str, ',')) {
                LOG(V2_INFO, "SMT Appending Bitwuzla arg \"%s\"\n", str.c_str());
                argVec.push_back(str);
            }
        }
        int argc = argVec.size();
        std::vector<char*> v;
        for (auto& str : argVec) v.push_back((char*) str.c_str());
        char** argv = v.data();

        std::vector<std::string> args;
        bzla::main::Options main_options =
            bzla::main::parse_options(argc, argv, args);

        auto out = &std::cout;
        if (_params.smtOutputFile.isSet()) {
            out = new std::ofstream(getSmtOutputFilePath(_params, _desc.getId()));
        }

        // If Bitwuzla fails to clean up after itself, we're gonna do it.
        std::vector<BitwuzlaSatConnector*> solverPointers;
        std::vector<bool> solversCleanedUp;

        // This instruction replaces the internal SAT solver of Bitwuzla with a Mallob-connected solver.
        int solverCounter = 1;
        bzla::sat::ExternalSatSolver::new_sat_solver = [&, name=_name]() {
            auto sat = new BitwuzlaSatConnector(_params, _api, _desc,
                name + ":sat" + std::to_string(solverCounter++), _start_time); // cleaned up by Bitwuzla
            //sat->outputModels(out); // for debugging
            solverPointers.push_back(sat);
            solversCleanedUp.push_back(false);
            sat->setCleanupCallback([i = solverPointers.size()-1, &solversCleanedUp]() {
                solversCleanedUp[i] = true;
            });
            return sat;
        };

        try {
            bzla::main::set_time_limit(main_options.time_limit);
            options.set(args);

            if (main_options.print_unsat_core) {
                options.set(bitwuzla::Option::PRODUCE_UNSAT_CORES, 1);
            }
            if (main_options.print_model) {
                options.set(bitwuzla::Option::PRODUCE_MODELS, 1);
            }

            *out << bitwuzla::set_bv_format(main_options.bv_format);
            *out << bitwuzla::set_letify(!main_options.print_no_letify);
            bitwuzla::parser::Parser parser(
                tm, options, main_options.language, out);
            parser.configure_auto_print_model(main_options.print_model);
            parser.configure_terminator(&_terminator);
            parser.parse(
                main_options.infile_name,
                main_options.print || main_options.pp_only || main_options.parse_only
            );
            bzla::main::reset_time_limit();
            auto bitwuzla = parser.bitwuzla();

            if (main_options.pp_only) {
                bitwuzla->simplify();
            }
            if (main_options.print) {
                if (!main_options.parse_only && !main_options.pp_only) {
                    bitwuzla->simplify();
                }
                bitwuzla->print_formula(*out, "smt2");
            }

            if (main_options.print_unsat_core) {
                bitwuzla->print_unsat_core(*out);
            }

            if (options.get(bitwuzla::Option::VERBOSITY)) {
                auto stats = bitwuzla->statistics();
                for (auto& [name, val] : stats) {
                    *out << name << ": " << val << std::endl;
                }
            }

        } catch (const bitwuzla::parser::Exception& e) {
            bzla::main::Error() << e.msg();
        } catch (const bitwuzla::Exception& e) {
            //// Remove the "invalid call to '...', prefix
            if (e.msg().find("invalid call") == 0) {
                const std::string& msg = e.msg();
                size_t pos             = msg.find("', ");
                bzla::main::Error() << msg.substr(pos + 3);
            } else {
                bzla::main::Error() << e.msg();
            }
        } catch (...) {
            LOG(V0_CRIT, "[ERROR] uncaught exception in Bitwuzla program\n");
            abort();
        }

        for (int i = solverPointers.size()-1; i >= 0; i--) {
            if (!solversCleanedUp[i]) delete solverPointers[i];
        }

        if (_params.smtOutputFile.isSet()) {
            delete out;
        }

        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = 20;
        LOG(V2_INFO,"SMT return result\n");

        return res;
    }

    static std::string getSmtOutputFilePath(const Parameters& params, int jobId) {
        return params.smtOutputFile() + (params.monoFilename.isSet() ? "" : "." + std::to_string(jobId));
    }

    bool isTimeoutHit(const Parameters* params, JobDescription* desc, float startTime) const {
        if (Terminator::isTerminating())
            return true;
        float t = Timer::elapsedSeconds();
        if (params->timeLimit() > 0 && t >= params->timeLimit())
            return true;
        if (desc->getWallclockLimit() > 0 && (t - startTime) >= desc->getWallclockLimit())
            return true;
        return false;
    }
};
