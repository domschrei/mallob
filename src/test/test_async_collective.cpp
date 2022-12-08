
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>
#include <map>

#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "comm/async_collective.hpp"
#include "util/sys/threading.hpp"

// String wrapper with concatenation as an aggregation operation.
// Note that this is a non-commutative operation.
struct ReduceableString : public Reduceable {
    std::string content;
    ReduceableString() = default;
    ReduceableString(const std::string& content) : content(content) {}
    virtual std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> result(content.size());
        memcpy(result.data(), content.c_str(), content.size());
        return result;
    }
    virtual ReduceableString& deserialize(const std::vector<uint8_t>& packed) override {
        content = std::string(packed.data(), packed.data()+packed.size());
        return *this;
    }
    virtual void aggregate(const Reduceable& other) {
        ReduceableString* otherStr = (ReduceableString*) &other;
        content += otherStr->content;
    }
    virtual bool isEmpty() const {
        return content.empty();
    }
};

// Wrapper for std::set<int> with union as an aggregation operation.
struct ReduceableIntSet : public Reduceable {
    std::set<int> set;
    ReduceableIntSet() = default;
    virtual std::vector<uint8_t> serialize() const override {
        size_t i = 0;
        size_t n = sizeof(int);
        std::vector<uint8_t> result(set.size()*n);
        for (auto& key : set) {
            memcpy(result.data()+i, &key, n); i += n;
        }
        return result;
    }
    virtual ReduceableIntSet& deserialize(const std::vector<uint8_t>& packed) override {
        set.clear();
        size_t i = 0;
        size_t n = sizeof(int);
        while (i < packed.size()) {
            int key;
            memcpy(&key, packed.data()+i, n); i += n;
            set.insert(key);
        }
        return *this;
    }
    virtual void aggregate(const Reduceable& other) {
        ReduceableIntSet* otherSet = (ReduceableIntSet*) &other;
        for (auto& key : otherSet->set) {
            set.insert(key);
        }
    }
    virtual bool isEmpty() const {
        return set.empty();
    }
};

// Wrapper for std::map<int, int> with union as an aggregation operation.
struct ReduceableIntToIntMap : public Reduceable {
    std::map<int, int> map;
    ReduceableIntToIntMap() = default;
    virtual std::vector<uint8_t> serialize() const override {
        size_t i = 0;
        size_t n = sizeof(int);
        std::vector<uint8_t> result(2*map.size()*n);
        for (auto& [key, val] : map) {
            memcpy(result.data()+i, &key, n); i += n;
            memcpy(result.data()+i, &val, n); i += n;
        }
        return result;
    }
    virtual ReduceableIntToIntMap& deserialize(const std::vector<uint8_t>& packed) override {
        map.clear();
        size_t i = 0;
        size_t n = sizeof(int);
        while (i < packed.size()) {
            int key, val;
            memcpy(&key, packed.data()+i, n); i += n;
            memcpy(&val, packed.data()+i, n); i += n;
            map[key] = val;
        }
        return *this;
    }
    virtual void aggregate(const Reduceable& other) {
        ReduceableIntToIntMap* otherMap = (ReduceableIntToIntMap*) &other;
        for (auto& [key, val] : otherMap->map) {
            map[key] = val;
        }
    }
    virtual bool isEmpty() const {
        return map.empty();
    }
};



// Counter for the IDs of AsyncCollective instances 
int reductionInstanceCounter = 1;
// Counter for the IDs of specific allReduce calls
int reductionCallCounter = 1;


// All-reduce a simple sum of integers.
void testIntegerSum() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();

    // Create all-reduction object
    AsyncCollective<ReduceableInt> allRed(comm, q, reductionInstanceCounter++);

    MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback
    
    // Local object to contribute
    ReduceableInt myContrib(rank);
    
    // Initiate all-reduction
    allRed.allReduce(reductionCallCounter++, myContrib, [&](auto& results) {
        auto& result = results.front();
        LOG(V2_INFO, "AllReduction done: result \"%i\"\n", result.content);
        assert(result.content == (MyMpi::size(comm)*(MyMpi::size(comm)-1)/2));
        Terminator::setTerminating();
    });

    // Poll message queue until everything is done
    while (!Terminator::isTerminating() || q.hasOpenSends()) q.advance();
}

