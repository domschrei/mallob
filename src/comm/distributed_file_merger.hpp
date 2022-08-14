
#pragma once

#include <functional>
#include <string>
#include <fstream>

#include "mympi.hpp"
#include "data/serializable.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

class DistributedFileMerger {

public:
    struct IdQualifiedLine {
        unsigned long id;
        std::string body;
        size_t size() const {return body.size();}
        bool empty() const {return body.empty();}
    };
    typedef std::function<std::optional<IdQualifiedLine>()> LineSource;

    struct MergeMessage : public Serializable {
        enum Type {REQUEST, RESPONSE_SUCCESS, RESPONSE_EXHAUSTED} type;
        std::vector<IdQualifiedLine> lines;
        virtual std::vector<uint8_t> serialize() const {
            std::vector<uint8_t> result;
            size_t i = 0, n;
            result.resize(sizeof(Type));
            n = sizeof(Type); memcpy(result.data()+i, &type, n); i += n;
            for (auto& line : lines) {
                int lineSize = line.size();
                result.resize(result.size() + sizeof(unsigned long) + sizeof(int) + lineSize);
                n = sizeof(unsigned long); memcpy(result.data()+i, &line.id, n); i += n;
                n = sizeof(int); memcpy(result.data()+i, &lineSize, n); i += n;
                n = lineSize; memcpy(result.data()+i, line.body.c_str(), n); i += n;
            }
            return result;
        }
        virtual Serializable& deserialize(const std::vector<uint8_t>& packed) {
            size_t i = 0, n;
            n = sizeof(Type); memcpy(&type, packed.data()+i, n); i += n;
            while (i < packed.size()) {
                unsigned long id;
                n = sizeof(unsigned long); memcpy(&id, packed.data()+i, n); i += n;
                int lineSize;
                n = sizeof(int); memcpy(&lineSize, packed.data()+i, n); i += n;
                std::string lineBody((const char*) (packed.data()+i), lineSize);
                i += lineSize;
                lines.push_back(IdQualifiedLine{id, std::move(lineBody)});
            }
            return *this;
        }
    };

private:
    const static int FULL_CHUNK_SIZE_BYTES = 500000;
    const static int HALF_CHUNK_SIZE_BYTES = FULL_CHUNK_SIZE_BYTES / 2;

    MPI_Comm _comm;
    int _branching_factor;
    LineSource _local_source;
    bool _local_source_exhausted = false;
    bool _all_sources_exhausted = false;

    class MergeChild {
    private:
        int rankWithinComm;
        std::list<IdQualifiedLine> buffer;
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
                && bufferSize.load(std::memory_order_relaxed) < HALF_CHUNK_SIZE_BYTES; 
        }

        void add(std::vector<IdQualifiedLine>&& newLines) {
            auto lock = bufferMutex.getLock();
            for (auto& line : newLines) {
                buffer.push_back(line);
                bufferSize += line.size();
            }
            refillRequested = false;
        }
        IdQualifiedLine next() {
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

    std::vector<IdQualifiedLine> _output_buffer;
    Mutex _output_buffer_mutex;
    std::atomic_int _output_buffer_size = 0;

    // rank zero only
    std::string _output_filename;
    std::ofstream _output_filestream;

    size_t numOutputLines = 0;
    float lastOutputReport = 0;

    bool _began_merging = false;

    bool _began_final_barrier = false;

public:
    DistributedFileMerger(MPI_Comm comm, int branchingFactor, LineSource localSource, const std::string& outputFileAtZero) : 
            _comm(comm), _branching_factor(branchingFactor), _local_source(localSource) {

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
            // Create final output file
            _output_filename = outputFileAtZero;
            std::string reverseFilename = "_inv_" + _output_filename;
            LOG(V3_VERB, "DFM Opening output file \"%s\"\n", reverseFilename.c_str());
            _output_filestream = std::ofstream(reverseFilename);
        }

        MPI_Ibarrier(_comm, &_barrier_request);
    }

    bool readyToMerge() {
        if (_began_merging) return false;
        int flag;
        MPI_Test(&_barrier_request, &flag, MPI_STATUS_IGNORE);
        return flag;
    }

    void beginMerge() {
        _began_merging = true;
        ProcessWideThreadPool::get().addTask([&]() {
            doMerging();
            reverseFile();
        });
    }

    bool beganMerging() const {
        return _began_merging;
    }

