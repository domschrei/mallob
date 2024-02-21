
#include "app/job.hpp"
#include "app/sat/parse/sat_reader.hpp"
#include "app/sat/proof/lrat_utils.hpp"
#include "app/sat/proof/serialized_lrat_line.hpp"
#include "app/sat/proof/trusted/lrat_checker.hpp"
#include "app/sat/proof/trusted/trusted_checker_process.hpp"
#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "app/sat/proof/trusted_parser_process_adapter.hpp"
#include "data/job_description.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/reverse_file_reader.hpp"
#include "util/sys/buffered_io.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"
#include <linux/prctl.h>
#include <sys/prctl.h>

int main(int argc, char** argv) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    Timer::init();
    Parameters params;
    Logger::LoggerConfig logConfig;
    logConfig.rank = 0;
    logConfig.verbosity = params.verbosity();
    logConfig.coloredOutput = params.coloredOutput();
    logConfig.flushFileImmediately = params.immediateFileFlush();
    logConfig.quiet = params.quiet();
    logConfig.cPrefix = true;
    Logger::init(logConfig);

    LOG(V2_INFO, "MallobSat standalone LRAT checker\n");

    const char* cnfInput = nullptr;
    const char* proofInput = nullptr;
    enum ProofReadMode {NORMAL, REVERSED} proofReadMode = NORMAL;

    for (int i = 1; i < argc; i++) {
        if (TrustedUtils::beginsWith(argv[i], "-reversed")
        || TrustedUtils::beginsWith(argv[i], "--reversed"))
            proofReadMode = REVERSED;
        else if (!cnfInput) cnfInput = argv[i];
        else if (!proofInput) proofInput = argv[i];
        else {
            LOG(V1_WARN, "[WARN] Extraneous argument \"%s\"\n", argv[i]);
        }
    }
    if (!cnfInput || !proofInput) {
        LOG(V0_CRIT, "Usage: %s <cnf-file> <proof-file>\n", argv[0]);
        exit(1);
    }

    float time = Timer::elapsedSeconds();
    SatReader reader(params, cnfInput);
    JobDescription* desc = new JobDescription();
    desc->setRevision(0);
    LOG(V2_INFO, "Parsing CNF %s ...\n", cnfInput);
    bool ok = reader.read(*desc);
    if (!ok) {
        LOG(V0_CRIT, "[ERROR] problem while parsing CNF!\n");
        exit(1);
    }
    time = Timer::elapsedSeconds() - time;
    LOG(V2_INFO, "Parsed CNF %s with %i variables, %i clauses; time %.3f\n",
        cnfInput, reader.getNbVars(), reader.getNbClauses(), time);

    time = Timer::elapsedSeconds();
    LratChecker chk(reader.getNbVars(), nullptr);
    ok = chk.loadOriginalClauses(desc->getFormulaPayload(0), desc->getFormulaPayloadSize(0));
    if (!ok) {
        LOG(V0_CRIT, "[ERROR] problem while loading CNF to LRAT checker! %s\n", chk.getErrorMessage());
        exit(1);
    }
    time = Timer::elapsedSeconds() - time;
    LOG(V2_INFO, "Loaded CNF to LRAT checker; time %.3f\n", time);

    // Delete formula to make space
    time = Timer::elapsedSeconds();
    delete desc;
    desc = nullptr;
    time = Timer::elapsedSeconds() - time;
    LOG(V2_INFO, "Deleted parsed CNF; time %.3f\n", time);

    LOG(V2_INFO, "Reading and traversing binary LRAT proof %s\n", proofInput);
    std::ifstream ifs;
    LinearFileReader* proofReader;
    if (proofReadMode == NORMAL) {
        ifs = std::ifstream(proofInput, std::ios::binary);
        proofReader = new BufferedFileReader(ifs);
    } else {
        proofReader = new ReverseFileReader(proofInput);
    }
    lrat_utils::ReadBuffer readbuf(*proofReader);

    unsigned long nbLines {0};
    unsigned long nbAdditions {0};
    unsigned long nbDeletions {0};
    unsigned long liveClauses = reader.getNbClauses();
    unsigned long maxLiveClauses = liveClauses;
    LratLine line;
    time = Timer::elapsedSeconds();
    bool failureFlag {false};
    while (lrat_utils::readLine(readbuf, line, &failureFlag)) {
        nbLines++;
        if (line.isDeletionStatement()) {
            nbDeletions += line.hints.size();
            if (liveClauses > maxLiveClauses) maxLiveClauses = liveClauses;
            liveClauses -= line.hints.size();
            ok = chk.deleteClause(line.hints.data(), line.hints.size());
            if (!ok) {
                LOG(V0_CRIT, "[ERROR] Problem with clause deletion.\n");
                LOG(V0_CRIT, "Offending line %i: %s", nbLines, line.toStr().c_str());
                LOG(V0_CRIT, "Checker message: %s\n", chk.getErrorMessage());
                exit(1);
            }
        } else {
            nbAdditions++;
            liveClauses++;
            ok = chk.addClause(line.id, line.literals.data(), line.literals.size(), line.hints.data(), line.hints.size());
            if (!ok) {
                LOG(V0_CRIT, "[ERROR] problem while adding clause derivation.\n");
                LOG(V0_CRIT, "Offending line %i: %s", nbLines, line.toStr().c_str());
                LOG(V0_CRIT, "Checker message: %s\n", chk.getErrorMessage());
                exit(1);
            }
        }
        if (nbLines % 1048576 == 0) {
            auto rss = Proc::getRecursiveProportionalSetSizeKbs(Proc::getPid());
            LOG(V2_INFO, "%lu lines passed; RAM usage: %.1f MB\n", nbLines, rss/1024.0);
        }
    }
    time = Timer::elapsedSeconds() - time;

    LOG(V2_INFO, "Done; %lu lines, %lu added cls, %lu deleted cls, %lu bottleneck cls; time %.3f (= %.1f lines/sec)\n",
        nbLines, nbAdditions, nbDeletions, maxLiveClauses, time, nbLines/std::max(0.0001f, time));
    LOG(V2_INFO, "%lu non-deleted clauses remaining\n", liveClauses);
    if (failureFlag) {
        LOG(V0_CRIT, "[ERROR] parsing error in line %i\n", nbLines+1);
        exit(1);
    }

    if (!chk.validateUnsat()) {
        LOG(V0_CRIT, "[ERROR] %s\n", chk.getErrorMessage());
        exit(1);
    }
    LOG_OMIT_PREFIX(V0_CRIT, "s VERIFIED\n");
    LOG(V2_INFO, "Exiting happily\n");
}
