
#pragma once

#include <functional>
#include <string>
#include <fstream>

#include "mympi.hpp"
#include "data/serializable.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/bloom_filter.hpp"
#include "app/sat/proof/lrat_line.hpp"
#include "app/sat/proof/serialized_lrat_line.hpp"
#include "app/sat/proof/lrat_utils.hpp"
#include "app/sat/proof/reverse_binary_lrat_parser.hpp"

class DistributedFileMerger {

public:
    typedef std::function<std::optional<SerializedLratLine>()> LineSource;

    struct MergeMessage : public Serializable {

        enum Type {REQUEST, RESPONSE_SUCCESS, RESPONSE_EXHAUSTED} type;
        std::vector<SerializedLratLine> lines;

        static Type getTypeOfMessage(const std::vector<uint8_t>& serializedMsg) {
            Type type;
            memcpy(&type, serializedMsg.data(), sizeof(Type));
            return type;
        }        

        virtual std::vector<uint8_t> serialize() const {

            std::vector<uint8_t> result;
            size_t i = 0, n;

            result.resize(sizeof(Type));
            n = sizeof(Type); memcpy(result.data()+i, &type, n); i += n;

            for (const auto& line : lines) {
                result.insert(result.end(), line.data().begin(), line.data().end());
            }

            return result;
        }

        virtual Serializable& deserialize(const std::vector<uint8_t>& packed) {

            size_t i = 0, n;
            n = sizeof(Type); memcpy(&type, packed.data()+i, n); i += n;

            while (i < packed.size()) {

                n = sizeof(int);
                auto offsetNumLits = SerializedLratLine::getDataPosOfNumLits();
                int numLits;
                memcpy(&numLits, packed.data()+i+offsetNumLits, n);
                auto offsetNumHints = SerializedLratLine::getDataPosOfNumHints(numLits);
                int numHints;
                memcpy(&numHints, packed.data()+i+offsetNumHints, n);

                int lineSize = SerializedLratLine::getSize(numLits, numHints);
                SerializedLratLine line(std::vector<uint8_t>(
                    packed.data()+i,
                    packed.data()+i+lineSize
                ));

                i += lineSize;
                lines.push_back(std::move(line));
            }

            return *this;
        }
    };

private:
    const static int FULL_CHUNK_SIZE_BYTES = 950'000;
    const static int HALF_CHUNK_SIZE_BYTES = FULL_CHUNK_SIZE_BYTES / 2;

    MPI_Comm _comm;
    int _branching_factor;
    LineSource _local_source;
    int _num_original_clauses;
    
    bool _local_source_exhausted = false;
    bool _all_sources_exhausted = false;

    class MergeChild {
    private:
        int rankWithinComm;
        std::list<SerializedLratLine> buffer;
        std::atomic_int bufferSize = 0;
        bool exhausted = false;
        bool refillRequested = false;
        Mutex bufferMutex;
    public:
        MergeChild(int rankWithinComm) : rankWithinComm(rankWithinComm) {}

        int getRankWithinComm() const {return rankWithinComm;}
        bool isEmpty() const {return bufferSize.load(std::memory_order_relaxed) == 0;}
        bool isExhausted() const {return exhausted;}
        bool hasNext() const {return !isEmpty();}
        bool isRefillDesired() const {
            return !isExhausted() && !refillRequested 
                && bufferSize.load(std::memory_order_relaxed) < FULL_CHUNK_SIZE_BYTES; 
        }

        void add(std::vector<SerializedLratLine>&& newLines) {
            auto lock = bufferMutex.getLock();
            int pos = 0;
            for (auto& line : newLines) {
                bufferSize += line.size();
                buffer.push_back(std::move(line));
            }
            refillRequested = false;
        }
        SerializedLratLine next() {
            assert(hasNext());
            auto lock = bufferMutex.getLock();
            auto line = std::move(buffer.front());
            buffer.pop_front();
            bufferSize.fetch_sub(line.size(), std::memory_order_relaxed);
            return line;
        }
        void conclude() {exhausted = true;}
        void setRefillRequested(bool requested) {refillRequested = requested;}
    };

    std::list<MergeChild> _children;
    MPI_Request _barrier_request;

    int _parent_rank = -1;
    bool _request_by_parent = false;

    std::vector<SerializedLratLine> _output_buffer;
    Mutex _output_buffer_mutex;
    std::atomic_int _output_buffer_size = 0;
    bool _binary_output = true;

    // rank zero only
    std::string _output_filename;
    std::ofstream _output_filestream;
    std::unique_ptr<BloomFilter<unsigned long>> _output_id_filter;
    std::future<void> _fut_root_prepare;
    bool _root_prepared = false;

    size_t numOutputLines = 0;
    float lastOutputReport = 0;

    std::future<void> _fut_merging;
    bool _began_merging = false;
    bool _began_final_barrier = false;
    bool _reversed_file = false;

