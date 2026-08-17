
#pragma once

#include "app/smt/bitwuzllob_sat_solver_factory.hpp"
#include "core/dtask_tracker.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"

#include "bitwuzla/cpp/parser.h"
#include "bitwuzla/cpp/bitwuzla.h"
#include "util/sys/thread_pool.hpp"

#include <cstdint>
#include <cstdio>

class BitwuzlaSolver {

private:
    const Parameters _params;
    APIConnector& _api;
    JobDescription& _desc;
    std::string _problem_file;
    float _start_time = (float) INT32_MAX;

    std::string _name;

    std::future<void> _fut;
    JobResult _result;

    struct BzllobTerminator : public bitwuzla::Terminator {
        BitwuzlaSolver& inst;
        const Parameters& params;
        JobDescription& desc;
        float endTime = INT32_MAX;
        BzllobTerminator(BitwuzlaSolver& inst, const Parameters& params, JobDescription& desc, float startTime) :
            inst(inst), params(params), desc(desc) {
            updateStartTime(startTime);
        }
        void updateStartTime(float startTime) {
            endTime = std::min(endTime, getEndTime(&params, &desc, startTime));
        }
        inline bool terminate() {
            return inst.isTimeoutHit(&params, &desc, endTime);
        }
    } _terminator;

public:
    BitwuzlaSolver(const Parameters& params, APIConnector& api, JobDescription& desc, const std::string& problemFile) :
            _params(params), _api(api), _desc(desc), _problem_file(problemFile),
            _name("#" + std::to_string(desc.getId()) + "(SMT)"),
            _terminator(*this, _params, _desc, _start_time) {

        LOG(V2_INFO,"SMT Bitwuzla+Mallob %s\n", _name.c_str());
    }
    ~BitwuzlaSolver() {
        if (_fut.valid()) _fut.get();
        LOG(V2_INFO, "Deleting SMT Bitwuzla+Mallob #%i\n", _desc.getId());
    }