// Concatenate a number of strings.
void testStringConcatenation() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();

    // Create all-reduction object
    AsyncCollective<ReduceableString> allRed(comm, q, reductionInstanceCounter++);
    
    MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback

    // Local object to contribute
    ReduceableString myContrib("{" + std::to_string(rank) + "}");
    std::string validateString;
    for (int r = 0; r < MyMpi::size(comm); r++) validateString += "{" + std::to_string(r) + "}";
    // Initiate all-reduction
    allRed.allReduce(reductionCallCounter++, myContrib, [&](auto& results) {
        auto& result = results.front();
        LOG(V2_INFO, "AllReduction done: result \"%s\"\n", result.content.c_str());
        assert(result.content == validateString);
        Terminator::setTerminating();
    });

    // Poll message queue until everything is done
    while (!Terminator::isTerminating() || q.hasOpenSends()) q.advance();
}

void testIntToIntMap() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();

    // Create all-reduction object
    AsyncCollective<ReduceableIntToIntMap> allRed(comm, q, reductionInstanceCounter++);

    MPI_Barrier(comm); // ensure all processes have a registered callback
    
    // Local object to contribute
    ReduceableIntToIntMap myContrib;
    myContrib.map[rank] = 1;
    // Initiate all-reduction
    allRed.allReduce(reductionCallCounter++, myContrib, [&](auto& results) {
        auto& result = results.front();
        LOG(V2_INFO, "AllReduction done\n");
        assert(result.map.size() == MyMpi::size(comm));
        for (size_t i = 0; i < MyMpi::size(comm); i++) {
            assert(result.map[i] == 1);
        }
        Terminator::setTerminating();
    });

    // Poll message queue until everything is done
    while (!Terminator::isTerminating() || q.hasOpenSends()) q.advance();
}

// Use a single AllReduction instance on each process and use it
// to perform multiple all-reductions at the same time
void testMultipleAllReductionCalls() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();
    int numAllReductions = 10;

    AsyncCollective<ReduceableInt> allred(comm, q, reductionInstanceCounter++);

    MPI_Barrier(comm); // ensure all processes have a registered callback

    int numFinished = 0;

    int contrib = 1;
    for (size_t i = 0; i < numAllReductions; i++) {
        allred.allReduce(reductionCallCounter++, contrib, [&, i](auto& results) {
            auto& result = results.front();
            LOG(V2_INFO, "AllReduction %i done: result \"%i\"\n", i, result.content);
            assert(result.content/MyMpi::size(comm) == i+1);
            numFinished++;
            if (numFinished == numAllReductions) Terminator::setTerminating();
        });
        contrib++;
    }

    // Poll message queue until everything is done
    while (!Terminator::isTerminating() || q.hasOpenSends()) q.advance();
}

// Use multiple AllReduction instances on each process and use each
// to perform one all-reduction (all at the same time)
void testMultipleAllReductionInstances() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();
    int numAllReductions = 10;

    std::list<AsyncCollective<ReduceableInt>> allreductions;
    for (size_t i = 0; i < numAllReductions; i++) {
        // Create all-reduction object
        allreductions.emplace_back(comm, q, reductionInstanceCounter++);
    }
    
    MPI_Barrier(comm); // ensure all processes have a registered callback

    int numFinished = 0;

    int contrib = 1;
    for (auto& allred : allreductions) {
        allred.allReduce(reductionCallCounter++, contrib, [&, contrib](auto& results) {
            auto& result = results.front();
            LOG(V2_INFO, "AllReduction %i done: result \"%i\"\n", contrib, result.content);
            assert(result.content == MyMpi::size(comm)*contrib);
            numFinished++;
            if (numFinished == numAllReductions) Terminator::setTerminating();
        });
        contrib++;
    }

    // Poll message queue until everything is done
    while (!Terminator::isTerminating() || q.hasOpenSends()) q.advance();
}

// Use multiple AllReduction instances on each process and use each
// to perform multiple all-reductions, all at the same time
void testMultipleAllReductionInstancesAndCalls() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();
    const int numInstances = 10;
    const int numCallsPerInstance = 10;

    std::list<AsyncCollective<ReduceableInt>> allreductions;
    for (size_t i = 0; i < numInstances; i++) {
        // Create all-reduction object
        allreductions.emplace_back(comm, q, reductionInstanceCounter++);
    }
    
    MPI_Barrier(comm); // ensure all processes have a registered callback

    int numFinished = 0;
    int contrib = 1;
    for (auto& allred : allreductions) {
        for (size_t i = 0; i < numCallsPerInstance; i++) {
            allred.allReduce(reductionCallCounter++, contrib, [&, contrib, i](auto& results) {
                auto& result = results.front();
                numFinished++;
                LOG(V2_INFO, "[%i/%i] AllReduction %i-%i done: result \"%i\"\n", 
                    numFinished, numInstances*numCallsPerInstance, contrib, i, result.content);
                assert(result.content == MyMpi::size(comm)*contrib);
                if (numFinished == numInstances*numCallsPerInstance) Terminator::setTerminating();
            });
        }
        contrib++;
    }

    // Poll message queue until everything is done
    while (!Terminator::isTerminating() || q.hasOpenSends()) q.advance();
}

