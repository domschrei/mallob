
#pragma once

#include <vector>
#include <list>
#include <array>

#include "util/params.hpp"
#include "base_sat_job.hpp"
#include "../sharing/buffer/adaptive_clause_database.hpp"
#include "util/logger.hpp"

class ClauseHistory {

public:
    struct Subscription {
        int correspondingRank = -1;
        int nextIndex;
        int endIndex;
    };
    struct Entry {
        std::vector<std::vector<int>> clauses;
        std::vector<bool> aggregated;
        Entry() {}
        Entry(int size) : aggregated(std::vector<bool>(size, false)) {}
        int numAggregated() {
            int s = 0; 
            for (bool b : aggregated) {
                if (b) s++;
            }
            return s;
        }
        bool empty() {return numAggregated() == 0;}
    };
    static const int MSG_CLAUSE_HISTORY_SUBSCRIBE = 4194304;
    static const int MSG_CLAUSE_HISTORY_UNSUBSCRIBE = 4194305;
    static const int MSG_CLAUSE_HISTORY_SEND_CLAUSES = 4194306;

private:
    int _aggregation_factor;
    int _num_stm_slots;
    int _stm_buffer_size;
    int _ltm_buffer_size;
    bool _use_checksums;
    std::vector<Entry> _history;
    std::list<std::pair<int, int>> _missing_epoch_ranges;
    int _latest_epoch = -1;

    AdaptiveClauseDatabase& _cdb;
    BaseSatJob& _job;

    robin_hood::unordered_flat_map<int, Subscription> _subscribers;
    Subscription _subscription;

public:
    ClauseHistory(const Parameters& params, int stmBufferSizePerEpoch, BaseSatJob& job, AdaptiveClauseDatabase& cdb) : 
        _aggregation_factor(params.clauseHistoryAggregationFactor()), 
        _num_stm_slots(params.clauseHistoryShortTermMemSize()), 
        _stm_buffer_size(stmBufferSizePerEpoch), 
        _ltm_buffer_size(params.clauseBufferBaseSize()), 
        _use_checksums(params.useChecksums()),
        _cdb(cdb), _job(job) {}

    // Should be called periodically but not to often in order to 
    // give the job nodes time to digest each batch of clauses.
    void sendNextBatches() {

        std::vector<int> subscribersToDelete;
        for (auto& [rank, subscription] : _subscribers) {
            LOG(V4_VVER, "CLSHIST %s Subscriber [%i]: [%i,%i) %i/%i\n", _job.toStr(), subscription.correspondingRank, 
                subscription.nextIndex, subscription.endIndex, 
                subscription.nextIndex < _history.size() ? _history[subscription.nextIndex].numAggregated() : 0, _aggregation_factor);

            if (!isBatchComplete(subscription.nextIndex)) continue;

            // Send historic clauses
            JobMessage msg;
            msg.jobId = _job.getId();
            msg.revision = _job.getRevision();
            msg.epoch = indexToFirstEpoch(subscription.nextIndex);
            msg.tag = MSG_CLAUSE_HISTORY_SEND_CLAUSES;
            msg.payload = _history[subscription.nextIndex].clauses.at(0);
            if (_use_checksums) setChecksum(msg);
            LOG_ADD_DEST(V4_VVER, "CLSHIST %s Send batch of index %i", rank, _job.toStr(), subscription.nextIndex);
            
            if (rank == MyMpi::rank(MPI_COMM_WORLD)) {
                // message to self: digest clauses directly
                _job.digestSharingWithoutFilter(msg.payload);
            } else {
                // message to another worker
                MyMpi::isend(rank, MSG_SEND_APPLICATION_MESSAGE, msg);
            }

            subscription.nextIndex++;
            if (subscription.nextIndex >= subscription.endIndex) {
                // Erase completed subscription
                LOG(V4_VVER, "CLSHIST %s Subscription [%i] finished\n", _job.toStr(), rank);    
                subscribersToDelete.push_back(rank);
            }
        }
        for (int rank : subscribersToDelete) _subscribers.erase(rank);
    }

