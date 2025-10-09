
#pragma once

#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/proof/trusted_parser_process_adapter.hpp"
#include "app/sat/stream/internal_sat_job_stream_processor.hpp"
#include "app/sat/stream/sat_job_stream_garbage_collector.hpp"
#include "app/sat/stream/sat_job_stream_processor.hpp"
#include "app/sat/stream/wrapped_sat_job_stream.hpp"
#include "core/job_slot_registry.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/tmpdir.hpp"
#include <sys/stat.h>

class IncSatController {

private:
    const Parameters& _params;
    APIConnector& _api;
    JobDescription& _desc;
    std::string _problem_file;

    int _rev {-1};
    std::unique_ptr<TrustedParserProcessAdapter> _tppa;

    static int getNextStreamId() {
        static int _stream_id = 1;
        return _stream_id++;
    }
    int _stream_id;
    std::string _name;

    float _start_time = -1;
    std::unique_ptr<WrappedSatJobStream> _stream;

public:
    IncSatController(const Parameters& params, APIConnector& api, JobDescription& desc) :
            _params(params), _api(api), _desc(desc), _stream_id(getNextStreamId()),
            _name("#" + std::to_string(desc.getId()) + "(ISAT):" + std::to_string(_stream_id)) {

        LOG(V3_VERB, "+IncSat %s\n", _name.c_str());
        if (!JobSlotRegistry::isInitialized()) JobSlotRegistry::init(params);
    }
    ~IncSatController() {
        LOG(V3_VERB, "-IncSat %s\n", _name.c_str());
        finalize();
    }

    JobResult solveFromIncrementalFile(const std::string& problemFile) {
        _problem_file = problemFile;
        assert(!_stream);
        initStream(false);

        while (!isTimeoutHit(&_params, &_desc, _start_time)
                && parseNextRevision()
                && !isTimeoutHit(&_params, &_desc, _start_time)) {
            LOG(V3_VERB, "Parsed next revision\n");
            std::vector<int> payload(_desc.getFormulaPayload(_rev), _desc.getFormulaPayload(_rev)+_desc.getFormulaPayloadSize(_rev));
            auto [res, sol] = _stream->stream.solve({SatJobStreamProcessor::SatTask::Type::RAW, std::move(payload)});
            if (res == 0) break;
        }

        LOG(V4_VVER, "%s finalizing stream\n", _name.c_str());
        finalize();
        LOG(V3_VERB, "%s done\n", _name.c_str());

        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = 10;
        return res;
    }

    std::pair<int, std::vector<int>> solveNextRevision(std::vector<int>&& clauses, std::vector<int>&& assumptions) {
        if (!_stream) {
            if (_params.onTheFlyChecking())
                _problem_file = TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallob.incsattparse."
                    + std::to_string(_desc.getId()) + "." + std::to_string(_stream_id);
            initStream(true);
        }

        if (!_params.onTheFlyChecking()) {
            return _stream->stream.solve({SatJobStreamProcessor::SatTask::Type::SPLIT,
                std::move(clauses), std::move(assumptions)});
        }
        auto futWrite = ProcessWideThreadPool::get().addTask([&]() {
            // Output formula increment to the pipe file
            std::ofstream& ofs = _tppa->getFormulaToParserStream();
            for (int lit : clauses) {
                if (lit == 0) ofs << "0\n";
                else ofs << lit << " ";
            }
            ofs << "a ";
            for (int lit : assumptions) ofs << lit << " ";
            ofs << "0\n";
            ofs.flush();
        });
        bool ok = parseNextRevision();
        futWrite.get();
        assumptions.clear();
        if (!ok) {
            LOG(V1_WARN, "[WARN] %s parsing revision unsuccessful\n", _name.c_str());
            return {0, {}};
        }
        std::vector<int> payload(_desc.getFormulaPayload(_rev),
            _desc.getFormulaPayload(_rev)+_desc.getFormulaPayloadSize(_rev));
        return _stream->stream.solve({SatJobStreamProcessor::SatTask::Type::RAW, std::move(payload)});
    }

