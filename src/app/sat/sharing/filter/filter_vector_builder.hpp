
#pragma once

#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "util/logger.hpp"

class FilterVectorBuilder {

private:
    size_t _header;
    bool _verb;

public:
    FilterVectorBuilder(size_t header, int epoch, bool verbose) : _header(header), _verb(verbose) {}

    std::vector<int> build(BufferReader& reader, std::function<bool(Mallob::Clause&)> accept,
        std::function<void(int)> lock = [](int) {}, std::function<void(int)> unlock = [](int) {}) {
       
        constexpr auto bitsPerElem = 8*sizeof(int);
        int shift = bitsPerElem;
        auto clause = reader.getNextIncomingClause();
        int filterPos = -1 + (ClauseMetadata::enabled() ? 2 : 0);
        int nbFiltered = 0;
        int nbTotal = 0;

        std::vector<int> result;

        if (ClauseMetadata::enabled()) {
            auto id = _header; //_id_alignment ? _id_alignment->contributeFirstClauseIdOfEpoch() : 0UL;
            result.resize(sizeof(unsigned long) / sizeof(int));
            memcpy(result.data(), &id, sizeof(unsigned long));
        }

        int filterSizeBeingLocked = -1;
        while (clause.begin != nullptr) {
            ++nbTotal;

            if (filterSizeBeingLocked != clause.size) {
                if (filterSizeBeingLocked != -1) unlock(filterSizeBeingLocked);
                filterSizeBeingLocked = clause.size;
                lock(filterSizeBeingLocked);
            }

            if (shift == bitsPerElem) {
                ++filterPos;
                result.push_back(0);
                shift = 0;
            }
            
            if (!accept(clause)) {
                // filtered!
                auto bitFiltered = 1 << shift;
                result[filterPos] |= bitFiltered;
                ++nbFiltered;
            }
            
            ++shift;
            clause = reader.getNextIncomingClause();
        }
        if (filterSizeBeingLocked != -1) unlock(filterSizeBeingLocked);

        if (_verb)
            LOG(V4_VVER, "filtered %i/%i\n", nbFiltered, nbTotal);
        else
            LOG(V5_DEBG, "filtered %i/%i\n", nbFiltered, nbTotal);
        return result;
    }
};
