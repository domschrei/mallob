
#pragma once

#include "app/sat/proof/serialized_lrat_line.hpp"
#include "util/sys/atomics.hpp"

class MergeChild {

private:
    const static int MERGE_CHILD_BUFFER_SIZE = 100'000;
    const int rankWithinComm;
    const int fullChunkSize;
    std::vector<SerializedLratLine> buffer;
    std::atomic_int bufferSize = 0;
    std::atomic_int readPos = 0;
    std::atomic_int writePos = 0;
    bool exhausted = false;
    bool refillRequested = false;

public:
    MergeChild(int rankWithinComm, int fullChunkSize) : 
            rankWithinComm(rankWithinComm), fullChunkSize(fullChunkSize) {
        buffer.resize(MERGE_CHILD_BUFFER_SIZE);
    }

    int getRankWithinComm() const {return rankWithinComm;}
    bool isEmpty() const {return bufferSize.load(std::memory_order_relaxed) == 0;}
    bool isExhausted() const {return exhausted;}
    bool hasNext() const {return !isEmpty();}
    bool isRefillDesired() const {
        return !isExhausted() && !refillRequested 
            && bufferSize.load(std::memory_order_relaxed) < fullChunkSize; 
    }

    void add(std::vector<SerializedLratLine>&& newLines) {
        int pos = writePos.load(std::memory_order_relaxed);
        int size = 0;
        for (auto& line : newLines) {
            size += line.size();
            buffer[pos].data().swap(line.data());
            pos++;
            if (pos == MERGE_CHILD_BUFFER_SIZE) pos = 0;
        }
        bufferSize.fetch_add(size, std::memory_order_relaxed);
        writePos.store(pos, std::memory_order_relaxed);
        refillRequested = false;
    }
    void next(SerializedLratLine& line) {
        assert(hasNext());
        int pos = readPos.load(std::memory_order_relaxed);
        line.data().swap(buffer[pos].data());
        pos++;
        if (pos == MERGE_CHILD_BUFFER_SIZE) pos = 0;
        readPos.store(pos, std::memory_order_relaxed);
        bufferSize.fetch_sub(line.size(), std::memory_order_relaxed);
    }
    void conclude() {exhausted = true;}
    void setRefillRequested(bool requested) {refillRequested = requested;}
};
