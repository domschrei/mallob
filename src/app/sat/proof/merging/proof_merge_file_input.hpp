
#pragma once

#include "../serialized_lrat_line.hpp"
#include "util/merge_source_interface.hpp"
#include "../lrat_utils.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"

class ProofMergeFileInput : public MergeSourceInterface<SerializedLratLine> {

private:
    std::ifstream _ifs;
    lrat_utils::ReadBuffer _readbuf;

public:
    ProofMergeFileInput(const std::string& inputFilename) : 
        _ifs(inputFilename, std::ios::binary), _readbuf(_ifs) {}

    bool pollBlocking(SerializedLratLine& elem) override {
        return lrat_utils::readLine(_readbuf, elem);
    }
    size_t getCurrentSize() const override {
        return 0;
    }
};
