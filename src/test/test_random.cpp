
#include <vector>
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/params.hpp"

#include "app/sat/data/clause_histogram.hpp"

void testSelection(Parameters& params) {

    // abuse "clause histogram" for a histogram over how often each element was selected
    ClauseHistogram hist(1024);

    int maxReps = 100'000;
    for (int rep = 0; rep < maxReps; rep++) {

        // Generate elements
        std::vector<int> elems;
        for (size_t i = 0; i < 1024; i++) {
            elems.push_back(i);
        }

        // Select k
        int k = 16;
        SplitMix64Rng rng(params.seed()*maxReps+rep);
        auto rngLambda = [&]() {return ((double)rng()) / rng.max();};
        auto selection = random_choice_k_from_n(elems.data(), elems.size(), k, rngLambda);
        assert(selection.size() == 16);
        
        // Print out
        //std::string report;
        //for (auto& elem : selection) report += std::to_string(elem) + " ";
        //LOG(V2_INFO, "Selected elements: %s\n", report.c_str());

        for (int elem : selection) hist.increment(elem+1);
    }

    LOG(V2_INFO, "%s\n", hist.getReport().c_str());
}

int main(int argc, char *argv[]) {
    Timer::init();
    Parameters params;
    params.init(argc, argv);
    Random::init(params.seed(), params.seed());
    Logger::init(0, params.verbosity());

    testSelection(params);
}
