
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include <string>

#include "robin_map.h"
#include "util/logger.hpp"
#include "util/random.hpp"

struct VariableVoting {

    struct Vote {int var; int votes;};
    std::vector<Vote> voting;

    VariableVoting() {}
    VariableVoting(const tsl::robin_map<int, int>& map) {
        load(map);
    }

    void load(const tsl::robin_map<int, int>& map) {
        voting.clear();
        for (auto [var, votes] : map) {
            voting.push_back({var, votes});
        }
        std::sort(voting.begin(), voting.end(), [&](const Vote& left, const Vote& right) {
            if (left.votes != right.votes) return left.votes > right.votes;
            return left.var < right.var;
        });
    }

    void merge(const VariableVoting& other) {
        tsl::robin_map<int, int> map;
        for (auto vote : voting) map[vote.var] += vote.votes;
        for (auto vote : other.voting) map[vote.var] += vote.votes;
        load(map);
    }

    std::vector<int> getWinners(int maxNumWinners, int minNumVotes, SplitMix64Rng* rng = nullptr) {
        // add first k variables to the winners
        std::vector<int> winners;
        int lastBlockVotes = 0;
        int lastBlockBegin = -1;
        for (size_t i = 0; winners.size() < maxNumWinners && i < voting.size(); i++) {
            Vote vote = voting[i];
            if (vote.votes < minNumVotes) break;
            winners.push_back(vote.var);
            if (vote.votes != lastBlockVotes) {
                lastBlockVotes = vote.votes;
                lastBlockBegin = i;
            }
        }
        // base cases to return winners immediately
        if (winners.empty() || winners.size()==voting.size() || !rng) return winners;

        // randomly sample winners from the correct block of equal votes 
        int lastBlockEnd = winners.size();
        winners.resize(lastBlockBegin); // truncate winners to remove the winners due to sorting
        // find the end of the block of equal votes
        while (lastBlockEnd < voting.size() && voting[lastBlockEnd].votes == lastBlockVotes) lastBlockEnd++;
        // randomly select necessary number of winners from the block
        assert(maxNumWinners-winners.size() <= lastBlockEnd-lastBlockBegin);
        auto selection = random_choice_k_from_n(voting.data()+lastBlockBegin, lastBlockEnd-lastBlockBegin,
            maxNumWinners-winners.size(), [&]() {return rng->randomInRange(0, 1);});
        // add selection to winners
        for (auto vote : selection) winners.push_back(vote.var);
        assert(winners.size() <= maxNumWinners);
        return winners;
    }

    std::vector<int> serialize() const {
        std::vector<int> out;
        out.push_back(voting.size());
        for (auto& vote : voting) {
            out.push_back(vote.var);
            out.push_back(vote.votes);
        }
        return out;
    }
    static size_t getSerializedSize(const std::vector<int>& packed) {
        int size = packed[0];
        return 1 + 2*size;
    }
    void deserialize(const std::vector<int>& packed) {
        deserialize(packed.data(), packed.size());
    }
    void deserialize(const int* packed, size_t datalen) {
        assert(datalen > 0);
        voting.clear();
        int size = packed[0];
        for (size_t i = 1; i <= size; i++) {
            assert(2*i < datalen);
            voting.push_back({packed[2*i - 1], packed[2*i]});
        }
    }

    std::string toStr(int maxEntries = INT32_MAX) const {
        std::string out;
        if (voting.empty()) return out;
        int nbEntries = 0;
        for (auto vote : voting) {
            out += std::to_string(vote.var) + ":" + std::to_string(vote.votes) + " ";
            nbEntries++;
            if (nbEntries == maxEntries) break;
        }
        return out.substr(0, out.size()-1);
    }
};
