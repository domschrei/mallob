
#pragma once

#include <functional>
#include <string>
#include <fstream>

#include "comm/mympi.hpp"
#include "data/serializable.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/bloom_filter.hpp"
#include "app/sat/proof/lrat_line.hpp"
#include "app/sat/proof/serialized_lrat_line.hpp"
#include "app/sat/proof/lrat_utils.hpp"
#include "app/sat/proof/reverse_binary_lrat_parser.hpp"
#include "util/sys/buffered_io.hpp"
#include "merge_message.hpp"
#include "merge_child.hpp"
#include "proof_writer.hpp"
#include "util/small_merger.hpp"

class DistributedProofMerger {

public:
    Logger _log;

private:
    const static int FULL_CHUNK_SIZE_BYTES = 950'000;
    const static int HALF_CHUNK_SIZE_BYTES = FULL_CHUNK_SIZE_BYTES / 2;

    MPI_Comm _comm;
    int _branching_factor;
    MergeSourceInterface<SerializedLratLine>* _local_source;
    int _num_original_clauses = 0;
    
    bool _local_source_exhausted = false;
    bool _all_sources_exhausted = false;

    std::list<MergeChild> _children;
    MPI_Request _barrier_request;

    int _parent_rank = -1;
    bool _request_by_parent = false;
    bool _is_root;

    std::vector<SerializedLratLine> _output_buffer;
    Mutex _output_buffer_mutex;
    std::atomic_int _output_buffer_size = 0;
    bool _sentinel_ending_output {false};
    bool _binary_output = true;

    // rank zero only
    std::string _output_filename;
    std::unique_ptr<ProofWriter> _proof_writer;
    std::unique_ptr<BloomFilter<unsigned long>> _output_id_filter;
    std::future<void> _fut_root_prepare;
    bool _root_prepared = false;

    float lastOutputReport = 0;
    size_t numArrivedLines = 0;
    size_t numOutputLines = 0;

    std::future<void> _fut_merging;
    bool _began_merging = false;
    bool _began_final_barrier = false;
    bool _reversed_file = false;

    float _timepoint_merge_begin {0};
    float _time_inactive {0};

public:
    DistributedProofMerger(MPI_Comm comm, int branchingFactor, 
        MergeSourceInterface<SerializedLratLine>* localSource, const std::string& outputFileAtZero) : 
            _log(Logger::getMainInstance().copy("DFM", ".proofmerge")),
            _comm(comm), _branching_factor(branchingFactor), _local_source(localSource) {

        int myRank = MyMpi::rank(comm);
        _is_root = myRank == 0;

        setUpMergeTree();

        if (myRank == 0) {
            _fut_root_prepare = ProcessWideThreadPool::get().addTask([&, outputFileAtZero]() {
                // Create final output file
                _output_filename = outputFileAtZero;
                std::string reverseFilename = _output_filename + ".inv";
                LOGGER(_log, V3_VERB, "Opening output file \"%s\"\n", reverseFilename.c_str());
                _proof_writer.reset(new ProofWriter(reverseFilename, _binary_output));
                // TODO choose size relative to proof size
                _output_id_filter.reset(new BloomFilter<unsigned long>(268435399, 4));
                _root_prepared = true;
            });
        } else {
            MPI_Ibarrier(_comm, &_barrier_request);
        }
    }

    ~DistributedProofMerger() {
        if (_fut_root_prepare.valid()) _fut_root_prepare.get();
        if (_fut_merging.valid()) _fut_merging.get();
    }

    void setNumOriginalClauses(int numOriginalClauses) {
        _num_original_clauses = numOriginalClauses;
    }

    bool readyToMerge() {
        if (_began_merging) return false;
        if (_is_root) {
            if (!_root_prepared) return false;
            if (_fut_root_prepare.valid()) {
                // Root has been prepared for merging
                _fut_root_prepare.get();
                MPI_Ibarrier(_comm, &_barrier_request);
            } // else: root was already prepared
        }
        int flag;
        MPI_Test(&_barrier_request, &flag, MPI_STATUS_IGNORE);
        return flag;
    }

    void beginMerge() {
        _began_merging = true;
        _timepoint_merge_begin = Timer::elapsedSeconds();
        _fut_merging = ProcessWideThreadPool::get().addTask([&]() {
            doMerging();
            concludeMerging();
            reverseFile();
        });
    }

    bool beganMerging() const {
        return _began_merging;
    }