    void handle(int sourceWithinComm, MergeMessage& msg) {

        LOG(V3_VERB, "DFM Msg from [%i]\n", sourceWithinComm);

        if (msg.type == MergeMessage::Type::REQUEST) {
            _request_by_parent = true;
            advance();
            return;
        }

        bool foundChild = false;
        for (auto& child : _children) if (child.getRankWithinComm() == sourceWithinComm) {
            child.add(std::move(msg.lines));
            if (msg.type == MergeMessage::Type::RESPONSE_EXHAUSTED) {
                child.conclude();
            }
            foundChild = true;
            break;
        }
        assert(foundChild);
    }

    void advance() {

        if (Timer::elapsedSeconds() - lastOutputReport > 1.0) {
            LOG(V3_VERB, "DFM Output %i lines so far, exhausted: %s\n", numOutputLines, areInputsExhausted()?"yes":"no");
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

        // Check if there is a refill request from the parent which can be fulfilled
        if (_request_by_parent) {
            MergeMessage msg;
            msg.type = MergeMessage::Type::REQUEST;
            if (_output_buffer_size.load(std::memory_order_relaxed) >= HALF_CHUNK_SIZE_BYTES) {
                // Transfer to parent
                msg.type = MergeMessage::Type::RESPONSE_SUCCESS;
                auto lock = _output_buffer_mutex.getLock();
                msg.lines = std::move(_output_buffer);
                _output_buffer.clear();
                _output_buffer_size.store(0, std::memory_order_relaxed);
            } else if (_all_sources_exhausted) {
                // If all sources are exhausted, send the final buffer content and mark as exhausted
                auto lock = _output_buffer_mutex.getLock();
                msg.lines = std::move(_output_buffer);
                _output_buffer.clear();
                _output_buffer_size.store(0, std::memory_order_relaxed);
                msg.type = MergeMessage::Type::RESPONSE_EXHAUSTED;
            }
            if (msg.type != MergeMessage::Type::REQUEST) {
                LOG(V3_VERB, "DFM Sending refill (%i lines) to [%i]\n", msg.lines.size(), _parent_rank);
                MyMpi::isend(_parent_rank, MSG_ADVANCE_DISTRIBUTED_FILE_MERGE, msg);
                _request_by_parent = false;
            }
        }
    }

    bool finished() const {
        return isFullyExhausted();
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
        std::vector<IdQualifiedLine> merger(_children.size()+1);
        while (true) {
            bool canMerge = true;
            
            // Refill next line from each child as necessary 
            auto childIt = _children.begin();
            for (size_t i = 0; i < _children.size(); i++) {

                if (merger[i].empty()) {
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

            if (merger.back().empty() && !_local_source_exhausted) {
                // Refill from local source
                auto optLine = _local_source();
                if (!optLine.has_value()) {
                    _local_source_exhausted = true;
                } else {
                    merger.back() = optLine.value();
                }
            }

            if (!canMerge || _output_buffer_size >= FULL_CHUNK_SIZE_BYTES) {
                // Sleep (for simplicity; TODO use condition variable instead)
                usleep(1000 * 10);
                continue;
            }

            // Find next line to output
            size_t chosenSource;
            IdQualifiedLine chosenLine;
            for (size_t i = 0; i < merger.size(); i++) {
                if (merger[i].empty()) continue;
                // Largest ID first!
                if (chosenLine.empty() || merger[i].id > chosenLine.id) {
                    chosenSource = i;
                    chosenLine = merger[i];
                }
            }
            if (chosenLine.empty()) {
                // There's no line to output!
                if (areInputsExhausted()) {
                    // Merge procedure is completely done here
                    _all_sources_exhausted = true;
                    break;
                }
            } else {
                // Line to output found
                if (MyMpi::rank(_comm) == 0) {
                    // Write into final file
                    std::string output = std::to_string(chosenLine.id) + " " + chosenLine.body + "\n";
                    _output_filestream.write(output.c_str(), output.size());
                } else {
                    // Write into output buffer
                    auto lock = _output_buffer_mutex.getLock();
                    _output_buffer.push_back(chosenLine);
                    _output_buffer_size.fetch_add(chosenLine.size(), std::memory_order_relaxed);
                }
                merger[chosenSource].body = "";
                numOutputLines++;
            }
        }

        if (MyMpi::rank(_comm) == 0) {
            _output_filestream.flush();
            _output_filestream.close();
        }
    }

    void reverseFile() {
        if (MyMpi::rank(_comm) == 0) {
            std::string cmd = "tac _inv_" + _output_filename + " > " + _output_filename 
                + " && rm _inv_" + _output_filename;
            system(cmd.c_str());
        }
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
