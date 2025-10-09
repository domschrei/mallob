
#ifndef DOMPASCH_MALLOB_JOB_COMM_HPP
#define DOMPASCH_MALLOB_JOB_COMM_HPP

#include <vector>

#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/job_tree.hpp"
#include "comm/msgtags.h"
#include "util/sys/threading.hpp"

class JobComm {

public:
    struct Address {
        int rank;
        ctx_id_t contextId;
    };
    struct AddressList {
        std::vector<Address> list;
        std::vector<int> serializeForJobMsg() const {
            std::vector<int> packed;
            for (auto& [rank, ctxId] : list) {
                auto sizeBefore = packed.size();
                static_assert(sizeof(ctx_id_t) == 2*sizeof(int));
                packed.resize(sizeBefore + 3);
                packed[sizeBefore] = rank;
                memcpy(packed.data()+sizeBefore+1, &ctxId, sizeof(ctx_id_t));
            }
            return packed;
        }
        AddressList& deserializeFromJobMsg(const std::vector<int>& packed) {
            list.clear();
            size_t i = 0;
            while (i < packed.size()) {
                int rank = packed[i];
                ctx_id_t ctxId;
                memcpy(&ctxId, packed.data()+i+1, sizeof(ctx_id_t));
                list.push_back({rank, ctxId});
                i += 3;
            }
            return *this;
        }
    };

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
        return (size_t)intRank < _address_list.list.size() ? _address_list.list[intRank].rank : -1;
    }
    
    ctx_id_t getContextIdOrZero(int intRank) const {
        auto lock = _access_mutex.getLock();
        return (size_t)intRank < _address_list.list.size() ? _address_list.list[intRank].contextId : 0;
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
        return _address_list.list.size();
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
    std::vector<Address> getAddressList() const {
        auto lock = _access_mutex.getLock();
        return _address_list.list;
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
    AddressList _address_list;
    robin_hood::unordered_flat_map<int, int> _world_to_int_rank;

    AddressList _next_address_list;
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
        if (_job_tree.isRoot()) {
            auto lock = _access_mutex.getLock();
            _address_list = AddressList {{1, {_job_tree.getRank(), _job_tree.getContextId()}}};
            updateMap();
        } else {
            JobMessage msg;
            AddressList addressList{{(size_t)(_job_tree.getIndex()+1), Address {-1, 0}}};
            addressList.list[_job_tree.getIndex()] = {_job_tree.getRank(), _job_tree.getContextId()};
            msg.payload = addressList.serializeForJobMsg();
            msg.epoch = 0;
            msg.jobId = _id;
            msg.tag = MSG_AGGREGATE_RANKLIST;
            LOG(V1_WARN, "jobId<#%i>: sending [%i]->[%i] MSG_AGGREGATE_RANKLIST \n", _id, _job_tree.getRank(), _job_tree.getParentNodeRank());
            for (Address adr : addressList.list) {
                LOG(V1_WARN, "  ----  address: rank %i context %i \n", adr.rank, adr.contextId);
            }
            //log(LOG_ADD_DESTRANK | V3_VERB, "send Ranklist size %i", getJobTree().getParentNodeRank(), msg.payload.size());
            _job_tree.sendToParent(msg);
        }
        _time_of_last_ranklist_agg = Timer::elapsedSecondsCached();
        _aggregate_ranklist = false;
    }

    bool handle(JobMessage& msg) {
        if (msg.returnedToSender) return false;

        if (msg.tag == MSG_AGGREGATE_RANKLIST) {

            {
                AddressList ranklist;
                ranklist.deserializeFromJobMsg(msg.payload);

                // Update local ranklist with new incoming entries
                for (size_t i = 0; i < ranklist.list.size(); i++) {
                    if (_next_address_list.list.size() == i) _next_address_list.list.push_back({-1, 0});
                    if (ranklist.list[i].rank != -1) _next_address_list.list[i] = ranklist.list[i];
                }
                _num_ranklist_contribs++;
            }

            // Check if ranklist can be forwarded
            int numChildren = 0;
            if (_job_tree.hasLeftChild()) numChildren++;
            if (_job_tree.hasRightChild()) numChildren++;
            bool readyToForward = numChildren <= _num_ranklist_contribs;
            if (_job_tree.hasLeftChild()) 
                readyToForward &= (size_t)_job_tree.getLeftChildIndex() < _next_address_list.list.size() 
                    && _next_address_list.list[_job_tree.getLeftChildIndex()].rank != -1;
            if (_job_tree.hasRightChild()) 
                readyToForward &= (size_t)_job_tree.getRightChildIndex() < _next_address_list.list.size() 
                    && _next_address_list.list[_job_tree.getRightChildIndex()].rank != -1;
            if (!readyToForward) return true;

            // Set own index, write result back into payload
            int myIndex = _job_tree.getIndex();
            _next_address_list.list[myIndex] = {_job_tree.getRank(), _job_tree.getContextId()};
            msg.payload = _next_address_list.serializeForJobMsg();

            if (myIndex == 0) {
                // Broadcast
                msg.tag = MSG_BROADCAST_RANKLIST;
                _job_tree.sendToSelf(msg);
            } else {
                // Forward to parent
                //log(LOG_ADD_DESTRANK | V3_VERB, "send Ranklist size %i", getJobTree().getParentNodeRank(), msg.payload.size());
                _job_tree.sendToParent(msg);
            }

            // Clean up aggregation data
            _num_ranklist_contribs = 0;
            _next_address_list.list.clear();

            return true;

        } else if (msg.tag == MSG_BROADCAST_RANKLIST) {

            // Store locally and forward to children
            {
                auto lock = _access_mutex.getLock();
                _address_list.deserializeFromJobMsg(msg.payload);
                updateMap();
            }
            _job_tree.sendToAnyChildren(msg);
            return true;
        }

        return false;
    }

    void updateMap() {
        _world_to_int_rank.clear();
        for (size_t i = 0; i < _address_list.list.size(); i++) 
            _world_to_int_rank[_address_list.list[i].rank] = i;
    }
};

#endif