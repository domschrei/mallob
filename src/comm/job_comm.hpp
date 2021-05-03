
#ifndef DOMPASCH_MALLOB_JOB_COMM_HPP
#define DOMPASCH_MALLOB_JOB_COMM_HPP

#include <vector>
#include <assert.h>

#include "util/sys/timer.hpp"
#include "app/job_tree.hpp"
#include "comm/mympi.hpp"

#define MSG_AGGREGATE_RANKLIST 420
#define MSG_BROADCAST_RANKLIST 421

class JobComm {

private:
    int _id;
    JobTree& _job_tree;
    
    std::vector<int> _ranklist;

    std::vector<int> _next_ranklist;
    bool _aggregate_ranklist = false;
    int _num_ranklist_contribs = 0;
    float _time_of_last_ranklist_agg = 0;
    float _ranklist_agg_period = 0;

public:
    JobComm(int id, JobTree& tree, float updatePeriod) : _id(id), 
        _job_tree(tree), _ranklist_agg_period(updatePeriod) {}

    bool wantsToAggregate() {
        if (_ranklist_agg_period <= 0) return false;
        if (_job_tree.isLeaf() 
            && Timer::elapsedSeconds() - _time_of_last_ranklist_agg >= _ranklist_agg_period) {
            _aggregate_ranklist = true;
            return true;
        }
        return false;
    }

    bool isAggregating() {return _aggregate_ranklist;}

    void beginAggregation() {
        if (_job_tree.getIndex() == 0) {
            _ranklist = std::vector<int>(1, _job_tree.getRank());
        } else {
            JobMessage msg;
            msg.payload = std::vector<int>(_job_tree.getIndex()+1, -1);
            msg.payload[_job_tree.getIndex()] = _job_tree.getRank();
            msg.epoch = 0;
            msg.jobId = _id;
            msg.tag = MSG_AGGREGATE_RANKLIST;
            //log(LOG_ADD_DESTRANK | V3_VERB, "send Ranklist size %i", getJobTree().getParentNodeRank(), msg.payload.size());
            MyMpi::isend(MPI_COMM_WORLD, _job_tree.getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        }
        _time_of_last_ranklist_agg = Timer::elapsedSeconds();
        _aggregate_ranklist = false;
    }

    bool handle(JobMessage& msg) {

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
                // Store locally and broadcast
                _ranklist = _next_ranklist;
                msg.tag = MSG_BROADCAST_RANKLIST;
                if (_job_tree.hasLeftChild())
                    MyMpi::isend(MPI_COMM_WORLD, _job_tree.getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
                if (_job_tree.hasRightChild())
                    MyMpi::isend(MPI_COMM_WORLD, _job_tree.getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            } else {
                // Forward to parent
                //log(LOG_ADD_DESTRANK | V3_VERB, "send Ranklist size %i", getJobTree().getParentNodeRank(), msg.payload.size());
                MyMpi::isend(MPI_COMM_WORLD, _job_tree.getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            }

            // Clean up aggregation data
            _num_ranklist_contribs = 0;
            _next_ranklist.clear();

            return true;

        } else if (msg.tag == MSG_BROADCAST_RANKLIST) {

            // Store locally and forward to children
            _ranklist = msg.payload;
            if (_job_tree.hasLeftChild())
                MyMpi::isend(MPI_COMM_WORLD, _job_tree.getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            if (_job_tree.hasRightChild())
                MyMpi::isend(MPI_COMM_WORLD, _job_tree.getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            return true;
        }

        return false;
    }

    int getRankOrMinusOne(int index) const {return (size_t)index < _ranklist.size() ? _ranklist[index] : -1;}
    bool hasRank(int index) const {return getRankOrMinusOne(index) >= 0;}
    int getRank(int index) const {
        int rank = getRankOrMinusOne(index);
        assert(rank >= 0);
        return rank;
    }
    int operator[](int index) const {return getRankOrMinusOne(index);}
    size_t getCommSize() const {return _ranklist.size();}
};

#endif