    void handle(int sourceWithinComm, MergeMessage& msg) {

        auto type = msg.type;
        if (type == MergeMessage::Type::REQUEST) {
            LOGGER(_log, V5_DEBG, "Msg from [%i] requesting lines\n", sourceWithinComm);
            _request_by_parent = true;
            advance();
            return;
        }

        LOGGER(_log, V5_DEBG, "Msg from [%i] responding to request\n", sourceWithinComm);

        bool foundChild = false;
        for (auto& child : _children) if (child.getRankWithinComm() == sourceWithinComm) {
            numArrivedLines += msg.lines.size();
            child.add(std::move(msg.lines));
            if (type == MergeMessage::Type::RESPONSE_EXHAUSTED) {
                child.conclude();
                LOGGER(_log, V3_VERB, "child [%i] marked exhausted\n", child.getRankWithinComm());
            }
            foundChild = true;
            break;
        }
        assert(foundChild);
    }

    void advance() {

        if (Timer::elapsedSeconds() - lastOutputReport > 1.0) {
            LOGGER(_log, V3_VERB, "narrv:%ld noutp:%ld eff:%.4f exh:%s\n", 
                numArrivedLines, numOutputLines, 
                1 - _time_inactive/(Timer::elapsedSeconds()-_timepoint_merge_begin), 
                _all_sources_exhausted ? "yes":"no");
            lastOutputReport = Timer::elapsedSeconds();
        }

        // Check if a request for a refill should be sent for some child
        for (auto& child : _children) {
            if (child.isRefillDesired()) {
                MergeMessage msg;
                msg.type = MergeMessage::REQUEST;
                MyMpi::isend(child.getRankWithinComm(), MSG_ADVANCE_DISTRIBUTED_FILE_MERGE, msg);
                child.setRefillRequested(true);
                LOGGER(_log, V5_DEBG, "Requesting refill from [%i]\n", child.getRankWithinComm());
            }
        }

        MergeMessage msg;
        auto writeOutputIntoMsg = [&]() {
            msg.lines = std::move(_output_buffer);
            _output_buffer.clear();
            _output_buffer_size.store(0, std::memory_order_relaxed);
            _sentinel_ending_output = false;
        };

        // Check if there is a refill request from the parent which can be fulfilled
        if (_request_by_parent) {
            msg.type = MergeMessage::Type::REQUEST;
            auto lock = _output_buffer_mutex.getLock();
            int lowerBound = _sentinel_ending_output ? 0 : 100;
            if (_output_buffer_size.load(std::memory_order_relaxed) >= lowerBound) {
                // Transfer to parent
                msg.type = MergeMessage::Type::RESPONSE_SUCCESS;
                writeOutputIntoMsg();
            } else if (_all_sources_exhausted) {
                // If all sources are exhausted, send the final buffer content and mark as exhausted
                msg.type = MergeMessage::Type::RESPONSE_EXHAUSTED;
                writeOutputIntoMsg();
            }
            if (msg.type != MergeMessage::Type::REQUEST) {
                LOGGER(_log, V5_DEBG, "Sending refill (%i lines) to [%i], exhausted:%s\n", 
                    msg.lines.size(), _parent_rank, 
                    msg.type == MergeMessage::Type::RESPONSE_EXHAUSTED ? "yes":"no");
                MyMpi::isend(_parent_rank, MSG_ADVANCE_DISTRIBUTED_FILE_MERGE, msg);
                _request_by_parent = false;
            }
        }
    }

    bool finished() const {
        if (!isFullyExhausted()) return false;
        if (_is_root) return _reversed_file;
        return true;
    }

    bool allProcessesFinished() {
        if (finished()) {
            if (!_began_final_barrier) {
                MPI_Ibarrier(_comm, &_barrier_request);
                _began_final_barrier = true;
            }
            int flag;
            MPI_Test(&_barrier_request, &flag, MPI_STATUS_IGNORE);
            return flag;
        }
        return false;
    }

private:

    void setUpMergeTree() {
        const int numChildrenOfRoot = 6;

        int myRank = MyMpi::rank(_comm);
        int commSize = MyMpi::size(_comm);
        _is_root = myRank == 0;

        int numChildRanks = commSize - 1;
        int numChildRanksPerTree = (int) std::ceil(((float)numChildRanks) / numChildrenOfRoot);
        int myTreeIdx = (myRank - 1) / numChildRanksPerTree;
        int rankOffset = _is_root ? 0 : 1 + myTreeIdx * numChildRanksPerTree;
        int myRankWithinTree = (myRank - 1) - numChildRanksPerTree * myTreeIdx;

        if (myRankWithinTree == 0) {
            // Root is the parent of each such child
            _parent_rank = 0;
        } else {
            _parent_rank = (myRankWithinTree-1) / _branching_factor + rankOffset;
        }

        LOGGER(_log, V3_VERB, "Tree #%i, internal rank %i, offset %i, parent [%i]\n", 
            myTreeIdx, myRankWithinTree, rankOffset, _parent_rank);

        // Compute children of this rank
        if (_is_root) {
            for (int i = 0; i < numChildrenOfRoot; i++) {
                int childRank = numChildRanksPerTree*i + 1;
                if (childRank < commSize) {
                    _children.emplace_back(childRank, FULL_CHUNK_SIZE_BYTES);
                    LOGGER(_log, V4_VVER, "Adding child [%i]\n", childRank);
                }
            }
        } else {
            for (int childRank = _branching_factor*myRankWithinTree+1; 
                    childRank <= _branching_factor*(myRankWithinTree+1); 
                    childRank++) {
                int adjChildRank = childRank + rankOffset;
                if (adjChildRank < std::min(commSize, 1 + (myTreeIdx+1) * numChildRanksPerTree)) {
                    // Create child
                    _children.emplace_back(adjChildRank, FULL_CHUNK_SIZE_BYTES);
                    LOGGER(_log, V4_VVER, "Adding child [%i]\n", adjChildRank);
                }
            }
        }
    }

