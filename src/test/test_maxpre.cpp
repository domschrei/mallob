

#include "parserinterface.hpp"
#include "preprocessor.hpp"
#include "preprocessorinterface.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"

void testOneshot(const std::string& filename) {

    maxPreprocessor::ParserInterface parser;
    float time, timeParse, timePreprocess;
    {
        time = Timer::elapsedSeconds();
        std::ifstream ifs {filename};
        int res = parser.read_file_init_interface(ifs);
        timeParse = Timer::elapsedSeconds() - time;
        assert(res == 0);
    }
    time = Timer::elapsedSeconds();
    parser.preprocess("[bu]#[buvsrgcHTVGR]", 0, 120);
    timePreprocess = Timer::elapsedSeconds() - time;
    std::vector<int> formula;
    std::vector<std::pair<uint64_t, int>> objective;
    int nbVars, nbClauses;
    parser.getInstance(formula, objective, nbVars, nbClauses);
    LOG(V2_INFO, "MAXSAT MaxPRE stat lits:%i vars:%i cls:%i obj:%lu\n", formula.size(), nbVars, nbClauses, objective.size());
    LOG(V2_INFO, "MAXSAT MaxPRE time parse:%.3f preprocess:%.3f\n", timeParse, timePreprocess);
}

void testTwoshot(const std::string& filename) {

    maxPreprocessor::ParserInterface parser;
    float time, timeParse, timePreprocess;
    {
        time = Timer::elapsedSeconds();
        std::ifstream ifs {filename};
        int res = parser.read_file_init_interface(ifs);
        timeParse = Timer::elapsedSeconds() - time;
        assert(res == 0);
    }

    {
        time = Timer::elapsedSeconds();
        parser.preprocess("", 1, 0);
        timePreprocess = Timer::elapsedSeconds() - time;
        std::vector<int> formula;
        std::vector<std::pair<uint64_t, int>> objective;
        int nbVars, nbClauses;
        parser.getInstance(formula, objective, nbVars, nbClauses);
        LOG(V2_INFO, "MAXSAT MaxPRE(1) stat lits:%i vars:%i cls:%i obj:%lu\n", formula.size(), nbVars, nbClauses, objective.size());
        LOG(V2_INFO, "MAXSAT MaxPRE(1) time parse:%.3f preprocess:%.3f\n", timeParse, timePreprocess);
    }

    {
        time = Timer::elapsedSeconds();
        parser.preprocess("[bu]#[buvsrgcHTVGR]", 1, 10);
        timePreprocess = Timer::elapsedSeconds() - time;
        std::vector<int> formula;
        std::vector<std::pair<uint64_t, int>> objective;
        int nbVars, nbClauses;
        parser.getInstance(formula, objective, nbVars, nbClauses);
        LOG(V2_INFO, "MAXSAT MaxPRE(2) stat lits:%i vars:%i cls:%i obj:%lu\n", formula.size(), nbVars, nbClauses, objective.size());
        LOG(V2_INFO, "MAXSAT MaxPRE(2) time parse:%.3f preprocess:%.3f\n", timeParse, timePreprocess);
    }
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);
    ProcessWideThreadPool::init(1);

    auto instances = {
        "instances/wcnf/warehouses_wt-warehouse1.wcsp.wcnf",
        "instances/wcnf/normalized-ii8e1.wcnf",
        "instances/wcnf/drmx-cryptogen_wt-wolfram80_8.wcnf",
        "instances/wcnf/robot-navigation_8.wcnf",
        "instances/wcnf/CSG_wt-CSG140-140-46.wcnf",
        "instances/wcnf/mse24-exact-weighted/switchingactivitymaximization-SwitchingActivityMaximization_OpenRISC1200_200.wcnf"
    };

    LOG(V2_INFO, "*** testOneshot ***\n");
    for (auto file : instances) testOneshot(file);

    LOG(V2_INFO, "*** testTwoshot ***\n");
    for (auto file : instances) testTwoshot(file);
}
