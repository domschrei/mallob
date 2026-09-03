
#pragma once

#include <fstream>
#include <list>
#include <stdexcept>
#include <string>
#include <vector>

#include "robin_map.h"
#include "robin_set.h"
#include "util/json.hpp"
#include "app/satwithpre/actor_context.hpp"

// ---------------------------------------------------------------------------
// Parses JSON configs of the form:
//
// [
//   {
//     "id": "sat1",
//     "type": "SATSUMA_INT",
//     "prerequisite": null,
//     "actorsBeingDisplaced": [],
//     "onlyStartIfPrerequisiteSimplified": false
//   },
//   {
//     "id": "kissat1",
//     "type": "KISSAT",
//     "prerequisite": "sat1",
//     "actorsBeingDisplaced": ["sat1"],
//     "onlyStartIfPrerequisiteSimplified": true
//   }
// ]
//
// A top-level object of the form {"actors": [...]} is also accepted.
// ---------------------------------------------------------------------------
// Initial version created via Claude 5 Sonnet (medium) on 2026-09-02.
class ActorConfigParser {
public:
    class ParseError : public std::runtime_error {
    public:
        explicit ParseError(const std::string& what) : std::runtime_error(what) {}
    };

    // Reads and parses a JSON file from disk.
    static std::list<ActorContext> parseFile(const std::string& path) {
        std::ifstream in(path);
        if (!in) {
            throw ParseError("ActorConfigParser: could not open file '" + path + "'");
        }
        nlohmann::json root;
        try {
            in >> root;
        } catch (const nlohmann::json::parse_error& e) {
            throw ParseError(std::string("ActorConfigParser: JSON parse error in '") + path +
                              "': " + e.what());
        }
        return parse(root);
    }

    // Parses an already-loaded JSON document.
    static std::list<ActorContext> parse(const nlohmann::json& root) {
        const nlohmann::json& array = extractActorArray(root);

        std::list<ActorContext> actors;
        tsl::robin_map<std::string, ActorContext*> byId;

        // Side table: id -> (prerequisite id or empty, displaced ids)
        // kept separate because ActorContext itself only stores pointers,
        // not the raw id strings needed to resolve them.
        struct PendingLinks {
            std::string prerequisiteId;   // empty == no prerequisite
            bool hasPrerequisite = false;
            std::vector<std::string> displacedIds;
        };
        tsl::robin_map<std::string, PendingLinks> pending;

        // --- Pass 1: create every ActorContext, so all addresses are stable.
        for (const auto& entry : array) {
            requireField(entry, "id");
            requireField(entry, "type");

            std::string id = entry.at("id").get<std::string>();
            if (id.empty()) {
                throw ParseError("ActorConfigParser: encountered an actor with an empty id");
            }
            if (byId.count(id)) {
                throw ParseError("ActorConfigParser: duplicate actor id '" + id + "'");
            }

            ActorContext ctx;
            ctx.id = id;
            ctx.type = parseType(entry.at("type").get<std::string>());
            ctx.onlyStartIfPrerequisiteSimplified =
                entry.value("onlyStartIfPrerequisiteSimplified", false);
            ctx.options = entry.count("options") ? entry.at("options").get<std::string>() : "";
            ctx.groupId = entry.count("group-id") ? entry.at("group-id").get<std::string>() : "";

            actors.push_back(std::move(ctx));
            byId[id] = &actors.back();

            PendingLinks links;
            if (entry.contains("prerequisite") && !entry.at("prerequisite").is_null()) {
                links.hasPrerequisite = true;
                links.prerequisiteId = entry.at("prerequisite").get<std::string>();
            }
            if (entry.contains("actorsBeingDisplaced")) {
                const auto& disp = entry.at("actorsBeingDisplaced");
                if (!disp.is_array()) {
                    throw ParseError("ActorConfigParser: actor '" + id +
                                      "': 'actorsBeingDisplaced' must be an array");
                }
                for (const auto& d : disp) {
                    links.displacedIds.push_back(d.get<std::string>());
                }
            }
            pending.emplace(id, std::move(links));
        }

        // --- Pass 2: resolve ids to pointers now that every actor exists.
        for (auto& ctx : actors) {
            const PendingLinks& links = pending.at(ctx.id);

            if (links.hasPrerequisite) {
                auto it = byId.find(links.prerequisiteId);
                if (it == byId.end()) {
                    throw ParseError("ActorConfigParser: actor '" + ctx.id +
                                      "' references unknown prerequisite '" +
                                      links.prerequisiteId + "'");
                }
                if (it->second == &ctx) {
                    throw ParseError("ActorConfigParser: actor '" + ctx.id +
                                      "' cannot be its own prerequisite");
                }
                ctx.prerequisite = it->second;
            }

            ctx.actorsBeingDisplaced.reserve(links.displacedIds.size());
            for (const auto& dispId : links.displacedIds) {
                auto it = byId.find(dispId);
                if (it == byId.end()) {
                    throw ParseError("ActorConfigParser: actor '" + ctx.id +
                                      "' references unknown displaced actor '" + dispId + "'");
                }
                ctx.actorsBeingDisplaced.push_back(it->second);
            }
        }

        // --- Sanity check: prerequisite chains must not form a cycle.
        checkPrerequisiteChainsAcyclic(actors);

        return actors;
    }

