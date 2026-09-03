
#pragma once

#include "app/satwithpre/actor_config_parser.hpp"
#include "app/satwithpre/actor_context.hpp"
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
#include <cstdlib>
#include <list>

class PreprocessorOrchestrator {

private:
    const Parameters& _params;
    const JobDescription& _desc;
    APIConnector& _api;

    std::list<ActorContext> _actors;
    const std::vector<int> _base_cnf;

    float _time_of_start;
    ActorContext* _winning_actor {nullptr};

public:
    PreprocessorOrchestrator(const Parameters& params, const JobDescription& desc, APIConnector& api) : _params(params), _desc(desc), _api(api),
            _base_cnf(getCnfFromJobDescription()) {

        _time_of_start = Timer::elapsedSeconds();
        try {
            _actors = ActorConfigParser().parseFile(_params.preprocessConfig());
        } catch (const std::runtime_error& e) {
            LOG(V0_CRIT, "[ERROR] Parsing error for preprocess actor config file \"%s\": %s\n",
                _params.preprocessConfig().c_str(), e.what());
            abort();
        }
    }

    int loop() {

        int actorIdx = -1;
        int nbRemainingActors = 0;
        for (auto& actor : _actors) {
            actorIdx++;
            nbRemainingActors += (actor.state != ActorContext::FINISHED);

            if (actor.state == ActorContext::UNINITIALIZED) {

                // check prerequisite
                if (actor.prerequisite && actor.prerequisite->state != ActorContext::FINISHED)
                    continue; // prerequisite not done yet - skip for now
                if (actor.prerequisite && actor.onlyStartIfPrerequisiteSimplified && actor.prerequisite->result != SatPreprocessActor::SIMPLIFIED)
                    continue; // never initialize this actor since its prerequisite didn't lead to a simplification

                // prerequisite done: initialize actor
                auto formula = actor.prerequisite ? actor.prerequisite->formula : _base_cnf;
                auto name = std::to_string(actorIdx) + ":";
                switch (actor.type) {
                case ActorContext::SATSUMA_INT:
                    name += "SatsumaInt";
                    actor.actor.reset(new SatsumaPreprocessor(_params, _desc, name, std::move(formula)));
                    break;
                case ActorContext::SATSUMA_EXT:
                    name += "SatsumaExt";
                    actor.actor.reset(new ExtSatsumaCaller(_params, _desc, name, std::move(formula)));
                    break;
                case ActorContext::KISSAT:
                    name += "Kissat";
                    actor.actor.reset(new KissatPreprocessor(_params, _desc, name, std::move(formula)));
                    break;
                case ActorContext::LINGELING:
                    name += "Lingeling";
                    actor.actor.reset(new LingelingPreprocessor(_params, _desc, name, std::move(formula)));
                    break;
                case ActorContext::MALLOBSAT:
                    name += "MallobSat";
                    actor.actor.reset(new MallobSatPreprocessActor(_params, _desc, name, _api, std::move(formula), _time_of_start));
                    break;
                }
                actor.id += ":" + name;
                LOG(V2_INFO, "SATWP launch %s\n", actor.getId());
                actor.actor->preprocessAsync();
                actor.state = ActorContext::RUNNING;

                // signal displacement to actors being displaced
                for (auto& other : actor.actorsBeingDisplaced) {
                    if (other->timeOfSignalledDisplacement <= 0) {
                        if (other->actor)
                            LOG(V2_INFO, "SATWP %s --displace--> %s\n", actor.getId(), other->getId());
                        else
                            LOG(V2_INFO, "SATWP %s --displace--\n", actor.getId());
                        other->timeOfSignalledDisplacement = Timer::elapsedSeconds();
                    }
                }
            }

            if (actor.state == ActorContext::RUNNING && actor.actor->isDonePreprocessing()) {
                // this actor is done
                auto res = actor.actor->getPreprocessingResult();
                LOG(V2_INFO, "SATWP %s done, result %s\n", actor.getId(),
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
                    LOG(V2_INFO, "SATWP %s found SAT\n", actor.getId());
                    _winning_actor = &actor;

                    return 10;
                }
                if (res == SatPreprocessActor::UNSAT) {
                    LOG(V2_INFO, "SATWP %s found UNSAT\n", actor.getId());
                    _winning_actor = &actor;
                    return 20;
                }
                assert(actor.formula[0] != 0); // no empty clause without reporting UNSAT!
            }

            if (actor.state == ActorContext::RUNNING && actor.timeOfSignalledDisplacement > 0) {
                // this actor is being displaced (after some time)
                if (_params.preprocessBalancing() == 0) {
                    LOG(V2_INFO, "SATWP %s interrupt\n", actor.getId());
                    actor.actor->interrupt();
                    actor.timeOfSignalledDisplacement = 0;
                }
                if (_params.preprocessBalancing() == 1 && Timer::elapsedSeconds() - _time_of_start >=
                    _params.preprocessExpansionFactor() * (actor.timeOfSignalledDisplacement - _time_of_start)) {
                    LOG(V2_INFO, "SATWP %s interrupt\n", actor.getId());
                    actor.actor->interrupt();
                    actor.timeOfSignalledDisplacement = 0;
                }
            }
        }

        return nbRemainingActors == 0 ? -1 : 0;
    }

    std::vector<int> getModel() {
        auto actor = _winning_actor;
        assert(actor);
        auto model = std::move(actor->model);
        checkModel(actor->actor->getInputCnf(), model);
        LOG(V2_INFO, "SATWP Checked model @ %s, size %lu\n", actor->getId(), model.size()-1);
        while (true) {
            actor = actor->prerequisite;
            if (!actor) break;
            actor->actor->reconstructSolution(model);
            LOG(V2_INFO, "SATWP Reconstructed model @ %s, size %lu\n", actor->getId(), model.size()-1);
            checkModel(actor->actor->getInputCnf(), model);
            LOG(V2_INFO, "SATWP Checked model @ %s\n", actor->getId());
        }
        return model;
    }

    void stopAll() {
        for (auto& actor : _actors) if (actor.state == ActorContext::RUNNING) {
            LOG(V2_INFO, "SATWP %s interrupt\n", actor.getId());
            actor.actor->interrupt();
        }
    }

private:
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

    void checkModel(const std::vector<int>& formula, const std::vector<int>& model) {
        bool clauseSatisfied = false;
        int clauseNo = 1;
        for (int i = 0; i < formula.size()-2; i++) {
            int lit = formula[i];
            if (lit == 0) {
                if (!clauseSatisfied) {
                    LOG(V0_CRIT, "[ERROR] Clause # %i at position %i not satisfied by model!\n", clauseNo, i);
                    abort();
                }
                clauseNo++;
                clauseSatisfied = false;
                continue;
            }
            assert(std::abs(lit) < model.size());
            int modelLit = model[std::abs(lit)];
            assert(modelLit == lit || modelLit == -lit);
            if (modelLit == lit) clauseSatisfied = true;
        }
        assert(formula[formula.size()-3] == 0);
    }
};
