
#pragma once

#include "comm/async_collective.hpp"
#include "comm/msgtags.h"
#include "comm/mympi.hpp"
#include "core/job_registry.hpp"
#include "data/job_transfer.hpp"
#include "data/reduceable.hpp"
#include "mpi.h"
#include "robin_set.h"
#include "util/periodic_event.hpp"
#include <vector>

class GroupCommBuilder {

public:
    struct Address {int worldRank; ctx_id_t contextId; int groupId;};
    struct GroupsAddressList : public Reduceable {
        std::vector<Address> list;
        virtual std::vector<uint8_t> serialize() const override {
            std::vector<uint8_t> packed(list.size() * (sizeof(int)+sizeof(ctx_id_t)+sizeof(int)));
            int i = 0;
            for (auto& [rank, ctxId, groupId] : list) {
                * (int*) (packed.data()+i) = rank;
                i += sizeof(int);
                * (ctx_id_t*) (packed.data()+i) = ctxId;
                i += sizeof(ctx_id_t);
                * (int*) (packed.data()+i) = groupId;
                i += sizeof(int);
            }
            return packed;
        }
        virtual GroupsAddressList& deserialize(const std::vector<uint8_t>& packed) override {
            list.clear();
            size_t i = 0, n;
            while (i < packed.size()) {
                Address addr;
                n = sizeof(int); memcpy(&addr.worldRank, packed.data()+i, n); i += n;
                n = sizeof(ctx_id_t); memcpy(&addr.contextId, packed.data()+i, n); i += n;
                n = sizeof(int); memcpy(&addr.groupId, packed.data()+i, n); i += n;
                list.push_back(addr);
            }
            return *this;
        }
        virtual bool isEmpty() const override {
            return list.empty();
        }
        // Associative operation [ (axb)xc = ax(bxc) ] where "other" represents
        // the "right" element and this instance represents the "left" element.
        // The operation does not need to be commutative [ axb = bxa ].
        virtual void aggregate(const Reduceable& other) override {
            GroupsAddressList* otherList = (GroupsAddressList*) &other;
            list.insert(list.end(), otherList->list.begin(), otherList->list.end());
        }
    };

private:
    const int GROUP_COMM_INSTANCE_ID = 20427138;

    AsyncCollective<GroupsAddressList> _collective;
    JobRegistry& _job_registry;

    int _group_comm_counter {1};
    bool _group_comm_reduce_ongoing {false};
    PeriodicEvent<1000> _periodic_group_comm_check;

    tsl::robin_map<ctx_id_t, int> _ctx_id_to_job_id;
    tsl::robin_set<int> _relevant_group_ids;

public:
    GroupCommBuilder(MPI_Comm comm, JobRegistry& registry) : _collective(comm, MyMpi::getMessageQueue(), GROUP_COMM_INSTANCE_ID), _job_registry(registry) {}

    void advance(float time) {
        if (_group_comm_reduce_ongoing) return;
        if (!_periodic_group_comm_check.ready(time)) return;

        GroupsAddressList list;
        for (auto& [id, job] : _job_registry.getJobMap()) {
            if (job->getJobTree().isRoot() && job->getDescription().getGroupId() > 0) {
                auto ctxId = job->getContextId();
                int groupId = job->getDescription().getGroupId();
                list.list.push_back({MyMpi::rank(MPI_COMM_WORLD), ctxId, groupId});
                _ctx_id_to_job_id[ctxId] = job->getId();
                _relevant_group_ids.insert(groupId);
            }
        }
        bool relevant = !list.isEmpty();

        _group_comm_reduce_ongoing = true;
        _collective.allReduce(_group_comm_counter++, list, [&, relevant, list](std::list<GroupsAddressList> results) {
            if (!relevant) return;
            assert(results.size() == 1);
            tsl::robin_map<int, std::vector<std::pair<int, ctx_id_t>>> _group_id_to_comm;
            for (auto& addr : results.begin()->list) {
                if (!_relevant_group_ids.contains(addr.groupId)) continue;
                _group_id_to_comm[addr.groupId].push_back({addr.worldRank, addr.contextId});
            }
            for (auto& [rank, ctxId, groupId] : list.list) {
                int jobId = _ctx_id_to_job_id.at(ctxId);
                GroupComm comm(_group_id_to_comm.at(groupId), ctxId);
                _job_registry.get(jobId).setGroupComm(std::move(comm));
            }
            _ctx_id_to_job_id.clear();
            _relevant_group_ids.clear();
            _group_comm_reduce_ongoing = false;
        });
    }
};
