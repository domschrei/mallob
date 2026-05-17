
#pragma once

#include "app/satwithpre/ext_satsuma_caller.hpp"
#include "app/satwithpre/kissat_preprocessor.hpp"
#include "app/satwithpre/lingeling_preprocessor.hpp"
#include "app/satwithpre/mallobsat_preprocess_actor.hpp"
#include "app/satwithpre/sat_preprocess_actor.hpp"
#include "app/satwithpre/satsuma_preprocessor.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include <list>

class PreprocessorOrchestrator {

private:
    const Parameters& _params;
    const JobDescription& _desc;
    APIConnector& _api;

    struct ActorContext {
        enum ActorType {SATSUMA_INT, SATSUMA_EXT, KISSAT, LINGELING, MALLOBSAT} type;
        ActorContext* prerequisite {nullptr};
        std::vector<ActorContext*> actorsBeingDisplaced;

        std::unique_ptr<SatPreprocessActor> actor;
        enum ActiveActorState {UNINITIALIZED, RUNNING, FINISHED} state {UNINITIALIZED};
        SatPreprocessActor::PreprocessActorResult result {SatPreprocessActor::PENDING};
        std::vector<int> formula;
        std::vector<int> model;
        float timeOfSignalledDisplacement {0};
    };
    std::list<ActorContext> _actors;
    const std::vector<int> _base_cnf;

    float _time_of_start;
    ActorContext* _winning_actor {nullptr};

public:
    PreprocessorOrchestrator(const Parameters& params, const JobDescription& desc, APIConnector& api) : _params(params), _desc(desc), _api(api),
            _base_cnf(getCnfFromJobDescription()) {

        _time_of_start = Timer::elapsedSeconds();

        // Mallob on original instance
        _actors.push_back({PreprocessorOrchestrator::ActorContext::MALLOBSAT, nullptr});
        ActorContext* ctxMalOrig = &_actors.back();

        // Lingeling (SAT, UNSAT or nothing)
        _actors.push_back({PreprocessorOrchestrator::ActorContext::LINGELING, nullptr, {}});
        ActorContext* ctxLgl = &_actors.back();
        // Kissat (preprocesses the formula)
        _actors.push_back({PreprocessorOrchestrator::ActorContext::KISSAT, nullptr, {}});
        ActorContext* ctxKis = &_actors.back();
        // Satsuma (preprocesses the formula)
        _actors.push_back({PreprocessorOrchestrator::ActorContext::SATSUMA_EXT, nullptr, {}});
        ActorContext* ctxSats = &_actors.back();

        // Kissat on Satsuma-preprocessed formula (preprocesses the formula)
        _actors.push_back({PreprocessorOrchestrator::ActorContext::KISSAT, ctxSats, {}});
        ActorContext* ctxKisAfterSats = &_actors.back();

        // Mallob on Kissat-preprocessed formula - displaces original Mallob task
        _actors.push_back({PreprocessorOrchestrator::ActorContext::MALLOBSAT, ctxKis, {ctxMalOrig}});
        ActorContext* ctxMalPre1 = &_actors.back();
        // Mallob on Satsuma+Kissat-preprocessed formula - displaces all prior Mallob tasks
        _actors.push_back({PreprocessorOrchestrator::ActorContext::MALLOBSAT, ctxKisAfterSats, {ctxMalOrig, ctxMalPre1}});
        ActorContext* ctxMalPreFull = &_actors.back();
    }

