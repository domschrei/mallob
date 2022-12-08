
#ifndef DOMPASCH_MALLOB_JOB_COMM_HPP
#define DOMPASCH_MALLOB_JOB_COMM_HPP

#include <vector>
#include "util/assert.hpp"

#include "util/hashing.hpp"
#include "util/sys/timer.hpp"
#include "app/job_tree.hpp"
#include "comm/mympi.hpp"
#include "util/sys/threading.hpp"

class JobComm {

public:
    ////////////////////////////////////////////////////////////////////////////////
    // Communicator methods
    ////////////////////////////////////////////////////////////////////////////////

    /*
    Returns the MPI_COMM_WORLD rank of the node of the given internal rank 
    (job tree index) or -1 if the internal rank is unknown or invalid.
    */
    int getWorldRankOrMinusOne(int intRank) const {
        auto lock = _access_mutex.getLock();
        return (size_t)intRank < _ranklist.size() ? _ranklist[intRank] : -1;
    }

    /*
    See getWorldRankOrMinusOne(intRank).
    */
    int operator[](int intRank) const {
        return getWorldRankOrMinusOne(intRank);
    }
    
    /*
    Returns the current number of nodes within the communicator.
    */
    size_t size() const {
        auto lock = _access_mutex.getLock();
        return _ranklist.size();
    }
    
    /*
    Returns the internal rank (job tree index) of the given MPI_COMM_WORLD rank
    or -1 if no node of this internal rank exists within the communicator as of now.
    */
    int getInternalRankOrMinusOne(int worldRank) const {
        auto lock = _access_mutex.getLock();
        return _world_to_int_rank.count(worldRank) ? 
            _world_to_int_rank.at(worldRank) : -1;
    }

    /*
    Returns the entire list of world ranks in the communicator at the present time.
    The result at position i contains this->getWorldRankOrMinusOne(i).
    */
    std::vector<int> getRanklist() const {
        auto lock = _access_mutex.getLock();
        return _ranklist;
    }

    /*
    Returns the entire map of world ranks to internal ranks in the commicator
    at the present time. The result maps i to this->getInternalRankOrMinusOne(i).
    */
    robin_hood::unordered_flat_map<int, int> getWorldToInternalMap() const {
        auto lock = _access_mutex.getLock();
        return _world_to_int_rank;
    }

    ////////////////////////////////////////////////////////////////////////////////
    // End of communicator methods
    ////////////////////////////////////////////////////////////////////////////////    

private:
    int _id;
    JobTree& _job_tree;
    
    mutable Mutex _access_mutex;
    std::vector<int> _ranklist;
    robin_hood::unordered_flat_map<int, int> _world_to_int_rank;

    std::vector<int> _next_ranklist;
    bool _aggregate_ranklist = false;
    int _num_ranklist_contribs = 0;
    float _time_of_last_ranklist_agg = 0;
    float _ranklist_agg_period = 0;

public:
    ////////////////////////////////////////////////////////////////////////////////
    // Construction methods
    ////////////////////////////////////////////////////////////////////////////////

    JobComm(int id, JobTree& tree, float updatePeriod) : _id(id), 
        _job_tree(tree), _ranklist_agg_period(updatePeriod) {}
    
    bool wantsToAggregate() {
        if (_ranklist_agg_period <= 0) return false;
        if (_job_tree.isLeaf() 
            && Timer::elapsedSecondsCached() - _time_of_last_ranklist_agg >= _ranklist_agg_period) {
            _aggregate_ranklist = true;
            return true;
        }
        return false;
    }

    bool isAggregating() {return _aggregate_ranklist;}

    void beginAggregation() {
        if (_job_tree.getIndex() == 0) {
            auto lock = _access_mutex.getLock();
            _ranklist = std::vector<int>(1, _job_tree.getRank());
            updateMap();
        } else {
            JobMessage msg;
            msg.payload = std::vector<int>(_job_tree.getIndex()+1, -1);
            msg.payload[_job_tree.getIndex()] = _job_tree.getRank();
            msg.epoch = 0;
            msg.jobId = _id;
            msg.tag = MSG_AGGREGATE_RANKLIST;
            //log(LOG_ADD_DESTRANK | V3_VERB, "send Ranklist size %i", getJobTree().getParentNodeRank(), msg.payload.size());
            MyMpi::isend(_job_tree.getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        }
        _time_of_last_ranklist_agg = Timer::elapsedSecondsCached();
        _aggregate_ranklist = false;
    }

    bool handle(JobMessage& msg) {

        if (msg.returnedToSender) return false;

        if (msg.tag == MSG_AGGREGATE_RANKLIST) {

            std::vector<int>& ranklist = msg.payload;

            // Update local ranklist with new incoming entries
            for (size_t i = 0; i < ranklist.size(); i++) {
                if (_next_ranklist.size() == i) _next_ranklist.push_back(-1);
                if (ranklist[i] != -1) _next_ranklist[i] = ranklist[i];
            }
            _num_ranklist_contribs++;

            // Check if ranklist can be forwarded
            int numChildren = 0;
            if (_job_tree.hasLeftChild()) numChildren++;
            if (_job_tree.hasRightChild()) numChildren++;
            bool readyToForward = numChildren <= _num_ranklist_contribs;
            if (_job_tree.hasLeftChild()) 
                readyToForward &= (size_t)_job_tree.getLeftChildIndex() < _next_ranklist.size() 
                    && _next_ranklist[_job_tree.getLeftChildIndex()] != -1;
            if (_job_tree.hasRightChild()) 
                readyToForward &= (size_t)_job_tree.getRightChildIndex() < _next_ranklist.size() 
                    && _next_ranklist[_job_tree.getRightChildIndex()] != -1;
            if (!readyToForward) return true;

            // Set own index, write result back into payload
            int myIndex = _job_tree.getIndex();
            _next_ranklist[myIndex] = _job_tree.getRank();
            ranklist = _next_ranklist;

            if (myIndex == 0) {
                // Store locally
                {
                    auto lock = _access_mutex.getLock();
                    _ranklist = _next_ranklist;
                    updateMap();
                }
                // Broadcast
                msg.tag = MSG_BROADCAST_RANKLIST;
                if (_job_tree.hasLeftChild())
                    MyMpi::isend(_job_tree.getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
                if (_job_tree.hasRightChild())
                    MyMpi::isend(_job_tree.getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            } else {
                // Forward to parent
                //log(LOG_ADD_DESTRANK | V3_VERB, "send Ranklist size %i", getJobTree().getParentNodeRank(), msg.payload.size());
                MyMpi::isend(_job_tree.getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            }

            // Clean up aggregation data
            _num_ranklist_contribs = 0;
            _next_ranklist.clear();

            return true;

        } else if (msg.tag == MSG_BROADCAST_RANKLIST) {

            // Store locally and forward to children
            {
                auto lock = _access_mutex.getLock();
                _ranklist = msg.payload;
                updateMap();
            }
            if (_job_tree.hasLeftChild())
                MyMpi::isend(_job_tree.getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            if (_job_tree.hasRightChild())
                MyMpi::isend(_job_tree.getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            return true;
        }

        return false;
    }

    void updateMap() {
        _world_to_int_rank.clear();
        for (size_t i = 0; i < _ranklist.size(); i++) 
            _world_to_int_rank[_ranklist[i]] = i;
    }
};

#endif