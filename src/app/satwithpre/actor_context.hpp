
#pragma once

#include "app/satwithpre/sat_preprocess_actor.hpp"
#include <memory>
#include <vector>

struct ActorContext {
    std::string id;
    enum ActorType {SATSUMA_INT, SATSUMA_EXT, KISSAT, LINGELING, MALLOBSAT, MALLOBSWEEP} type;
    ActorContext* prerequisite {nullptr};
    std::vector<ActorContext*> actorsBeingDisplaced;
    bool onlyStartIfPrerequisiteSimplified {false};

    std::unique_ptr<SatPreprocessActor> actor;
    enum ActiveActorState {UNINITIALIZED, RUNNING, FINISHED} state {UNINITIALIZED};
    SatPreprocessActor::PreprocessActorResult result {SatPreprocessActor::PENDING};
    std::vector<int> formula;
    std::vector<int> model;
    float timeOfSignalledDisplacement {0};

    const char* getId() const {
        return id.c_str();
    }
};
