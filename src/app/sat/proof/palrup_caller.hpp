
#pragma once

#include "comm/mympi.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/assert.hpp"
#include "util/sys/fileutils.hpp"
#include <unistd.h>

class PalRupCaller {

private:
    const Parameters& _params;
    const int _global_num_workers;
    const std::string _cnf_path;
    const std::string _proofdir;

public:
    PalRupCaller(const Parameters& params, int globalNumWorkers, const std::string& cnfPath, const std::string& proofDir) :
        _params(params), _global_num_workers(globalNumWorkers), _cnf_path(cnfPath), _proofdir(proofDir) {}

    enum PalRupResult {DONE, VALIDATED, ERROR};
    PalRupResult callBlocking() {

        assert(_params.regularProcessDistribution());
        assert(_params.logDirectory.isSet());
        assert(_params.proofDirectory.isSet());
        assert(_params.palRupCheckWorkdir.isSet());

        const int nbProcsPerHost = _params.processesPerHost();
        const int nbHosts = _global_num_workers / nbProcsPerHost;
        const int nbSolvers = _params.numThreadsPerProcess() * _global_num_workers;
        const int jwl = _params.jobWallclockLimit();
        const std::string proofInputDir = FileUtils::getAbsoluteFilePath(_proofdir);
        const std::string proofWorkingDir = FileUtils::getAbsoluteFilePath(_params.palRupCheckWorkdir());
        const std::string logDir = FileUtils::getAbsoluteFilePath(_params.logDirectory());
        FileUtils::mkdir(proofWorkingDir);

        auto fileSuccess = logDir + "/success.palrup";
        auto fileFailure = logDir + "/failure.palrup";
        if (FileUtils::isRegularFile(fileSuccess)) {
            LOG(V0_CRIT, "[ERROR] PalRUP success file exists before starting a checker!\n");
            return ERROR;
        }
        if (FileUtils::isRegularFile(fileFailure)) {
            LOG(V0_CRIT, "[ERROR] PalRUP failure file discovered immediately\n");
            return ERROR;
        }

        std::string palRupCall = "cd lib/palrup;"
            " NUM_SOLVERS=" + std::to_string(nbSolvers)
            + " NUM_NODES=" + std::to_string(nbHosts)
            + " NUM_PROCS_PER_NODE=" + std::to_string(nbProcsPerHost)
            // FIXME replace monoFilename with path to *this specific job's* description
            + " FORMULA_PATH=\"" + FileUtils::getAbsoluteFilePath(_cnf_path) + "\""
            + " PROOF_PALRUP=\"" + proofInputDir + "\""
            + " PROOF_WORKING=\"" + proofWorkingDir + "\""
            + " LOG_DIR=\"" + logDir + "\""
            + " TIMEOUT=" + std::to_string(jwl > 0 ? jwl : 9999999)
            + " bash scripts/pal_launcher.sh";

        LOG(V4_VVER, "Calling PalRUP checker: %s\n", palRupCall.c_str());
        const int retval = system(palRupCall.c_str());
        LOG(V4_VVER, "PalRUP checker returned, retval=%i\n", retval);

        if (retval != 0) {
            FileUtils::create(fileFailure);
            return ERROR;
        }
        if (FileUtils::isRegularFile(fileSuccess)) {
            LOG(V2_INFO, "PalRUP VALIDATED UNSAT\n");
            return VALIDATED;
        }
        return DONE;
    }
};
