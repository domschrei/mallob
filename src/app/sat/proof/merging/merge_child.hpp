
#pragma once

#include "app/sat/proof/serialized_lrat_line.hpp"
#include "util/sys/atomics.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"

class MergeChild : public MergeSourceInterface<SerializedLratLine> {

private:
    const static int MERGE_CHILD_BUFFER_SIZE = 100'000;
    const int rankWithinComm;
    SPSCBlockingRingbuffer<SerializedLratLine> buffer;
    bool refillRequested = false;

public:
    MergeChild(int rankWithinComm) : 
            rankWithinComm(rankWithinComm), buffer(MERGE_CHILD_BUFFER_SIZE) {}

    int getRankWithinComm() const {return rankWithinComm;}
    bool isEmpty() const {return buffer.empty();}
    bool isExhausted() const {return buffer.exhausted();}
    bool hasNext() const {return !isEmpty();}
    bool isRefillDesired() const {
        return !isExhausted() && !refillRequested 
            && buffer.size() < 5'000; 
    }

    void add(std::vector<SerializedLratLine>&& newLines) {
        refillRequested = false;
        for (auto& line : newLines) {
            buffer.pushBlocking(line);
        }
    }
    void next(SerializedLratLine& line) {
        buffer.pollBlocking(line);
    }
    bool pollBlocking(SerializedLratLine& line) override {
        return buffer.pollBlocking(line);
    }

    void conclude() {buffer.markExhausted();}
    void setRefillRequested(bool requested) {refillRequested = requested;}

    int getNumLinesInBuffer() const {
        return buffer.size();
    }
};
