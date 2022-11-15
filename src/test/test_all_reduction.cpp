
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

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
    virtual void merge(const Reduceable& other) {
        ReduceableInt* otherInt = (ReduceableInt*) &other;
        content += otherInt->content;
    }
    virtual bool isEmpty() const {
        return content == 0;
    }
};

// String wrapper with concatenation as an aggregation operation.
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
    virtual void merge(const Reduceable& other) {
        ReduceableString* otherStr = (ReduceableString*) &other;
        content += otherStr->content;
    }
    virtual bool isEmpty() const {
        return content.empty();
    }
};

void testAllReduction() {

    int rank = MyMpi::rank(MPI_COMM_WORLD);
    auto& q = MyMpi::getMessageQueue();
    MPI_Comm comm = MPI_COMM_WORLD;

    // Integer sum
    {
        Terminator::reset();
        q.clearCallbacks();

        // Create all-reduction object
        AllReduction<ReduceableInt> allRed(comm);
        // Register required callbacks
        q.registerCallback(MSG_ALL_REDUCTION_UP, [&](auto& h) {allRed.handle(h);});
        q.registerCallback(MSG_ALL_REDUCTION_DOWN, [&](auto& h) {allRed.handle(h);});

        MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback
        
        // Local object to contribute
        ReduceableInt myContrib(rank);
        // Initiate all-reduction
        allRed.allReduce(myContrib, [&](auto& result) {
            LOG(V2_INFO, "AllReduction done: result \"%i\"\n", result.content);
            Terminator::setTerminating();
        });

        // Poll message queue until everything is done
        while (!Terminator::isTerminating()) q.advance();
    }

    // String concatenation (not commutative, hence result is nondeterministic)
    {
        Terminator::reset();
        q.clearCallbacks();

        // Create all-reduction object
        AllReduction<ReduceableString> allRed(comm);
        // Register required callbacks
        q.registerCallback(MSG_ALL_REDUCTION_UP, [&](auto& h) {allRed.handle(h);});
        q.registerCallback(MSG_ALL_REDUCTION_DOWN, [&](auto& h) {allRed.handle(h);});
        
        MPI_Barrier(MPI_COMM_WORLD); // ensure all processes have a registered callback

        // Local object to contribute
        ReduceableString myContrib("{" + std::to_string(rank) + "}");
        // Initiate all-reduction
        allRed.allReduce(myContrib, [&](auto& result) {
            LOG(V2_INFO, "AllReduction done: result \"%s\"\n", result.content.c_str());
            Terminator::setTerminating();
        });

        // Poll message queue until everything is done
        while (!Terminator::isTerminating()) q.advance();
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

    testAllReduction();

    // Exit properly
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    LOG(V2_INFO, "Exiting happily\n");
    Process::doExit(0);
}