    float _timepoint_merge_begin;
    float _time_active = 0;

public:
    DistributedFileMerger(MPI_Comm comm, int branchingFactor, LineSource localSource, const std::string& outputFileAtZero, int numOriginalClauses) : 
            _comm(comm), _branching_factor(branchingFactor), _local_source(localSource), _num_original_clauses(numOriginalClauses) {

        int myRank = MyMpi::rank(comm);
        int commSize = MyMpi::size(comm);

        if (myRank > 0) {
            _parent_rank = (myRank-1) / branchingFactor;
            LOG(V3_VERB, "DFM Adding parent [%i]\n", _parent_rank);
        }

        // Compute children of this rank
        for (int childRank = branchingFactor*myRank+1; childRank <= branchingFactor*(myRank+1); childRank++) {
            if (childRank < commSize) {
                // Create child
                _children.emplace_back(childRank);
                LOG(V3_VERB, "DFM Adding child [%i]\n", childRank);
            }
        }

        if (myRank == 0) {
            _fut_root_prepare = ProcessWideThreadPool::get().addTask([&, outputFileAtZero]() {
                // Create final output file
                _output_filename = outputFileAtZero;
                std::string reverseFilename = _output_filename + ".inv";
                LOG(V3_VERB, "DFM Opening output file \"%s\"\n", reverseFilename.c_str());
                if (_binary_output) {
                    _output_filestream = std::ofstream(reverseFilename, std::ofstream::binary);
                } else {
                    _output_filestream = std::ofstream(reverseFilename);
                }
                // TODO choose size relative to proof size
                _output_id_filter.reset(new BloomFilter<unsigned long>(26843543, 4));
                _root_prepared = true;
            });
        } else {
            MPI_Ibarrier(_comm, &_barrier_request);
        }
    }

    ~DistributedFileMerger() {
        if (_fut_root_prepare.valid()) _fut_root_prepare.get();
        if (_fut_merging.valid()) _fut_merging.get();
    }

