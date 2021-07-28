
#ifndef DOMSCHREI_MALLOB_RINGBUFFER_HPP
#define DOMSCHREI_MALLOB_RINGBUFFER_HPP

#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <assert.h>
#include <list>
#include <atomic>

#include "util/logger.hpp"
#include "app/sat/hordesat/utilities/clause.hpp"
using namespace Mallob;

#include "util/ringbuf/ringbuf.h"

template <typename T>
class SPSCRingBuffer {

private:
    std::vector<T> _data;
    ringbuf_t* _ringbuf = nullptr;
    ringbuf_worker_t* _producer;
    size_t _capacity = 0;
    std::atomic_int _size = 0;

    std::list<T> _consumer_buffer;

public:
    SPSCRingBuffer() {}
    SPSCRingBuffer(size_t capacity) : _data(capacity), _capacity(capacity) {
        size_t ringbufSize;
        ringbuf_get_sizes(/*numProducers=*/1, &ringbufSize, nullptr);
        _ringbuf = (ringbuf_t*)malloc(ringbufSize);
        ringbuf_setup(_ringbuf, /*nworkers=*/1, _capacity);
        _producer = ringbuf_register(_ringbuf, /*worker_id=*/0);
    }

    bool produce(T&& element) {
        ssize_t offset = ringbuf_acquire(_ringbuf, _producer, /*size=*/1);
        if (offset == -1) return false;
        _data[offset] = std::move(element);
        ringbuf_produce(_ringbuf, _producer);
        _size++;
        return true;
    }

    std::optional<T> consume() {
        std::optional<T> out;
        if (_consumer_buffer.empty()) {
            size_t offset;
            size_t len = ringbuf_consume(_ringbuf, &offset);
            if (len == 1) {
                out = std::move(_data[offset]);
                ringbuf_release(_ringbuf, len);
                _size--;
                return out;
            } else {
                for (size_t i = 0; i < len; i++) {
                    _consumer_buffer.push_back(std::move(_data[offset+i]));
                }
                ringbuf_release(_ringbuf, len);
            }
        }
        if (_consumer_buffer.empty()) return out;

        out = std::move(_consumer_buffer.front());
        _consumer_buffer.pop_front();
        _size--;
        return out;
    }

    size_t getCapacity() const {
        return _capacity;
    }

    size_t getSize() const {
        return _size;
    }

    bool empty() const {
        return _size == 0;
    }

    ~SPSCRingBuffer() {
        if (!isNull()) {
            free(_ringbuf);
        }
    }

    bool isNull() const {return _ringbuf == nullptr;}
};

class RingBuffer {

private:
    uint8_t* _data = nullptr;
    ringbuf_t* _ringbuf = nullptr;
    std::vector<ringbuf_worker_t*> _producers;
    size_t _capacity = 0;

    std::vector<int> _consumed_buffer;

public:
    RingBuffer() {}
    RingBuffer(size_t size, int numProducers = 1) : _capacity(size) {
        size_t ringbufSize;
        ringbuf_get_sizes(/*nworkers=*/numProducers, &ringbufSize, nullptr);
        _ringbuf = (ringbuf_t*)malloc(ringbufSize);
        ringbuf_setup(_ringbuf, /*nworkers=*/numProducers, size);
        for (int i = 0; i < numProducers; i++) {
            _producers.push_back(ringbuf_register(_ringbuf, /*worker_id=*/i));
        }
        _data = (uint8_t*)malloc(sizeof(int)*size);
    }

    bool produce(const int* data, size_t size, int prefixOrZero, bool appendZero, int producerId = 0) {
        
        bool addPrefix = prefixOrZero != 0;
        ssize_t offset = ringbuf_acquire(_ringbuf, _producers[producerId], size + (addPrefix?1:0) + (appendZero?1:0));
        if (offset == -1) return false;
        
        // Copy the prefix value
        if (addPrefix) {
            memcpy(_data+sizeof(int)*offset, &prefixOrZero, sizeof(int));
            offset++;
        }
        // Copy the clause itself
        memcpy(_data+sizeof(int)*offset, data, sizeof(int)*size);
        offset += size;
        // Insert a separation zero
        if (appendZero) {
            int zeroToAppend = 0;
            memcpy(_data+sizeof(int)*offset, &zeroToAppend, sizeof(int));
        }
        
        ringbuf_produce(_ringbuf, _producers[producerId]);
        return true;
    }

    bool consume(int sizeOrZero, std::vector<int>& elem) {

        if (_consumed_buffer.empty()) {
            // Fetch batch of clauses from ringbuffer
            size_t offset;
            size_t len = ringbuf_consume(_ringbuf, &offset);
            _consumed_buffer.resize(len);
            memcpy(_consumed_buffer.data(), 
                    _data+sizeof(int)*offset, 
                    len*sizeof(int));
            ringbuf_release(_ringbuf, len);
        }
        if (_consumed_buffer.empty()) return false;

        // Extract a clause from the consumed buffer
        if (sizeOrZero > 0) {
            size_t start = _consumed_buffer.size()-sizeOrZero;
            elem.insert(elem.end(), _consumed_buffer.begin()+start, _consumed_buffer.end());
            _consumed_buffer.resize(_consumed_buffer.size()-sizeOrZero);
        } else {
            // Read from the back until you hit a separation zero
            size_t start = _consumed_buffer.size()-1;
            while (start > 0 && _consumed_buffer[start-1] != 0) start--;
            assert(_consumed_buffer[start] != 0);
            for (size_t i = start; i < _consumed_buffer.size(); i++)
                elem.push_back(_consumed_buffer[i]);
            _consumed_buffer.resize(start);
        }
        
        return true;
    }

    size_t getCapacity() const {
        return _capacity;
    }

    ~RingBuffer() {
        if (!isNull()) {
            free(_ringbuf);
            free(_data);
        }
    }

    bool isNull() const {return _ringbuf == nullptr;}
};

class UniformSizeClauseRingBuffer {

protected:
    RingBuffer _ringbuf;
    int _clause_size;

public:
    UniformSizeClauseRingBuffer() {}
    UniformSizeClauseRingBuffer(size_t ringbufSize, int clauseSize, int numProducers = 1) : _ringbuf(ringbufSize, numProducers), _clause_size(clauseSize) {}
    
    bool isNull() {return _ringbuf.isNull();}
    virtual bool insertClause(const Clause& c, int producerId = 0) {
        assert(c.size == _clause_size);
        return _ringbuf.produce(c.begin, _clause_size, c.lbd, false, producerId);
    }
    virtual bool getClause(std::vector<int>& out) {
        return _ringbuf.consume(_clause_size+1, out);
    }
};

class UniformClauseRingBuffer : public UniformSizeClauseRingBuffer {

public:
    UniformClauseRingBuffer(size_t ringbufSize, int clauseSize, int numProducers = 1) : 
        UniformSizeClauseRingBuffer(ringbufSize, clauseSize, numProducers) {}
    bool insertClause(const Clause& c, int producerId = 0) override {
        assert(c.size == _clause_size);
        return _ringbuf.produce(c.begin, _clause_size, 0, false, producerId);
    }
    bool getClause(std::vector<int>& out) override {
        return _ringbuf.consume(_clause_size, out);
    }
};

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

#endif