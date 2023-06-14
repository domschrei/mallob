
#pragma once

#include "app/qbf/execution/qbf_notification.hpp"
#include "app/sat/job/sat_constants.h"
#include "comm/msgtags.h"
#include "comm/mympi.hpp"
#include "data/app_configuration.hpp"

struct QbfContext {

    int rootJobId;
    int nodeJobId;
    int depth;
    int parentRank;
    int childIdx;
    bool isRootNode;

    bool cancelled {false};

    enum NodeType {AND, OR} nodeType;
    struct ChildInfo {
        bool qbfJob {false};
        int rank {-1};
        int jobId {-1};
        enum State {PREPARING, INTRODUCED, READY, DONE, CANCELLED} state {PREPARING};
    };
    std::vector<ChildInfo> children;
    int nbDoneChildren {0};

    QbfContext(int nodeJobId, const AppConfiguration& source) {
        rootJobId = source.getIntOrDefault("root_job_id", nodeJobId);
        this->nodeJobId = nodeJobId;
        depth = source.getIntOrDefault("depth", 0);
        parentRank = source.getIntOrDefault("parent_rank", -1);
        childIdx = source.getIntOrDefault("child_idx", -1);
        isRootNode = rootJobId == nodeJobId;
    }

    QbfContext deriveChildContext(int childIdx, int myRank) const {
        QbfContext childCtx(*this);
        childCtx.depth++;
        childCtx.parentRank = myRank;
        childCtx.childIdx = childIdx;
        return childCtx;
    }

    void writeToAppConfig(bool qbf, AppConfiguration& dest) const {
        dest.setInt("root_job_id", rootJobId);
        dest.setInt("depth", depth);
        dest.setInt("parent_rank", parentRank);
        dest.setInt("child_idx", childIdx);
        dest.setInt("report_to_parent", qbf?0:1);
    }

    void appendChild(bool qbfJob, int rank, int jobId) {
        children.push_back(ChildInfo{qbfJob, rank, jobId, ChildInfo::PREPARING});
    }

    void markChildAsSpawned(int childIdx) {
        auto& child = children[childIdx];
        child.state = ChildInfo::INTRODUCED;
    }

    void markChildAsReady(int childIdx, int rank, int jobId) {
        auto& child = children[childIdx];
        child.state = ChildInfo::READY;
        child.rank = rank;
        child.jobId = jobId;
    }

    // SAT-like return values: 0=UNKNOWN, 10=SAT, 20=UNSAT
    int handleNotification(QbfNotification& msg) {
        auto& child = children[msg.childIdx];
        if (child.state == ChildInfo::CANCELLED)
            return RESULT_UNKNOWN;
        child.state = ChildInfo::DONE;
        int resultCode = msg.resultCode;
        nbDoneChildren++;
        if (nbDoneChildren == children.size()) {
            return resultCode;
        }
        if (nodeType == AND && resultCode == RESULT_UNSAT) {
            return RESULT_UNSAT;
        }
        if (nodeType == OR && resultCode == RESULT_SAT) {
            return RESULT_SAT;
        }
        return RESULT_UNKNOWN;
    }

    void cancelActiveChildren() {
        if (!cancelled) return;
        for (auto child : children) {
            if (child.state != ChildInfo::READY) continue;
            if (child.qbfJob) {
                // For QBF jobs
                MyMpi::isend(child.rank, MSG_QBF_CANCEL_CHILDREN, IntVec({rootJobId, depth+1}));
            } else {
                // For SAT jobs
                MyMpi::isend(child.rank, MSG_NOTIFY_JOB_ABORTING, IntVec({child.jobId}));
            }
            child.state = ChildInfo::CANCELLED;
        }
    }

    bool isDestructible() {
        cancelActiveChildren();
        for (auto child : children) {
            if (child.state == ChildInfo::INTRODUCED || child.state == ChildInfo::READY)
                return false;
        }
        return true;
    }

private:
    int findChildIdx(int rank) {
        int childIdx = 0;
        while (childIdx < children.size() && children[childIdx].rank != rank) {
            childIdx++;
        }
        return childIdx < children.size() ? childIdx : -1;
    }
};