    bool readyToMerge() {
        if (_began_merging) return false;
        if (MyMpi::rank(_comm) == 0) {
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
            reverseFile();
        });
    }

    bool beganMerging() const {
        return _began_merging;
    }

    void handle(int sourceWithinComm, MergeMessage& msg) {

        LOG(V3_VERB, "DFM Msg from [%i]\n", sourceWithinComm);

        auto type = msg.type;
        if (type == MergeMessage::Type::REQUEST) {
            _request_by_parent = true;
            advance();
            return;
        }

        bool foundChild = false;
        for (auto& child : _children) if (child.getRankWithinComm() == sourceWithinComm) {
            child.add(std::move(msg.lines));
            if (type == MergeMessage::Type::RESPONSE_EXHAUSTED) {
                child.conclude();
            }
            foundChild = true;
            break;
        }
        assert(foundChild);
    }

    void advance() {

        if (Timer::elapsedSeconds() - lastOutputReport > 1.0) {
            LOG(V3_VERB, "DFM outputlines:%i efficiency:%.4f exhausted:%s\n", 
                numOutputLines, 
                _time_active/(Timer::elapsedSeconds()-_timepoint_merge_begin), 
                areInputsExhausted()?"yes":"no");
            lastOutputReport = Timer::elapsedSeconds();
        }

        // Check if a request for a refill should be sent for some child
        for (auto& child : _children) {
            if (child.isRefillDesired()) {
                MergeMessage msg;
                msg.type = MergeMessage::REQUEST;
                MyMpi::isend(child.getRankWithinComm(), MSG_ADVANCE_DISTRIBUTED_FILE_MERGE, msg);
                child.setRefillRequested(true);
                LOG(V3_VERB, "DFM Requesting refill from [%i]\n", child.getRankWithinComm());
            }
        }

        MergeMessage msg;
        auto writeOutputIntoMsg = [&]() {
            auto lock = _output_buffer_mutex.getLock();
            msg.lines = std::move(_output_buffer);
            _output_buffer.clear();
            _output_buffer_size.store(0, std::memory_order_relaxed);
        };

        // Check if there is a refill request from the parent which can be fulfilled
        if (_request_by_parent) {
            msg.type = MergeMessage::Type::REQUEST;
            if (_output_buffer_size.load(std::memory_order_relaxed) >= 100) {
                // Transfer to parent
                msg.type = MergeMessage::Type::RESPONSE_SUCCESS;
                writeOutputIntoMsg();
            } else if (_all_sources_exhausted) {
                // If all sources are exhausted, send the final buffer content and mark as exhausted
                msg.type = MergeMessage::Type::RESPONSE_EXHAUSTED;
                writeOutputIntoMsg();
            }
            if (msg.type != MergeMessage::Type::REQUEST) {
                LOG(V3_VERB, "DFM Sending refill (%i lines) to [%i]\n", msg.lines.size(), _parent_rank);
                MyMpi::isend(_parent_rank, MSG_ADVANCE_DISTRIBUTED_FILE_MERGE, msg);
                _request_by_parent = false;
            }
        }
    }

    bool finished() const {
        if (!isFullyExhausted()) return false;
        if (MyMpi::rank(_comm) == 0) {
            return _reversed_file;
        } else return true;
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
    void doMerging() {

        std::vector<SerializedLratLine> merger(_children.size()+1);
        std::vector<LratClauseId> hintsToDelete;

        bool countedRefillTime = false;

        while (!Terminator::isTerminating()) {
            bool canMerge = true;
            
            float time = countedRefillTime ? 0 : Timer::elapsedSeconds();

            // Refill next line from each child as necessary 
            auto childIt = _children.begin();
            for (size_t i = 0; i < _children.size(); i++) {

                if (!merger[i].valid()) {
                    auto& child = *childIt;
                    if (child.hasNext()) {
                        merger[i] = child.next();
                    } else if (!child.isExhausted()) {
                        // Child COULD have more lines but they are not available.
                        canMerge = false;
                    }
                }

                ++childIt;
            }

            if (!merger.back().valid() && !_local_source_exhausted) {
                // Refill from local source
                auto optLine = _local_source();
                if (!optLine.has_value()) {
                    _local_source_exhausted = true;
                } else {
                    merger.back() = std::move(optLine.value());
                }
            }

            if (!countedRefillTime) {
                time = Timer::elapsedSeconds() - time;
                _time_active += time;
                countedRefillTime = true;
            }

            if (!canMerge || _output_buffer_size >= FULL_CHUNK_SIZE_BYTES) {
                // Sleep (for simplicity; TODO use condition variable instead)
                usleep(1000 * 1);
                continue;
            }

            time = Timer::elapsedSeconds();

            // Find next line to output
            int chosenSource = -1;
            LratClauseId chosenId;
            for (size_t i = 0; i < merger.size(); i++) {
                if (!merger[i].valid()) continue;
                // Largest ID first!
                auto thisId = merger[i].getId();
                if (chosenSource == -1 || thisId > chosenId) {
                    chosenSource = i;
                    chosenId = thisId;
                }
            }
            if (chosenSource == -1) {
                // There's no line to output!
                if (areInputsExhausted()) {
                    // Merge procedure is completely done here
                    _all_sources_exhausted = true;
                    break;
                }
            } else {
                // Line to output found
                auto& chosenLine = merger[chosenSource];
                if (MyMpi::rank(_comm) == 0) {
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
                        if (_binary_output) {
                            lrat_utils::writeDeletionLine(_output_filestream, chosenId, hintsToDelete, lrat_utils::REVERSED);
                        } else {
                            std::string delLine = std::to_string(chosenId) + " d";
                            for (auto hint : hintsToDelete) delLine += " " + std::to_string(hint);
                            delLine += " 0\n";
                            _output_filestream.write(delLine.c_str(), delLine.size());
                        }
                        hintsToDelete.clear();
                    }
                    // Write into final file
                    if (_binary_output) {
                        lrat_utils::writeLine(_output_filestream, chosenLine, lrat_utils::REVERSED);
                    } else {
                        std::string output = chosenLine.toStr();
                        _output_filestream.write(output.c_str(), output.size());
                    }
                } else {
                    // Write into output buffer
                    auto lock = _output_buffer_mutex.getLock();
                    _output_buffer.push_back(chosenLine);
                    _output_buffer_size.fetch_add(chosenLine.size(), std::memory_order_relaxed);
                }
                merger[chosenSource].clear();
                numOutputLines++;
            }

            time = Timer::elapsedSeconds() - time;
            _time_active += time;
            countedRefillTime = false;
        }

        if (MyMpi::rank(_comm) == 0) {
            _output_filestream.flush();
            _output_filestream.close();
        }
    }

    void reverseFile() {
        if (MyMpi::rank(_comm) != 0) return;
        if (_binary_output) {
            // Read binary file in reverse order, output lines into new file
            std::ofstream ofs(_output_filename, std::ofstream::binary);
            ReverseFileReader reader(_output_filename + ".inv");
            char c;
            while (reader.nextAsChar(c)) {
                ofs.put(c);
            }
        } else {
            // Just "tac" the text file
            std::string cmd = "tac " + _output_filename + ".inv > " + _output_filename;
            system(cmd.c_str());
        }
        // remove original (reversed) file
        std::string cmd = "rm " + _output_filename + ".inv";
        system(cmd.c_str());

        _reversed_file = true;
    }

    bool areInputsExhausted() const {
        if (!_local_source_exhausted) return false;
        for (auto& child : _children) {
            if (!child.isExhausted() || !child.isEmpty()) return false;
        }
        return true;
    }

    bool isFullyExhausted() const {
        return _all_sources_exhausted && _output_buffer_size.load(std::memory_order_relaxed) == 0;
    }
};