    void doMerging() {

        assert(_num_original_clauses > 0);

        std::vector<MergeSourceInterface<SerializedLratLine>*> mergeSources;
        mergeSources.push_back(_local_source);
        for (auto& child : _children) {
            mergeSources.push_back(&child);
        }
        SmallMerger<SerializedLratLine> merger(mergeSources);
        
        std::vector<LratClauseId> hintsToDelete;
        SerializedLratLine bufferLine;
        float inactiveTimeStart = 0;

        while (!Terminator::isTerminating()) {
            
            if (_output_buffer_size >= FULL_CHUNK_SIZE_BYTES) {
                // Sleep (for simplicity; TODO use condition variable instead)
                if (inactiveTimeStart <= 0) inactiveTimeStart = Timer::elapsedSeconds();
                //LOGGER(_log, V5_DEBG, "cannot merge - waiting for input\n");
                usleep(1);
                continue;
            }

            bool success = merger.pollBlocking(bufferLine);
            if (!success) {
                // Merging is done!
                _all_sources_exhausted = true;
                break;
            }     

            if (inactiveTimeStart > 0) {
                _time_inactive += Timer::elapsedSeconds() - inactiveTimeStart;
                inactiveTimeStart = 0;
            }

            // Line to output found
            auto& chosenLine = bufferLine;
            auto chosenId = chosenLine.getId();

            if (_is_root) {
                if (chosenLine.isStub()) {
                    chosenLine.clear();
                    continue;
                }
                auto [ptr, numHints] = chosenLine.getUnsignedHints();
                for (size_t i = 0; i < numHints; i++) {
                    auto hint = ptr[i];
                    if (hint > _num_original_clauses &&
                            _output_id_filter->tryInsert(hint)) {
                        // Guarantee that ID was never output before.
                        // Can add a deletion line.
                        hintsToDelete.push_back(hint);
                    }
                }
                if (!hintsToDelete.empty()) {
                    _proof_writer->pushDeletionBlocking(chosenId, hintsToDelete);
                    hintsToDelete.clear();
                }
                // Write into final file
                _proof_writer->pushAdditionBlocking(chosenLine);
                chosenLine.clear();
            } else {
                // Write into output buffer
                auto lock = _output_buffer_mutex.getLock();
                _sentinel_ending_output = chosenLine.isStub();
                _output_buffer_size.fetch_add(chosenLine.size(), std::memory_order_relaxed);
                _output_buffer.emplace_back(std::move(chosenLine));
            }
            numOutputLines++;
        }

        if (inactiveTimeStart > 0) {
            _time_inactive += Timer::elapsedSeconds() - inactiveTimeStart;
            inactiveTimeStart = 0;
        }
    }

    void concludeMerging() {
        if (_is_root) {
            _proof_writer->markExhausted();
            while (!_proof_writer->isDone()) usleep(1000*10);
            _proof_writer.reset(); // internally waits for writer to finish
        }
    }

    void reverseFile() {
        if (!_is_root) return;
        if (_binary_output) {
            // Read binary file in reverse order, output lines into new file
            std::ofstream ofs(_output_filename, std::ofstream::binary);
            BufferedFileWriter writer(ofs);
            ReverseFileReader reader(_output_filename + ".inv");
            char c;
            while (reader.nextAsChar(c)) {
                writer.put(c);
            }
        } else {
            // Just "tac" the text file
            std::string cmd = "tac " + _output_filename + ".inv > " + _output_filename;
            int result = system(cmd.c_str());
            assert(result == 0);
        }
        // remove original (reversed) file
        std::string cmd = "rm " + _output_filename + ".inv";
        int result = system(cmd.c_str());
        assert(result == 0);

        _reversed_file = true;
    }

    bool isFullyExhausted() const {
        return _all_sources_exhausted && _output_buffer_size.load(std::memory_order_relaxed) == 0;
    }
};
