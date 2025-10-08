
#pragma once

#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/proof/trusted_parser_process_adapter.hpp"
#include "app/sat/stream/internal_sat_job_stream_processor.hpp"
#include "app/sat/stream/sat_job_stream_processor.hpp"
#include "app/sat/stream/wrapped_sat_job_stream.hpp"
#include "core/job_slot_registry.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"

class IncSatController {

private:
    const Parameters& _params;
    APIConnector& _api;
    JobDescription& _desc;
    std::string _problem_file;
    CoreAllocator::Allocation _core_alloc;

    std::string _name;

    int _rev {-1};
    std::unique_ptr<TrustedParserProcessAdapter> _tppa;

    static int getNextStreamId() {
        static int _stream_id = 1;
        return _stream_id++;
    }
    int _stream_id;

public:
    IncSatController(const Parameters& params, APIConnector& api, JobDescription& desc, const std::string& problemFile) :
            _params(params), _api(api), _desc(desc), _problem_file(problemFile), _core_alloc(1),
            _name("#" + std::to_string(desc.getId()) + "(ISAT)"), _stream_id(getNextStreamId()) {

        LOG(V2_INFO,"IncSatController #%i : %s\n", _desc.getId(), _name.c_str());

        if (!JobSlotRegistry::isInitialized()) JobSlotRegistry::init(params);
    }
    ~IncSatController() {
        LOG(V2_INFO, "Deleting IncSatController #%i\n", _desc.getId());
    }

    JobResult solve() {
        float _start_time = Timer::elapsedSeconds();

        _tppa.reset(new TrustedParserProcessAdapter(_params.seed(), _desc.getId()));
        _tppa->setup(_problem_file.c_str(), false);

        WrappedSatJobStream stream(_name);
        stream.mallobProcessor = new MallobSatJobStreamProcessor(_params, _api, _desc,
            _name, _stream_id, true, stream.stream.getSynchronizer());
        stream.stream.addProcessor(stream.mallobProcessor);
        LOG(V2_INFO, "New: %s\n", _name.c_str());

        if (_params.internalStreamProcessor()) {
            SolverSetup setup;
            setup.baseSeed = _params.seed();
            setup.jobId = _desc.getId();
            setup.isJobIncremental = true;
            setup.onTheFlyChecking = _params.onTheFlyChecking();
            setup.onTheFlyCheckModel = _params.onTheFlyCheckModel();
            auto internalProcessor = new InternalSatJobStreamProcessor(setup, stream.stream.getSynchronizer());
            stream.stream.addProcessor(internalProcessor);
        }

        stream.stream.setTerminator([&, params=&_params, desc=&_desc, startTime=_start_time]() {
            if (stream.stream.finalizing()) return true;
            return isTimeoutHit(params, desc, startTime);
        });

        while (!isTimeoutHit(&_params, &_desc, _start_time)
                && parseNextRevision()
                && !isTimeoutHit(&_params, &_desc, _start_time)) {
            LOG(V2_INFO, "Parsed next revision\n");
            std::vector<int> payload(_desc.getFormulaPayload(_rev), _desc.getFormulaPayload(_rev)+_desc.getFormulaPayloadSize(_rev));
            auto [res, sol] = stream.stream.solve({SatJobStreamProcessor::SatTask::Type::RAW, std::move(payload)});
            if (res == 0) break;
        }

        LOG(V2_INFO, "%s finalizing stream\n", _name.c_str());
        stream.stream.finalize();
        _tppa.reset();
        LOG(V2_INFO, "%s stream finalized\n", _name.c_str());

        LOG(V2_INFO, "%s done\n", _name.c_str());

        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = 10;
        return res;
    }

private:
    bool parseNextRevision() {
        _rev = 0;
        const std::string NC_DEFAULT_VAL = "BMMMKKK111";
        _desc.setAppConfigurationEntry("__NC", NC_DEFAULT_VAL);
        _desc.setAppConfigurationEntry("__NV", NC_DEFAULT_VAL);
        _desc.beginInitialization(_rev);

        uint8_t* sig;
        std::vector<unsigned char> plain;
        std::vector<unsigned char>* out;
        out = _desc.getRevisionData(_desc.getRevision()).get();

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