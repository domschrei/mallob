
#include "clause_buffer_lbd_scrambler.hpp"

#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "util/random.hpp"
#include "util/tsl/robin_map.h"
#include "util/assert.hpp"

ClauseBufferLbdScrambler::ClauseBufferLbdScrambler(const Parameters& params, BufferReader& reader) : _params(params), _reader(reader) {}

std::vector<int> ClauseBufferLbdScrambler::scrambleLbdScores() {
    BufferBuilder builder(-1, _params.strictClauseLengthLimit()+ClauseMetadata::numInts(),
        false);

    std::vector<int*> clausePtrs;
    int clauseLength = 1;
    tsl::robin_map<int, int> lbdToNbOccs;
    Mallob::Clause c;
    while (true) {

        c = _reader.getNextIncomingClause();
        if (c.begin == nullptr || c.size > clauseLength) {
            // flush clauses of prior clause length
            // 1. shuffle clauses
            random_shuffle(clausePtrs.data(), clausePtrs.size());
            // 2. append shuffled clauses to builder with tracked LBD scores
            size_t shuffledClauseIdx = 0;
            for (int lbd = 1; lbd <= clauseLength; lbd++) {
                if (!lbdToNbOccs.count(lbd)) continue;
                // append as many clauses as this particular LBD occurred
                std::vector<Mallob::Clause> clauses;
                for (int i = 0; i < lbdToNbOccs[lbd]; i++) {
                    assert(shuffledClauseIdx < clausePtrs.size());
                    clauses.emplace_back(clausePtrs[shuffledClauseIdx++], clauseLength, lbd);
                }
                // need to sort clauses for BufferBuilder
                std::sort(clauses.begin(), clauses.end());
                for (auto& clause : clauses) {
                    bool success = builder.append(clause);
                    assert(success);
                }
            }
            assert(shuffledClauseIdx == clausePtrs.size());
            // 3. reset data structures
            clausePtrs.clear();
            lbdToNbOccs.clear();
            clauseLength = c.size;
        }

        if (c.begin == nullptr) break;

        clausePtrs.push_back(c.begin);
        lbdToNbOccs[c.lbd]++;
    }

    return builder.extractBuffer();
}