    void subscribe() {
        auto [from, to] = *_missing_epoch_ranges.begin();
        _subscription.nextIndex = epochToIndexAndOffset(from).first;
        _subscription.endIndex = epochToIndexAndOffset(to).first;
        _subscription.correspondingRank = _job.getJobTree().getParentNodeRank();
        
        // Send subscription message
        JobMessage msg;
        msg.jobId = _job.getId();
        msg.revision = _job.getRevision();
        msg.tag = MSG_CLAUSE_HISTORY_SUBSCRIBE;
        msg.payload.push_back(_subscription.nextIndex);
        msg.payload.push_back(_subscription.endIndex);
        if (_use_checksums) setChecksum(msg);
        MyMpi::isend(_subscription.correspondingRank, MSG_SEND_APPLICATION_MESSAGE, msg);
        LOG_ADD_DEST(V4_VVER, "CLSHIST %s Subscribe for indices [%i,%i)", _subscription.correspondingRank, _job.toStr(), 
            msg.payload[0], msg.payload[1]);
    }

    // Reaction to MSG_CLAUSE_HISTORY_SEND_CLAUSES
    void addEpoch(int epoch, std::vector<int>& clauses, bool entireIndex) {
        auto [index, offset] = epochToIndexAndOffset(epoch);

        if (entireIndex) LOG(V4_VVER, "CLSHIST Received cls, index %i\n", index);
        else LOG(V4_VVER, "CLSHIST Received cls, epoch %i\n", epoch);
        
        // Insert clauses into history vector
        bool alreadyPresent = entireIndex ? isBatchComplete(index) : isEpochPresent(epoch);
        if (!alreadyPresent) {
            // Append history entries as long as necessary
            while (index >= _history.size()) _history.emplace_back(_aggregation_factor);
            
            // Shorten the incoming buffer as necessary
            size_t sizeLimit = (isShorttermMemory(index) ? _stm_buffer_size/(entireIndex?1:_aggregation_factor) : _ltm_buffer_size);
            if (clauses.size() <= sizeLimit) {
                _history[index].clauses.push_back(clauses);
            } else {
                std::vector<int> shortened;
                shortened.reserve(sizeLimit);
                shortened.insert(shortened.end(), clauses.begin(), clauses.begin()+sizeLimit);
                _history[index].clauses.push_back(std::move(shortened));
            }

            // Mark incoming epoch(s) as arrived
            if (entireIndex) {
                for (size_t i = 0; i < _aggregation_factor; i++) 
                    _history[index].aggregated[i] = true;
            } else {
                _history[index].aggregated[offset] = true;
            }
            
            if (isBatchComplete(index)) {
                // Merge with prior clauses at that index
                LOG(V4_VVER, "CLSHIST Merging index ...\n");
                auto merger = _cdb.getBufferMerger(isShorttermMemory(index) ? _stm_buffer_size : _ltm_buffer_size);
                for (auto& clsbuf : _history[index].clauses)
                    merger.add(_cdb.getBufferReader(clsbuf.data(), clsbuf.size(), /*useChecksums=*/false));
                auto merged = merger.merge();
                _history[index].clauses.resize(1);
                _history[index].clauses[0] = std::move(merged);
                LOG(V3_VERB, "CLSHIST %s : Merged index %i (epochs %i..%i) into buf size %i\n", 
                    _job.toStr(), index, indexToFirstEpoch(index), indexToFirstEpoch(index+1)-1, 
                    _history[index].clauses[0].size());
            }
        }

        // Reduce missing slices in chronological order
        for (auto it = _missing_epoch_ranges.begin(); it != _missing_epoch_ranges.end(); ++it) {
            auto& [from, to] = *it;

            // Shrink missing range by all epochs which are now present
            while (from < to && isEpochPresent(from)) from++;
            while (from < to && isEpochPresent(to-1)) to--;
            
            if (from == to) {
                // Missing range finished!
                it = _missing_epoch_ranges.erase(it);
                it--;
            }
        }

        // Need for a new missing range?
        if (epoch > _latest_epoch+1) {
            // Missed one or several epochs before this epoch
            if (!_missing_epoch_ranges.empty() 
                && _missing_epoch_ranges.back().second == _latest_epoch+1) {
                // Can extend last missing range
                _missing_epoch_ranges.back().second = epoch+1;
            } else {
                // Create new missing range
                _missing_epoch_ranges.emplace_back(_latest_epoch+1, epoch+1);
            }
        }

        // Update your own subscription
        if (_subscription.correspondingRank >= 0) {
            if (_subscription.nextIndex != index) {
                // Received "wrong" index: Renew subscription with correct range
                subscribe();
            } else {
                _subscription.nextIndex++;
                if (_subscription.nextIndex >= _subscription.endIndex) {
                    // Subscription done! Ends automatically at the corresp. rank.
                    _subscription = Subscription();
                }
            }
        }   
        if (_subscription.correspondingRank == -1) {
            // No subscription active - start new one?
            if (!_missing_epoch_ranges.empty()) subscribe();
        }

        // Update most recent epoch
        auto prevLatestEpoch = _latest_epoch;
        _latest_epoch = std::max(_latest_epoch, epoch);

        int oldIndex = epochToIndexAndOffset(prevLatestEpoch).first;
        int newIndex = epochToIndexAndOffset(_latest_epoch).first;
        // If necessary, reduce a batch from Shorttermmemory-Size to Longtermmemory-Size
        if (newIndex > oldIndex && newIndex >= _num_stm_slots) {
            int indexToReduce = newIndex - _num_stm_slots;
            if (isBatchComplete(indexToReduce) && _history[indexToReduce].clauses[0].size() > _ltm_buffer_size) {
                // Reduce this batch
                _history[indexToReduce].clauses[0].resize(_ltm_buffer_size);
            }
        }

        // Debugging output
        if (!_missing_epoch_ranges.empty()) {
            std::string out = "";
            for (auto [from, to] : _missing_epoch_ranges) {
                out += "[" + std::to_string(from) + "," + std::to_string(to) + ") ";
            }
            LOG(V4_VVER, "CLSHIST %s missing ranges: %s\n", _job.toStr(), out.c_str());
        }
        /*
        std::string out = "";
        for (size_t i = 0; i < _history.size(); i++) {
            out += "[";
            for (size_t j = 0; j < _aggregation_factor; j++) {
                out += std::to_string(_history[i].aggregated[j] ? 1 : 0);
            }
            out += "]";
        }
        LOG(V4_VVER, "CLSHIST history flagfield: %s\n", out.c_str());
        */
    }

