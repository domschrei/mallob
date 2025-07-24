
#pragma once

#include <vector>

#include "app/sat/sharing/buffer/buffer_reducer.hpp"
#include "util/params.hpp"

class InPlaceClauseFiltering {

private:
    const Parameters& _params;
    int* _clause_buffer;
    size_t _clause_bufsize;
    const int* _filter;
    size_t _filter_size;

    int _num_cls {0};
    int _num_admitted_cls {0};

public:
    InPlaceClauseFiltering(const Parameters& params, int* clauseBuffer, size_t clauseBufsize, const int* filterBitvec, size_t filterSize) :
        _params(params), _clause_buffer(clauseBuffer), _clause_bufsize(clauseBufsize), 
        _filter(filterBitvec), _filter_size(filterSize) {}

    InPlaceClauseFiltering(const Parameters& params, std::vector<int>& clauseBuffer, const int* filterBitvec, size_t filterSize) :
        InPlaceClauseFiltering(params, clauseBuffer.data(), clauseBuffer.size(), filterBitvec, filterSize) {}
    
    InPlaceClauseFiltering(const Parameters& params, std::vector<int>& clauseBuffer, const std::vector<int>& filterBitvec) :
        InPlaceClauseFiltering(params, clauseBuffer, filterBitvec.data(), filterBitvec.size()) {}

    int applyAndGetNewSize() {

        const int bitsPerElem = sizeof(int)*8;
		int shift = bitsPerElem;
		int filterPos = -1 + (ClauseMetadata::enabled() ? 2 : 0);

        BufferReducer reducer(_clause_buffer, _clause_bufsize, 
            _params.strictClauseLengthLimit()+ClauseMetadata::numInts(), _params.groupClausesByLengthLbdSum());

        size_t newSize = reducer.reduce([&]() {
			_num_cls++;
			if (shift == bitsPerElem) {
				filterPos++;
				shift = 0;
			}
			int flags = filterPos < _filter_size ? _filter[filterPos] : 0;
			bool admitted = ((flags & (1 << shift)) == 0);
			if (admitted) {
				_num_admitted_cls++;
			}
			shift++;
			return admitted;
		});

        return newSize;
    }

    int getNumClauses() const {
        return _num_cls;
    }

    int getNumAdmittedClauses() const {
        return _num_admitted_cls;
    }
};