void testIntegerPrefixSum() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();

    // Create all-reduction object
    AsyncCollective<ReduceableInt> allRed(comm, q, reductionInstanceCounter++);

    MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback
    
    // Local object to contribute
    ReduceableInt myContrib(rank);
    int numDone = 0; int numExpectedDone = 4;
    // Initiate prefix sums
    allRed.inclusivePrefixSum(reductionCallCounter++, myContrib, [&](auto& results) {
        assert(results.size() == 1);
        auto& result = results.front();
        LOG(V2_INFO, "InclusivePrefixSum done: result \"%i\"\n", result.content);
        numDone++;
        if (numDone == numExpectedDone) Terminator::setTerminating();
    });
    allRed.exclusivePrefixSum(reductionCallCounter++, myContrib, [&](auto& results) {
        assert(results.size() == 1);
        auto& result = results.front();
        LOG(V2_INFO, "ExclusivePrefixSum done: result \"%i\"\n", result.content);
        numDone++;
        if (numDone == numExpectedDone) Terminator::setTerminating();
    });
    allRed.inclAndExclPrefixSum(reductionCallCounter++, myContrib, [&](auto& results) {
        assert(results.size() == 2);
        LOG(V2_INFO, "InclExclPrefixSum done: result {%i,%i}\n", results.front().content, results.back().content);
        numDone++;
        if (numDone == numExpectedDone) Terminator::setTerminating();
    });
    allRed.inclAndExclPrefixSumWithTotal(reductionCallCounter++, myContrib, [&](auto& results) {
        assert(results.size() == 3);
        auto it = results.begin();
        int excl = it->content; ++it;
        int incl = it->content; ++it;
        int total = it->content;
        LOG(V2_INFO, "InclExclTotalPrefixSum done: result {%i,%i,%i}\n", excl, incl, total);
        numDone++;
        if (numDone == numExpectedDone) Terminator::setTerminating();
    });

    // Poll message queue until everything is done
    while (!Terminator::isTerminating() || q.hasOpenSends()) q.advance();
}

void testStringPrefixSum() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();

    // Create all-reduction object
    AsyncCollective<ReduceableString> allRed(comm, q, reductionInstanceCounter++);

    MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback
    
    // Local object to contribute
    ReduceableString myContrib("{" + std::to_string(rank) + "}");
    int numDone = 0; int numExpectedDone = 4;
    // Initiate prefix sums
    allRed.inclusivePrefixSum(reductionCallCounter++, myContrib, [&](auto& results) {
        assert(results.size() == 1);
        auto& result = results.front();
        LOG(V2_INFO, "InclusivePrefixSum done: result \"%s\"\n", result.content.c_str());
        numDone++;
        if (numDone == numExpectedDone) Terminator::setTerminating();
    });
    allRed.exclusivePrefixSum(reductionCallCounter++, myContrib, [&](auto& results) {
        assert(results.size() == 1);
        auto& result = results.front();
        LOG(V2_INFO, "ExclusivePrefixSum done: result \"%s\"\n", result.content.c_str());
        numDone++;
        if (numDone == numExpectedDone) Terminator::setTerminating();
    });
    allRed.inclAndExclPrefixSum(reductionCallCounter++, myContrib, [&](auto& results) {
        assert(results.size() == 2);
        LOG(V2_INFO, "InclExclPrefixSum done: result {%s,%s}\n", results.front().content.c_str(), results.back().content.c_str());
        numDone++;
        if (numDone == numExpectedDone) Terminator::setTerminating();
    });
    allRed.inclAndExclPrefixSumWithTotal(reductionCallCounter++, myContrib, [&](auto& results) {
        assert(results.size() == 3);
        auto it = results.begin();
        std::string excl = it->content; ++it;
        std::string incl = it->content; ++it;
        std::string total = it->content;
        LOG(V2_INFO, "InclExclTotalPrefixSum done: result {%s,%s,%s}\n", excl.c_str(), incl.c_str(), total.c_str());
        numDone++;
        if (numDone == numExpectedDone) Terminator::setTerminating();
    });

    // Poll message queue until everything is done
    while (!Terminator::isTerminating() || q.hasOpenSends()) q.advance();
}