    // Reaction to MSG_CLAUSE_HISTORY_SUBSCRIBE
    void onSubscribe(int source, int beginIndex, int endIndex) {
        LOG(V4_VVER, "CLSHIST %s [%i] subscribed\n", _job.toStr(), source);
        _subscribers[source] = Subscription{source, beginIndex, endIndex};
    }

    // Reaction to MSG_CLAUSE_HISTORY_UNSUBSCRIBE
    void onUnsubscribe(int source) {
        LOG(V4_VVER, "CLSHIST %s [%i] unsubscribed\n", _job.toStr(), source);
        _subscribers.erase(source);
    }

    // Must be called whenever the job node suspends its execution.
    void onSuspend() {
        if (_subscription.correspondingRank >= 0) {
            // Send unsubscribe message
            JobMessage msg;
            msg.jobId = _job.getId();
            msg.revision = _job.getRevision();
            msg.tag = MSG_CLAUSE_HISTORY_UNSUBSCRIBE;
            if (_use_checksums) setChecksum(msg);
            MyMpi::isend(_subscription.correspondingRank, MSG_SEND_APPLICATION_MESSAGE, msg);
            LOG_ADD_DEST(V4_VVER, "CLSHIST %s Unsubscribe", _subscription.correspondingRank, _job.toStr());
            _subscription = Subscription();
        }
    }

    void feedHistoryIntoSolver() {
        int myRank = MyMpi::rank(MPI_COMM_WORLD);
        int lastIndex = epochToIndexAndOffset(_latest_epoch).first + 1;
        LOG(V4_VVER, "CLSHIST %s self-subscribing [%i,%i]\n", _job.toStr(), 0, lastIndex);
        _subscribers[myRank] = Subscription{myRank, 0, lastIndex};
    }

private:
    void setChecksum(JobMessage& msg) {
        msg.checksum.combine(msg.jobId);
        for (int& lit : msg.payload) msg.checksum.combine(lit);
    }

    std::pair<int, int> epochToIndexAndOffset(int epoch) {
        return std::pair<int, int>(epoch/_aggregation_factor, epoch%_aggregation_factor);
    }
    int indexToFirstEpoch(int index) {
        return index*_aggregation_factor;
    }
    bool isShorttermMemory(int index) {
        return epochToIndexAndOffset(_latest_epoch).first - index <= _num_stm_slots;
    }

    bool isBatchComplete(int index) {
        return index < _history.size() && _history[index].numAggregated() == _aggregation_factor;
    }
    bool isEpochPresent(int epoch) {
        auto [index, offset] = epochToIndexAndOffset(epoch);
        if (index >= _history.size()) return false;
        return _history[index].aggregated[offset];
    }

};