    int loop() {

        int actorIdx = -1;
        for (auto& actor : _actors) {
            actorIdx++;

            if (actor.state == ActorContext::UNINITIALIZED) {

                // check prerequisite
                if (actor.prerequisite && actor.prerequisite->state != ActorContext::FINISHED)
                    continue; // prerequisite not done yet - skip for now

                // prerequisite done: initialize actor
                auto formula = (actor.prerequisite && actor.prerequisite->result == SatPreprocessActor::SIMPLIFIED) ?
                    actor.prerequisite->formula : _base_cnf;
                switch (actor.type) {
                case ActorContext::SATSUMA_INT:
                    actor.actor.reset(new SatsumaPreprocessor(_params, _desc, std::to_string(actorIdx) + ":SatsumaInt", std::move(formula)));
                    break;
                case ActorContext::SATSUMA_EXT:
                    actor.actor.reset(new ExtSatsumaCaller(_params, _desc, std::to_string(actorIdx) + ":SatsumaExt", std::move(formula)));
                    break;
                case ActorContext::KISSAT:
                    actor.actor.reset(new KissatPreprocessor(_params, _desc, std::to_string(actorIdx) + ":Kissat", std::move(formula)));
                    break;
                case ActorContext::LINGELING:
                    actor.actor.reset(new LingelingPreprocessor(_params, _desc, std::to_string(actorIdx) + ":Lingeling", std::move(formula)));
                    break;
                case ActorContext::MALLOBSAT:
                    actor.actor.reset(new MallobSatPreprocessActor(_params, _desc, std::to_string(actorIdx) + ":MallobSat", _api, std::move(formula), _time_of_start));
                    break;
                }
                LOG(V2_INFO, "SATWP launch %s\n", actor.actor->getName());
                actor.actor->preprocessAsync();
                actor.state = ActorContext::RUNNING;

                // signal displacement to actors being displaced
                for (auto& other : actor.actorsBeingDisplaced) {
                    if (other->timeOfSignalledDisplacement <= 0) {
                        if (other->actor)
                            LOG(V2_INFO, "SATWP %s --displace--> %s\n", actor.actor->getName(), other->actor->getName());
                        else
                            LOG(V2_INFO, "SATWP %s --displace--\n", actor.actor->getName());
                        other->timeOfSignalledDisplacement = Timer::elapsedSeconds();
                    }
                }
            }

            if (actor.state == ActorContext::RUNNING && actor.actor->isDonePreprocessing()) {
                // this actor is done
                auto res = actor.actor->getPreprocessingResult();
                LOG(V2_INFO, "SATWP %s done, result %s\n", actor.actor->getName(),
                    actor.actor->getPreprocessingResultAsString().c_str());
                if (res == SatPreprocessActor::SAT) {
                    actor.model = std::move(actor.actor->getModel());
                }
                if (res == SatPreprocessActor::SIMPLIFIED) {
                    actor.formula = std::move(actor.actor->getPreprocessedFormula());
                } else {
                    actor.formula = std::move(actor.actor->getInputCnf());
                }
                assert(actor.formula.size() > 2);
                assert(actor.formula[actor.formula.size() - 2] >= 0); // # vars
                assert(actor.formula[actor.formula.size() - 1] >= 0); // # clauses
                actor.result = res;
                actor.state = ActorContext::FINISHED;
                if (res == SatPreprocessActor::SAT) {
                    LOG(V2_INFO, "SATWP %s found SAT\n", actor.actor->getName());
                    _winning_actor = &actor;
                    stopAll();
                    return 10;
                }
                if (res == SatPreprocessActor::UNSAT) {
                    LOG(V2_INFO, "SATWP %s found UNSAT\n", actor.actor->getName());
                    _winning_actor = &actor;
                    stopAll();
                    return 20;
                }
                assert(actor.formula[0] != 0); // no empty clause without reporting UNSAT!
            }

            if (actor.state == ActorContext::RUNNING && actor.timeOfSignalledDisplacement > 0) {
                // this actor is being displaced (after some time)
                if (_params.preprocessBalancing() == 0) {
                    LOG(V2_INFO, "SATWP %s interrupt\n", actor.actor->getName());
                    actor.actor->interrupt();
                    actor.timeOfSignalledDisplacement = 0;
                }
                if (_params.preprocessBalancing() == 1 && Timer::elapsedSeconds() - _time_of_start >=
                    _params.preprocessExpansionFactor() * (actor.timeOfSignalledDisplacement - _time_of_start)) {
                    LOG(V2_INFO, "SATWP %s interrupt\n", actor.actor->getName());
                    actor.actor->interrupt();
                    actor.timeOfSignalledDisplacement = 0;
                }
            }
        }

        return 0;
    }

    std::vector<int> getModel() {
        auto actor = _winning_actor;
        assert(actor);
        auto model = std::move(actor->model);
        while (true) {
            actor = actor->prerequisite;
            if (!actor) break;
            actor->actor->reconstructSolution(model);
        }
        return model;
    }

private:
    void stopAll() {
        for (auto& actor : _actors) if (actor.state == ActorContext::RUNNING) {
            LOG(V2_INFO, "SATWP %s interrupt\n", actor.actor->getName());
            actor.actor->interrupt();
        }
    }

    std::vector<int> getCnfFromJobDescription() {

        SerializedFormulaParser parser(Logger::getMainInstance(), _desc.getFormulaPayload(0),
            _desc.getFormulaPayloadSize(0));
        if (_params.compressFormula()) parser.setCompressed();
        int nbVars = _desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
        int nbCls = _desc.getAppConfiguration().fixedSizeEntryToInt("__NC");

        std::vector<int> cnf;
        int lit;
        while (parser.getNextLiteral(lit)) cnf.push_back(lit);
        cnf.push_back(nbVars);
        cnf.push_back(nbCls);
        return cnf;
    }
};
