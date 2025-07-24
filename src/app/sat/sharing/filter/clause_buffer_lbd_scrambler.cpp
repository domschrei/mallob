
#include "clause_buffer_lbd_scrambler.hpp"

#include <assert.h>
#include <stddef.h>
#include <algorithm>

#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "util/tsl/robin_map.h"
#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "robin_hash.h"
#include "util/option.hpp"

ClauseBufferLbdScrambler::ClauseBufferLbdScrambler(const Parameters& params, BufferReader& reader) : _params(params), _reader(reader) {}

std::vector<int> ClauseBufferLbdScrambler::scrambleLbdScores() {
    std::vector<int> outbuf;
    outbuf.reserve(_reader.getBufferSize());
    BufferBuilder builder(-1, _params.strictClauseLengthLimit()+ClauseMetadata::numInts(),
        false, &outbuf);

    std::vector<int*> clausePtrs;
    int clauseLength = 2;
    tsl::robin_map<int, int> lbdToNbOccs;
    Mallob::Clause c;
    while (true) {

        c = _reader.getNextIncomingClause();
        // skip unit clauses (nothing to shuffle)
        if (c.begin != nullptr && c.size < clauseLength) {
            builder.append(c);
            continue;
        }
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
