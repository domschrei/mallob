
#pragma once

#include "app/sat/execution/clause_shuffler.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "util/params.hpp"

class ClauseBufferLbdScrambler {

private:
    const Parameters& _params;
    BufferReader _reader;

public:
    ClauseBufferLbdScrambler(const Parameters& params, BufferReader& reader);
    std::vector<int> scrambleLbdScores();
};
