
#pragma once

#include "data/app_configuration.hpp"
struct QbfContext {

    int rootJobId;
    int nodeJobId;
    int depth;
    int parentRank;
    bool isRootNode;

    int nbDoneChildren {0};
    int nbTotalChildren {0};

    QbfContext(int nodeJobId, const AppConfiguration& source) {
        rootJobId = source.getIntOrDefault("root_job_id", nodeJobId);
        this->nodeJobId = nodeJobId;
        depth = source.getIntOrDefault("depth", 0);
        parentRank = source.getIntOrDefault("parent_rank", -1);
        isRootNode = rootJobId == nodeJobId;
        nbDoneChildren = source.getIntOrDefault("done_children", 0);
        nbTotalChildren = source.getIntOrDefault("total_children", 0);
    }

    void writeToAppConfig(AppConfiguration& dest) const {
        dest.setInt("root_job_id", rootJobId);
        dest.setInt("depth", depth);
        dest.setInt("parent_rank", parentRank);
        dest.setInt("done_children", nbDoneChildren);
        dest.setInt("total_children", nbTotalChildren);
    }

    QbfContext deriveChildContext(int myRank) const {
        QbfContext childCtx(*this);
        childCtx.depth++;
        childCtx.parentRank = myRank;
        return childCtx;
    }
};
