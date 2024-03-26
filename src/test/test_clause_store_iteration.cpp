
#include <assert.h>
#include <stdlib.h>
#include <set>
#include <vector>
#include <tuple>

#include "app/sat/sharing/store/static_clause_store.hpp"
#include "util/params.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/sys/process.hpp"
#include "util/option.hpp"


int main(int argc, char** argv) {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);

    Parameters params;
    params.init(argc, argv);

    typedef StaticClauseStore<false> Store;

    int nbBuckets = -1;
    std::vector<std::tuple<Store::BucketPriorityMode, Store::BucketPriorityMode, int, int>> configs = {
        {Store::LENGTH_FIRST, Store::LENGTH_FIRST, 20, 8},
        {Store::LENGTH_FIRST, Store::LBD_FIRST, 20, 8},
        {Store::LBD_FIRST, Store::LENGTH_FIRST, 20, 8},
        {Store::LBD_FIRST, Store::LBD_FIRST, 20, 8}
    };

    for (auto& [inner, outer, thresLength, thresLbd] : configs) {

        Store::BucketIndex index(params, inner, outer, thresLength, thresLbd);

        LOG(V2_INFO, "Iterating: inner=%i outer=%i threshold_length=%i threshold_lbd=%i max_length=%i max_lbd=%i\n",
            index.modeInner, index.modeOuter, index.thresholdLength, index.thresholdLbd, params.strictClauseLengthLimit(), params.strictLbdLimit());
        
        std::set<int> seenIndices;
        while (index.length <= params.strictClauseLengthLimit() && index.lbd <= params.strictLbdLimit()) {
            //LOG(V2_INFO, "%i : (%i, %i)\n", index.index, index.length, index.lbd);
            assert(!seenIndices.count(index.index));
            seenIndices.insert(index.index);
            index.next();
        }
        LOG(V2_INFO, "No duplicate indices, %i buckets\n", seenIndices.size());
        if (nbBuckets == -1) nbBuckets = seenIndices.size();
        else assert(seenIndices.size() == nbBuckets);
    }
}
