
#pragma once

#include "util/ringbuffer.hpp"
#include "clause.hpp"
using namespace Mallob;


class ClauseRingBuffer {

protected:
    RingBuffer _ringbuf;

public:
    ClauseRingBuffer() {}
    ClauseRingBuffer(size_t ringbufSize, int numProducers = 1, uint8_t* data = nullptr) : _ringbuf(ringbufSize, numProducers, data) {}

    bool isNull() {return _ringbuf.isNull();}
    uint8_t* releaseBuffer() {return _ringbuf.releaseBuffer();}
    size_t flushBufferUnsafe(uint8_t* otherBuffer) {return _ringbuf.flushBuffer(otherBuffer);}

    virtual bool insertClause(const Clause& c, int producerId) = 0;
    virtual bool getClause(std::vector<int>& out) = 0;
};

class UniformSizeClauseRingBuffer : public ClauseRingBuffer {

protected:
    int _clause_size;

public:
    UniformSizeClauseRingBuffer() {}
    UniformSizeClauseRingBuffer(size_t ringbufSize, int clauseSize, int numProducers = 1, uint8_t* data = nullptr) : 
        ClauseRingBuffer(ringbufSize, numProducers, data), _clause_size(clauseSize) {}
    
    virtual bool insertClause(const Clause& c, int producerId = 0) override {
        assert(c.size == _clause_size);
        return _ringbuf.produce(c.begin, _clause_size, c.lbd, false, producerId);
    }
    virtual bool getClause(std::vector<int>& out) override {
        return _ringbuf.consume(_clause_size+1, out);
    }
};

/*
class ComplementarySizeLbdClauseRingBuffer : public ClauseRingBuffer {

protected:
    int _sum_of_size_and_lbd;

public:
    ComplementarySizeLbdClauseRingBuffer() {}
    ComplementarySizeLbdClauseRingBuffer(size_t ringbufSize, int sumOfSizeAndLbd, int numProducers = 1, uint8_t* data = nullptr) : 
        ClauseRingBuffer(ringbufSize, numProducers, data), _sum_of_size_and_lbd(sumOfSizeAndLbd) {}
    
    virtual bool insertClause(const Clause& c, int producerId = 0) override {
        assert(c.size == _clause_size);
        return _ringbuf.produce(c.begin, _clause_size, c.lbd, false, producerId);
    }
    virtual bool getClause(std::vector<int>& out) override {
        return _ringbuf.consume(_clause_size+1, out);
    }

};
*/

class UniformClauseRingBuffer : public UniformSizeClauseRingBuffer {

public:
    UniformClauseRingBuffer(size_t ringbufSize, int clauseSize, int numProducers = 1, uint8_t* data = nullptr) : 
        UniformSizeClauseRingBuffer(ringbufSize, clauseSize, numProducers, data) {}
    UniformClauseRingBuffer(UniformClauseRingBuffer&& moved) : UniformSizeClauseRingBuffer(std::move(moved)) {}

    bool insertClause(const Clause& c, int producerId = 0) override {
        assert(c.size == _clause_size);
        return _ringbuf.produce(c.begin, _clause_size, 0, false, producerId);
    }
    bool getClause(std::vector<int>& out) override {
        size_t sizeBefore = out.size();
        bool success = _ringbuf.consume(_clause_size, out);
        if (success) assert(out.size() == sizeBefore+_clause_size);
        return success;
    }
};

/*
class MixedNonunitClauseRingBuffer {

private:
    RingBuffer _ringbuf;

public:
    MixedNonunitClauseRingBuffer(size_t ringbufSize, int numProducers = 1) : _ringbuf(ringbufSize, numProducers) {}
    
    bool insertClause(const Clause& c, int producerId = 0) {
        return _ringbuf.produce(c.begin, c.size, c.lbd, true, producerId);
    }
    bool insertClause(const int* begin, int sizeWithLbd, int producerId = 0) {
        return _ringbuf.produce(begin, sizeWithLbd, 0, true, producerId);
    }
    bool getClause(std::vector<int>& out) {
        return _ringbuf.consume(0, out);
    }
};
*/

class UnitClauseRingBuffer {

private:
    RingBuffer _ringbuf;

public:
    UnitClauseRingBuffer(size_t ringbufSize, int numProducers = 1) : _ringbuf(ringbufSize, numProducers) {}

    bool insertUnit(int lit, int producerId = 0) {
        return _ringbuf.produce(&lit, 1, 0, false, producerId);
    }
    bool getUnits(std::vector<int>& out) {
        return _ringbuf.consume(0, out);
    }
};
