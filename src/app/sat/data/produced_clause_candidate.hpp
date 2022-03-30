
#pragma once

#include "clause.hpp"

struct ProducedClauseCandidate {
    int* begin;
    uint16_t size;
    uint8_t lbd;
    uint8_t producerId;
    int epoch;
    ProducedClauseCandidate() {}
    ProducedClauseCandidate(int* begin, int size, int lbd, int producerId, int epoch) : 
        begin(begin), size(size), lbd(lbd), producerId(producerId), epoch(epoch) {

        this->begin = (int*) malloc(size * sizeof(int));
        memcpy(this->begin, begin, size * sizeof(int));
    }
    ProducedClauseCandidate(ProducedClauseCandidate&& moved) {
        begin = moved.begin;
        moved.begin = nullptr;
        size = moved.size;
        lbd = moved.lbd;
        producerId = moved.producerId;
        epoch = moved.epoch;
    }
    int* releaseData() {
        int* data = begin;
        begin = nullptr;
        return data;
    }
    ~ProducedClauseCandidate() {
        if (begin != nullptr) free(begin);
    }
};
