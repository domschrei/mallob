
#pragma once

#include <vector>

#include "app/sat/sharing/buffer/buffer_reader.hpp"

class Parameters;

class ClauseBufferLbdScrambler {

private:
    const Parameters& _params;
    BufferReader _reader;

public:
    ClauseBufferLbdScrambler(const Parameters& params, BufferReader& reader);
    std::vector<int> scrambleLbdScores();
};