    std::function<bool()> _cb_terminate;
    void setInnerTerminator(std::function<bool()> cb) {
        _cb_terminate = cb;
    }

    void finalize() {
        if (!_stream) return;
        _stream->stream.finalize();
        SatJobStreamGarbageCollector::get().add(std::move(_stream));
        _tppa.reset();
    }

private:
    void initStream(bool createProblemFileAsPipe) {
        _start_time = Timer::elapsedSeconds();
        _stream.reset(new WrappedSatJobStream(_name));
        _stream->mallobProcessor = new MallobSatJobStreamProcessor(_params, _api, _desc,
            _name, _stream_id, true, _stream->stream.getSynchronizer());
        _stream->stream.addProcessor(_stream->mallobProcessor);

        if (_params.internalStreamProcessor()) {
            SolverSetup setup;
            setup.baseSeed = _params.seed();
            setup.jobId = _desc.getId();
            setup.jobname = _name + ".int";
            setup.isJobIncremental = true;
            setup.onTheFlyChecking = _params.onTheFlyChecking();
            setup.onTheFlyCheckModel = _params.onTheFlyChecking() && _params.onTheFlyCheckModel();
            auto internalProcessor = new InternalSatJobStreamProcessor(
                setup, _stream->stream.getSynchronizer());
            _stream->stream.addProcessor(internalProcessor);
        }

        _stream->stream.setTerminator([&, str=&_stream->stream, params=&_params, desc=&_desc, startTime=_start_time]() {
            if (str->finalizing()) return true;
            if (_cb_terminate && _cb_terminate()) return true;
            return isTimeoutHit(params, desc, startTime);
        });

        if (!_problem_file.empty()) {
            _tppa.reset(new TrustedParserProcessAdapter(_params.seed(), _name));
            _tppa->setup(_problem_file.c_str(), createProblemFileAsPipe);
        }
    }
    bool parseNextRevision() {
        _rev = 0;
        const std::string NC_DEFAULT_VAL = "BMMMKKK111";
        _desc.setAppConfigurationEntry("__NC", NC_DEFAULT_VAL);
        _desc.setAppConfigurationEntry("__NV", NC_DEFAULT_VAL);
        _desc.beginInitialization(_rev);

        uint8_t* sig;
        std::vector<unsigned char>* out;
        out = _desc.getRevisionData(_desc.getRevision()).get();

        assert(_tppa);
        bool ok = _tppa->parseAndSign(*out, sig);
        if (!ok) return false;

        std::string sigStr = Logger::dataToHexStr(sig, SIG_SIZE_BYTES);
        int _max_var = _tppa->getNbVars();
        int _num_read_clauses = _tppa->getNbClauses();
        _desc.setFSize(_tppa->getFSize());
        LOG(V2_INFO, "IMPCHK parser -key-seed=%lu read %i vars, %i cls, %i asmpt - sig %s\n",
            ImpCheck::getKeySeed(_params.seed()), _max_var, _num_read_clauses, _tppa->getNbAssumptions(), sigStr.c_str());

        // Store # variables and # clauses in app config
        std::vector<std::pair<int, std::string>> fields {
            {_num_read_clauses, "__NC"},
            {_max_var, "__NV"}
        };
        for (auto [nbRead, dest] : fields) {
            std::string nbStr = std::to_string(nbRead);
            assert(nbStr.size() < NC_DEFAULT_VAL.size());
            while (nbStr.size() < NC_DEFAULT_VAL.size())
                nbStr += ".";
            _desc.setAppConfigurationEntry(dest, nbStr);
        }

        _desc.endInitialization();
        return true;
    }

    bool isTimeoutHit(const Parameters* params, JobDescription* desc, float startTime) const {
        if (Terminator::isTerminating())
            return true;
        if (params->timeLimit() > 0 && Timer::elapsedSeconds() >= params->timeLimit())
            return true;
        if (desc->getWallclockLimit() > 0 && (Timer::elapsedSeconds() - startTime) >= desc->getWallclockLimit())
            return true;
        return false;
    }
};