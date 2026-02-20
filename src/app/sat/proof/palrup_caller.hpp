
#pragma once

#include "comm/mympi.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/assert.hpp"
#include "util/sys/fileutils.hpp"

class PalRupCaller {

private:
    const Parameters& _params;
    const int _job_id;
    const int _global_num_workers;

public:
    PalRupCaller(const Parameters& params, int jobId, int globalNumWorkers) :
        _params(params), _job_id(jobId), _global_num_workers(globalNumWorkers) {}

    void callBlocking() {
        assert(_params.regularProcessDistribution());
        assert(_params.logDirectory.isSet());
        assert(_params.proofDirectory.isSet());
        assert(_params.palRupCheckWorkdir.isSet());

        const int nbProcsPerHost = _params.processesPerHost();
        const int nbHosts = _global_num_workers / nbProcsPerHost;
        const int nbSolvers = _params.numThreadsPerProcess() * _global_num_workers;
        const std::string proofInputDir = FileUtils::getAbsoluteFilePath(_params.proofDirectory()
            + "/proof#" + std::to_string(_job_id) + "/");
        const std::string proofWorkingDir = FileUtils::getAbsoluteFilePath(_params.palRupCheckWorkdir());
        const std::string logDir = FileUtils::getAbsoluteFilePath(_params.logDirectory());
        FileUtils::mkdir(proofWorkingDir);

        std::string palRupCall = "cd palrup;"
            " NUM_SOLVERS=" + std::to_string(nbSolvers)
            + " NUM_NODES=" + std::to_string(nbHosts)
            + " NUM_PROCS_PER_NODE=" + std::to_string(nbProcsPerHost)
            // FIXME replace monoFilename with path to *this specific job's* description
            + " FORMULA_PATH=\"" + FileUtils::getAbsoluteFilePath(_params.monoFilename()) + "\""
            + " PROOF_PALRUP=\"" + proofInputDir + "\""
            + " PROOF_WORKING=\"" + proofWorkingDir + "\""
            + " LOG_DIR=\"" + logDir + "\""
            + " bash scripts/pal_launcher.sh 2>&1 > "
            + logDir + "/palrup." + std::to_string(_job_id)
                + "." + std::to_string(MyMpi::rank(MPI_COMM_WORLD)) + ".out";

        LOG(V4_VVER, "Calling PalRUP checker: %s\n", palRupCall.c_str());
        int retval = system(palRupCall.c_str());
        LOG(V4_VVER, "PalRUP checker returned, retval=%i\n", retval);
    }
};