    // Serializes a single ActorContext back to JSON (pointers become ids).
    // Handy for round-tripping / debugging.
    static nlohmann::json toJson(const ActorContext& ctx) {
        nlohmann::json j;
        j["id"] = ctx.id;
        j["type"] = typeToString(ctx.type);
        j["prerequisite"] = ctx.prerequisite ? nlohmann::json(ctx.prerequisite->id)
                                              : nlohmann::json(nullptr);
        j["actorsBeingDisplaced"] = nlohmann::json::array();
        for (const auto* d : ctx.actorsBeingDisplaced) {
            j["actorsBeingDisplaced"].push_back(d->id);
        }
        j["onlyStartIfPrerequisiteSimplified"] = ctx.onlyStartIfPrerequisiteSimplified;
        return j;
    }

private:
    static const nlohmann::json& extractActorArray(const nlohmann::json& root) {
        if (root.is_array()) {
            return root;
        }
        if (root.is_object() && root.contains("actors")) {
            const nlohmann::json& actorsField = root.at("actors");
            if (!actorsField.is_array()) {
                throw ParseError("ActorConfigParser: 'actors' field must be an array");
            }
            return actorsField;
        }
        throw ParseError(
            "ActorConfigParser: root JSON must be an array of actors, "
            "or an object with an 'actors' array field");
    }

    static void requireField(const nlohmann::json& entry, const char* field) {
        if (!entry.contains(field)) {
            throw ParseError(std::string("ActorConfigParser: actor entry missing required field '") +
                              field + "'");
        }
    }

    static ActorContext::ActorType parseType(const std::string& s) {
        static const tsl::robin_map<std::string, ActorContext::ActorType> map = {
            {"SATSUMA_INT", ActorContext::SATSUMA_INT},
            {"SATSUMA_EXT", ActorContext::SATSUMA_EXT},
            {"KISSAT", ActorContext::KISSAT},
            {"LINGELING", ActorContext::LINGELING},
            {"MALLOBSAT", ActorContext::MALLOBSAT},
            {"MALLOBSWEEP", ActorContext::MALLOBSWEEP},
        };
        auto it = map.find(s);
        if (it == map.end()) {
            throw ParseError("ActorConfigParser: unknown actor type '" + s + "'");
        }
        return it->second;
    }

    static const char* typeToString(ActorContext::ActorType t) {
        switch (t) {
            case ActorContext::SATSUMA_INT: return "SATSUMA_INT";
            case ActorContext::SATSUMA_EXT: return "SATSUMA_EXT";
            case ActorContext::KISSAT: return "KISSAT";
            case ActorContext::LINGELING: return "LINGELING";
            case ActorContext::MALLOBSAT: return "MALLOBSAT";
            case ActorContext::MALLOBSWEEP: return "MALLOBSWEEP";
        }
        throw ParseError("ActorConfigParser: unhandled ActorType in typeToString");
    }

    // Follows the prerequisite pointer chain from every actor and throws if
    // it ever loops back on itself, since the setup is meant to be a DAG.
    static void checkPrerequisiteChainsAcyclic(const std::list<ActorContext>& actors) {
        for (const auto& start : actors) {
            tsl::robin_set<const ActorContext*> visited;
            const ActorContext* cur = &start;
            while (cur != nullptr) {
                if (!visited.insert(cur).second) {
                    throw ParseError(
                        "ActorConfigParser: cycle detected in prerequisite chain starting at '" +
                        start.id + "'");
                }
                cur = cur->prerequisite;
            }
        }
    }
};
