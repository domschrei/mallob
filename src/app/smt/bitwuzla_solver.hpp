
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
    }
    ~BitwuzlaSolver() {
        LOG(V2_INFO, "Deleting SMT Bitwuzla+Mallob #%i\n", _desc.getId());
    }

    JobResult solve() {
        _start_time = Timer::elapsedSeconds();

        bitwuzla::Options options;
        bitwuzla::TermManager tm;

        auto out = &std::cout;
        bool smtOutFileSet = false;
        if (_desc.getAppConfiguration().map.count("smt-out-file")) {
            out = new std::ofstream(_desc.getAppConfiguration().map["smt-out-file"]);
            smtOutFileSet = true;
        } else if (_params.smtOutputFile.isSet()) {
            out = new std::ofstream(getSmtOutputFilePath(_params, _desc.getId()));
            smtOutFileSet = true;
        }

        // Default top-level Bitwuzla options
        bool print_no_letify = false, print_formula = false, pp_only = false, parse_only = false;
        uint8_t bv_format = 2;
        std::string language = "smt2";

        // Parse Bitwuzla options
        std::string bzlaArgsString = _params.bitwuzlaArgs();
        if (_desc.getAppConfiguration().map.count("smt-args"))
            bzlaArgsString += _desc.getAppConfiguration().map["smt-args"];
        std::stringstream ss(bzlaArgsString);
        string arg;
        std::vector<std::string> opts;
        while (!bzlaArgsString.empty() && getline(ss, arg, ',')) {
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
            else if (rhs.empty() && lhs == "print-model")
                options.set(bitwuzla::Option::PRODUCE_MODELS, 1);
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
        if (endTime < INT32_MAX)
            options.set(bitwuzla::Option::TIME_LIMIT_PER, 1000.f * (endTime - Timer::elapsedSeconds()));

        DTaskTracker dTaskTracker(_params);
        std::unique_ptr<BitwuzllobSatSolverFactory> factory;

        try {
            //bzla::main::set_time_limit(main_options.time_limit);

            *out << bitwuzla::set_bv_format(bv_format);
            *out << bitwuzla::set_letify(!print_no_letify);

            factory = std::make_unique<BitwuzllobSatSolverFactory>(
                _params, _api, _desc, dTaskTracker,
                _terminator, _name, options);

            bitwuzla::parser::Parser parser(
                tm, *factory.get(), options, language, out);
            parser.configure_auto_print_model(options.get(bitwuzla::Option::PRODUCE_MODELS));
            parser.configure_terminator(&_terminator);
            parser.parse(
                _problem_file,
                print_formula || pp_only || parse_only
            );
            //bzla::main::reset_time_limit();
            auto bitwuzla = parser.bitwuzla();

            if (pp_only) bitwuzla->simplify();
            if (print_formula) {
                if (!parse_only && !pp_only) bitwuzla->simplify();
                bitwuzla->print_formula(*out, "smt2");
            }
            if (options.get(bitwuzla::Option::PRODUCE_UNSAT_CORES))
                bitwuzla->print_unsat_core(*out);

            if (options.get(bitwuzla::Option::VERBOSITY)) {
                auto stats = bitwuzla->statistics();
                for (auto& [name, val] : stats) {
                    *out << name << ": " << val << std::endl;
                }
            }

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

        factory.reset(); // cleans up any dangling solvers

        if (smtOutFileSet) delete out;

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

    static bool isTimeoutHit(const Parameters* params, JobDescription* desc, float startTime) {
        if (Terminator::isTerminating())
            return true;
        if (Timer::elapsedSeconds() > getEndTime(params, desc, startTime))
            return true;
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
};