    JobResult solve() {
        _start_time = Timer::elapsedSeconds();
        _terminator.updateStartTime(_start_time);
        float endTime = getEndTime(&_params, &_desc, _start_time);
        _result.result = -1;

        // We execute Bitwuzllob concurrently in another thread. If it gets stuck somewhere,
        // we can still quit the entire program without destroying this BitwuzlaSolver instance
        // and hence without waiting for Bitwuzllob to return (if -terminate-abruptly=1).
        _fut = ProcessWideThreadPool::get().addTask([&]() {
            run();
        });

        float sleepMicros = 1;
        while (_result.result == -1 && !isTimeoutHit(&_params, &_desc, endTime)) {
            // To allow for relatively small latencies for trivial problems:
            // initially just sleep for 1us, then increase it exponentially up to 25ms
            usleep((unsigned long) sleepMicros);
            sleepMicros = std::min(25'000.f, 1 + sleepMicros * 1.5f);
        }

        _result.id = _desc.getId();
        _result.revision = 0;
        if (_result.result <= 0 || isTimeoutHit(&_params, &_desc, endTime)) {
            LOG(V2_INFO, "%s SMT TASK INTERRUPTED time=%.3fs\n", _name.c_str(),
                Timer::elapsedSeconds()-_start_time);
            JobResult res = _result;
            res.result = 0;
            return res;
        } else {
            LOG(V2_INFO, "%s SMT TASK COMPLETE time=%.3fs\n", _name.c_str(),
                Timer::elapsedSeconds()-_start_time);
            return _result;
        }
    }

    static std::string getSmtOutputFilePath(const Parameters& params, int jobId) {
        return params.smtOutputFile() + (params.monoFilename.isSet() ? "" : "." + std::to_string(jobId));
    }

    static inline bool isTimeoutHit(const Parameters* params, JobDescription* desc, float endTime) {
        if (Terminator::isTerminating()) {
            return true;
        }
        if (Timer::elapsedSeconds() > endTime) {
            return true;
        }
        return false;
    }
    static float getEndTime(const Parameters* params, JobDescription* desc, float startTime) {
        float endTime = INT32_MAX;
        if (params->timeLimit() > 0)
            endTime = std::min(endTime, startTime + params->timeLimit());
        if (desc->getWallclockLimit() > 0)
            endTime = std::min(endTime, startTime + desc->getWallclockLimit());
        return endTime;
    }

private:
    void run() {
        bitwuzla::Options options;
        bitwuzla::TermManager tm;

        auto out = &std::cout;
        bool smtOutFileSet = false;
        if (_desc.getAppConfiguration().map.count("smt-out-file")) {
            LOG(V2_INFO, "SMT Using smt-out-file %s\n", _desc.getAppConfiguration().map["smt-out-file"].c_str());
            out = new std::ofstream(_desc.getAppConfiguration().map["smt-out-file"]);
            smtOutFileSet = true;
        } else if (_params.smtOutputFile.isSet()) {
            LOG(V2_INFO, "SMT Using smt-out-file %s\n", _params.smtOutputFile().c_str());
            out = new std::ofstream(getSmtOutputFilePath(_params, _desc.getId()));
            smtOutFileSet = true;
        } else {
            LOG(V2_INFO, "SMT Using NO smt-out-file\n");
        }

        // Default top-level Bitwuzla options
        bool print_no_letify = false, print_formula = false, pp_only = false, parse_only = false;
        bool print_model = false;
        uint8_t bv_format = 2;
        std::string language = "smt2";

        // Parse Bitwuzla options
        std::string bzlaArgsString = _params.bitwuzlaArgs();
        if (_desc.getAppConfiguration().map.count("smt-args"))
            bzlaArgsString += "," + _desc.getAppConfiguration().map["smt-args"];
        std::stringstream ss(bzlaArgsString);
        string arg;
        std::vector<std::string> opts;
        while (!bzlaArgsString.empty() && getline(ss, arg, ',')) {
            if (arg.empty()) continue;
            std::string lhs, rhs;
            int ll = (arg.size()>0 && arg[0]=='-') + (arg.size()>1 && arg[1]=='-');
            int lr = ll + 1;
            while (lr < arg.size() && arg[lr] != '=') lr++;
            if (lr < arg.size()) {
                // -a=b
                lhs = arg.substr(ll, lr-ll);
                rhs = arg.substr(lr+1);
            } else {
                // -a
                lhs = arg.substr(ll);
            }
            LOG(V2_INFO, "SMT Appending Bitwuzla arg: %s := %s\n", lhs.c_str(), rhs.c_str());
            if (rhs.empty() && lhs == "print-unsat-core")
                options.set(bitwuzla::Option::PRODUCE_UNSAT_CORES, 1);
            else if (rhs.empty() && lhs == "print-model") {
                options.set(bitwuzla::Option::PRODUCE_MODELS, 1);
                print_model = true;
            }
            else if (rhs.empty() && lhs == "print-formula") print_formula = true;
            else if (rhs.empty() && lhs == "print-no-letify") print_no_letify = true;
            else if (rhs.empty() && lhs == "pp-only") pp_only = true;
            else if (rhs.empty() && (lhs == "parse-only" || lhs == "P")) parse_only = true;
            else if (!rhs.empty() && lhs == "bv-output-format") bv_format = atoi(rhs.c_str());
            else if (!rhs.empty() && lhs == "lang") language = rhs;
            else opts.push_back(arg);
        }
        options.set(opts);
        float endTime = getEndTime(&_params, &_desc, _start_time);
        if (endTime < INT32_MAX) {
            options.set(bitwuzla::Option::TIME_LIMIT_PER, 1000.f * (endTime - Timer::elapsedSeconds()));
        }
        options.set_diagnostic_output_stream(*out);

        DTaskTracker dTaskTracker(_params);
        std::unique_ptr<BitwuzllobSatSolverFactory> factory;
        bool success = false;

        try {
            *out << bitwuzla::set_bv_format(bv_format);
            *out << bitwuzla::set_letify(!print_no_letify);

            factory = std::make_unique<BitwuzllobSatSolverFactory>(
                _params, _api, _desc, dTaskTracker,
                _terminator, _name);

            bitwuzla::parser::Parser parser(
                tm, *factory.get(), options, language, out);
            parser.configure_auto_print_model(print_model);
            parser.configure_terminator(&_terminator);
            parser.parse(
                _problem_file,
                print_formula || pp_only || parse_only
            );
            auto bitwuzla = parser.bitwuzla();

            if (pp_only) bitwuzla->simplify();
            if (print_formula) {
                if (!parse_only && !pp_only) bitwuzla->simplify();
                bitwuzla->print_formula(*out, "smt2");
            }
            success = true;

        } catch (const bitwuzla::parser::Exception& e) {
            LOG(V0_CRIT, "[ERROR] exception in Bitwuzla parser: %s\n", e.msg().c_str());
        } catch (const bitwuzla::Exception& e) {
            //// Remove the "invalid call to '...', prefix
            if (e.msg().find("invalid call") == 0) {
                const std::string& msg = e.msg();
                size_t pos             = msg.find("', ");
                LOG(V0_CRIT, "[ERROR] exception in Bitwuzla program: %s\n", msg.substr(pos+3).c_str());
            } else {
                LOG(V0_CRIT, "[ERROR] exception in Bitwuzla program: %s\n", e.msg().c_str());
            }
        } catch (...) {
            LOG(V0_CRIT, "[ERROR] uncaught exception in Bitwuzla program\n");
            abort();
        }

        if (smtOutFileSet) delete out;

        _result.id = _desc.getId();
        _result.revision = 0;
        _result.result = success ? 20 : 0;

        factory.reset(); // cleans up any dangling solvers
    }
};
