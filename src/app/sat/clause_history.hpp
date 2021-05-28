
#ifndef DOMPASCH_MALLOB_CLAUSE_HISTORY_HPP
#define DOMPASCH_MALLOB_CLAUSE_HISTORY_HPP

#include <vector>
#include <list>
#include <array>

#include "util/params.hpp"
#include "app/sat/base_sat_job.hpp"
#include "app/sat/hordesat/sharing/lockfree_clause_database.hpp"
#include "util/logger.hpp"

class ClauseHistory {

public:
    struct Subscription {
        int correspondingRank = -1;
        int nextIndex;
        int endIndex;
    };
    struct Entry {
        std::vector<int> clauses;
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

    LockfreeClauseDatabase _cdb;
    BaseSatJob& _job;

    std::vector<Subscription> _subscribers;
    Subscription _subscription;

public:
    ClauseHistory(Parameters& params, int stmBufferSizePerEpoch, BaseSatJob& job) : 
        _aggregation_factor(params.getIntParam("chaf")), 
        _num_stm_slots(params.getIntParam("chstms")), 
        _stm_buffer_size(_aggregation_factor * stmBufferSizePerEpoch), 
        _ltm_buffer_size(params.getIntParam("cbbs")), 
        _use_checksums(params.isNotNull("checksums")),
        _cdb(
            /*maxClauseSize=*/params.getIntParam("hmcl"), 
            /*maxLbdPartitionedSize=*/params.getIntParam("mlbdps"),
            /*baseBufferSize=*/params.getIntParam("cbbs"), 
            /*numProducers=*/1
	    ), _job(job) {}

    // Should be called periodically but not to often in order to 
    // give the job nodes time to digest each batch of clauses.
    void sendNextBatches() {

        for (size_t i = 0; i < _subscribers.size(); i++) {
            auto& subscription = _subscribers[i];
            log(V4_VVER, "CLSHIST %s Subscriber [%i]: [%i,%i) %i/%i\n", _job.toStr(), subscription.correspondingRank, 
                subscription.nextIndex, subscription.endIndex, 
                subscription.nextIndex < _history.size() ? _history[subscription.nextIndex].numAggregated() : 0, _aggregation_factor);

            if (!isBatchComplete(subscription.nextIndex)) continue;

            // Send historic clauses
            JobMessage msg;
            msg.jobId = _job.getId();
            msg.revision = _job.getRevision();
            msg.epoch = indexToFirstEpoch(subscription.nextIndex);
            msg.tag = MSG_CLAUSE_HISTORY_SEND_CLAUSES;
            msg.payload = _history[subscription.nextIndex].clauses;
            if (_use_checksums) setChecksum(msg);
            MyMpi::isend(MPI_COMM_WORLD, subscription.correspondingRank, MSG_SEND_APPLICATION_MESSAGE, msg);
            log(LOG_ADD_DESTRANK | V4_VVER, "CLSHIST %s Send batch of epoch %i", subscription.correspondingRank, _job.toStr(), msg.epoch);

            subscription.nextIndex++;
            if (subscription.nextIndex == subscription.endIndex) {
                // Erase completed subscription
                log(V4_VVER, "CLSHIST %s Subscription [%i] finished\n", _job.toStr(), subscription.correspondingRank);    
                _subscribers.erase(_subscribers.begin()+i);
                i--;
            }
        }
    }

    // Reaction to MSG_CLAUSE_HISTORY_SEND_CLAUSES
    void addEpoch(int epoch, std::vector<int>& clauses, bool entireIndex) {
        auto [index, offset] = epochToIndexAndOffset(epoch);

        //if (entireIndex) log(V4_VVER, "CLSHIST Received clauses of index %i\n", index);
        
        // Insert clauses into history vector
        while (index >= _history.size()) _history.emplace_back(_aggregation_factor);
        if (_history[index].empty()) {
            _history[index].clauses = clauses;
        } else {
            // Merge with prior clauses at that index
            auto merger = _cdb.getBufferMerger();
            merger.add(_cdb.getBufferReader(_history[index].clauses.data(), _history[index].clauses.size()));
            merger.add(_cdb.getBufferReader(clauses.data(), clauses.size()));
            _history[index].clauses = merger.merge(isShorttermMemory(index) ? _stm_buffer_size : _ltm_buffer_size);
        }
        if (entireIndex) {
            for (size_t i = 0; i < _aggregation_factor; i++) 
                _history[index].aggregated[i] = true;
        } else {
            _history[index].aggregated[offset] = true;
        }

        // Reduce missing slices in chronological order
        if (!_missing_epoch_ranges.empty()) {
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
            _subscription.nextIndex = index+1;
            if (_subscription.nextIndex == _subscription.endIndex) {
                // Subscription done! Ends automatically at the corresp. rank.
                _subscription = Subscription();
            }
        }   
        if (_subscription.correspondingRank == -1) {
            // No subscription active - start new one?

            if (!_missing_epoch_ranges.empty()) {
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
                MyMpi::isend(MPI_COMM_WORLD, _subscription.correspondingRank, MSG_SEND_APPLICATION_MESSAGE, msg);
                log(LOG_ADD_DESTRANK | V4_VVER, "CLSHIST %s Subscribe for indices [%i,%i)", _subscription.correspondingRank, _job.toStr(), 
                    msg.payload[0], msg.payload[1]);
            }
        }

        // Update most recent epoch
        _latest_epoch = std::max(_latest_epoch, epoch);

        // Debugging output
        if (!_missing_epoch_ranges.empty()) {
            std::string out = "";
            for (auto [from, to] : _missing_epoch_ranges) {
                out += "[" + std::to_string(from) + "," + std::to_string(to) + ") ";
            }
            log(V4_VVER, "CLSHIST %s missing ranges: %s\n", _job.toStr(), out.c_str());
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
        log(V4_VVER, "CLSHIST history flagfield: %s\n", out.c_str());
        */
    }

    // Reaction to MSG_CLAUSE_HISTORY_SUBSCRIBE
    void onSubscribe(int source, int beginIndex, int endIndex) {
        log(V4_VVER, "CLSHIST %s [%i] subscribed\n", _job.toStr(), source);
        _subscribers.push_back(Subscription{source, beginIndex, endIndex});
    }

    // Reaction to MSG_CLAUSE_HISTORY_UNSUBSCRIBE
    void onUnsubscribe(int source) {
        log(V4_VVER, "CLSHIST %s [%i] unsubscribed\n", _job.toStr(), source);
        for (size_t i = 0; i < _subscribers.size(); i++) {
            if (_subscribers[i].correspondingRank == source) {
                _subscribers.erase(_subscribers.begin()+i);
                i--;
            }
        }
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
            MyMpi::isend(MPI_COMM_WORLD, _subscription.correspondingRank, MSG_SEND_APPLICATION_MESSAGE, msg);
            log(LOG_ADD_DESTRANK | V4_VVER, "CLSHIST %s Unsubscribe", _subscription.correspondingRank, _job.toStr());
            _subscription = Subscription();
        }
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

#endif
