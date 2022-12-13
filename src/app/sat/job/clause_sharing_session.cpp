
#include "clause_sharing_session.hpp"

std::vector<int> ClauseSharingSession::applyGlobalFilter(const std::vector<int>& filter, std::vector<int>& clauses) {

    size_t clsIdx = 0;
    size_t filterIdx = 0;
    constexpr auto bitsPerElem = 8*sizeof(int);
    auto reader = _cdb.getBufferReader(clauses.data(), clauses.size());
    auto writer = _cdb.getBufferBuilder();

    auto clause = reader.getNextIncomingClause();
    while (clause.begin != nullptr) {

        int filterInt = filter[filterIdx];
        auto bit = 1 << (clsIdx % bitsPerElem);
        if ((filterInt & bit) == 0) {
            // Clause passed
            writer.append(clause);
        }
        
        clsIdx++;
        if (clsIdx % bitsPerElem == 0) filterIdx++;
        clause = reader.getNextIncomingClause();
    }

    _num_broadcast_clauses = clsIdx;
    _num_admitted_clauses = writer.getNumAddedClauses();

    return writer.extractBuffer();
}
