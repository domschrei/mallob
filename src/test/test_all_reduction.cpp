
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
#include "comm/all_reduction.hpp"


// Integer wrapper with addition as an aggregation operation.
struct ReduceableInt : public Reduceable {
    int content;
    ReduceableInt() = default;
    ReduceableInt(int content) : content(content) {}
    virtual std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> result(sizeof(int));
        memcpy(result.data(), &content, sizeof(int));
        return result;
    }
    virtual ReduceableInt& deserialize(const std::vector<uint8_t>& packed) override {
        memcpy(&content, packed.data(), sizeof(int));
        return *this;
    }
    virtual void aggregate(const Reduceable& other) {
        ReduceableInt* otherInt = (ReduceableInt*) &other;
        content += otherInt->content;
    }
    virtual bool isEmpty() const {
        return content == 0;
    }
};

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



// Counter for the IDs of AllReduction instances 
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
    AllReduction<ReduceableInt> allRed(comm, q, reductionInstanceCounter++);

    MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback
    
    // Local object to contribute
    ReduceableInt myContrib(rank);
    // Initiate all-reduction
    allRed.allReduce(reductionCallCounter++, myContrib, [&](auto& result) {
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
    AllReduction<ReduceableString> allRed(comm, q, reductionInstanceCounter++);
    
    MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback

    // Local object to contribute
    ReduceableString myContrib("{" + std::to_string(rank) + "}");
    std::string validateString;
    for (int r = 0; r < MyMpi::size(comm); r++) validateString += "{" + std::to_string(r) + "}";
    // Initiate all-reduction
    allRed.allReduce(reductionCallCounter++, myContrib, [&](auto& result) {
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
    AllReduction<ReduceableIntToIntMap> allRed(comm, q, reductionInstanceCounter++);

    MPI_Barrier(comm); // ensure all processes have a registered callback
    
    // Local object to contribute
    ReduceableIntToIntMap myContrib;
    myContrib.map[rank] = 1;
    // Initiate all-reduction
    allRed.allReduce(reductionCallCounter++, myContrib, [&](auto& result) {
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

    AllReduction<ReduceableInt> allred(comm, q, reductionInstanceCounter++);

    MPI_Barrier(comm); // ensure all processes have a registered callback

    int numFinished = 0;

    int contrib = 1;
    for (size_t i = 0; i < numAllReductions; i++) {
        allred.allReduce(reductionCallCounter++, contrib, [&, i](auto& result) {
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

    std::list<AllReduction<ReduceableInt>> allreductions;
    for (size_t i = 0; i < numAllReductions; i++) {
        // Create all-reduction object
        allreductions.emplace_back(comm, q, reductionInstanceCounter++);
    }
    
    MPI_Barrier(comm); // ensure all processes have a registered callback

    int numFinished = 0;

    int contrib = 1;
    for (auto& allred : allreductions) {
        allred.allReduce(reductionCallCounter++, contrib, [&, contrib](auto& result) {
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

    std::list<AllReduction<ReduceableInt>> allreductions;
    for (size_t i = 0; i < numInstances; i++) {
        // Create all-reduction object
        allreductions.emplace_back(comm, q, reductionInstanceCounter++);
    }
    
    MPI_Barrier(comm); // ensure all processes have a registered callback

    int numFinished = 0;
    int contrib = 1;
    for (auto& allred : allreductions) {
        for (size_t i = 0; i < numCallsPerInstance; i++) {
            allred.allReduce(reductionCallCounter++, contrib, [&, contrib, i](auto& result) {
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

    testIntegerSum();
    testStringConcatenation();
    testIntToIntMap();
    testMultipleAllReductionCalls();
    testMultipleAllReductionInstances();
    testMultipleAllReductionInstancesAndCalls();

    // Exit properly
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    LOG(V2_INFO, "Exiting happily\n");
    Process::doExit(0);
}