void testSparsePrefixSum() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();

    // Create all-reduction object
    AsyncCollective<ReduceableIntSet> allRed(comm, q, reductionInstanceCounter++);

    MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback

    MPI_Request req = MPI_REQUEST_NULL;

    int numContributionsPerRank = 100;

    int numContributionsReturned = 0;
    int sumOfAllResults = 0;
    allRed.initializeSparsePrefixSum(1, /*delaySecs=*/0.001, [&](auto& results) {
        assert(results.size() == 3);
        auto it = results.begin();
        std::set<int> excl = it->set; ++it;
        std::set<int> incl = it->set; ++it;
        std::set<int> total = it->set;
        //LOG(V2_INFO, "SPARSE done: result {%s,%s,%s}\n", excl.c_str(), incl.c_str(), total.c_str());
        numContributionsReturned += incl.size() - excl.size();
        LOG(V2_INFO, "SPARSE done: %i/%i contributions returned\n", 
            numContributionsReturned, numContributionsPerRank);
        if (numContributionsReturned == numContributionsPerRank && req == MPI_REQUEST_NULL) {
            MPI_Ibarrier(comm, &req);
        }
    });

    Mutex mtx;

    std::thread contributor([&]() {
        ReduceableIntSet rSet;
        for (int i = 0; i < numContributionsPerRank; i++) {
            rSet.set.clear(); 
            rSet.set.insert(i*MyMpi::size(comm) + rank);
            auto lock = mtx.getLock();
            allRed.contributeToSparsePrefixSum(1, rSet);
        }
    });

    while (!Terminator::isTerminating() || q.hasOpenSends()) {
        {
            auto lock = mtx.getLock();
            q.advance();
            allRed.advanceSparseOperations();
        }
        if (req != MPI_REQUEST_NULL) {
            int flag; MPI_Test(&req, &flag, MPI_STATUS_IGNORE);
            if (flag) {
                Terminator::setTerminating();
            }
        }
    }
    contributor.join();
}

void testDifferentialSparsePrefixSum() {

    MPI_Comm comm = MPI_COMM_WORLD;
    int rank = MyMpi::rank(comm);
    int size = MyMpi::size(comm);
    auto& q = MyMpi::getMessageQueue();
    Terminator::reset();

    // Create all-reduction object
    AsyncCollective<ReduceableInt> allRed(comm, q, reductionInstanceCounter++);

    MPI_Request req = MPI_REQUEST_NULL;
    allRed.initializeDifferentialSparsePrefixSum(1, /*delay=*/0.001, [&](auto& results) {
        LOG(V2_INFO, "DiffPS done: %i, total: %i\n", results.front().content, results.back().content);
        int totalSum = results.back().content;
        if (totalSum == size * (size-1)) {
            LOG(V2_INFO, "All DiffPS done\n");
            MPI_Ibarrier(comm, &req);
        }
    });
    allRed.contributeToSparsePrefixSum(1, ReduceableInt(0));

    MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback

    float time = Timer::elapsedSeconds();

    while (!Terminator::isTerminating() || q.hasOpenSends()) {
        
        q.advance();
        allRed.advanceSparseOperations();

        if (Timer::elapsedSeconds() - time >= rank+1) {
            allRed.contributeToSparsePrefixSum(1, ReduceableInt{2*rank});
            time = 99999999;
        }

        if (req != MPI_REQUEST_NULL) {
            int flag; MPI_Test(&req, &flag, MPI_STATUS_IGNORE);
            if (flag) {
                Terminator::setTerminating();
            }
        }
    }
}

int main(int argc, char *argv[]) {

    MyMpi::init();
    Timer::init();
    int rank = MyMpi::rank(MPI_COMM_WORLD);

    Process::init(rank);

    Random::init(rand(), rand());
    Logger::init(rank, V5_DEBG);

    Parameters params;
    params.init(argc, argv);
    MyMpi::setOptions(params);

    /*
    testIntegerSum();
    testStringConcatenation();
    testIntToIntMap();
    testMultipleAllReductionCalls();
    testMultipleAllReductionInstances();
    testMultipleAllReductionInstancesAndCalls();
    testIntegerPrefixSum();
    testStringPrefixSum();
    testSparsePrefixSum();
    */

    testDifferentialSparsePrefixSum();

    // Exit properly
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    LOG(V2_INFO, "Exiting happily\n");
    Process::doExit(0);
}
