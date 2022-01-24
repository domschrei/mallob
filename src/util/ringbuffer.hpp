
#ifndef DOMSCHREI_MALLOB_RINGBUFFER_HPP
#define DOMSCHREI_MALLOB_RINGBUFFER_HPP

#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "util/assert.hpp"
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
    int _consumed_size = 0;

public:
    RingBuffer() {}
    RingBuffer(size_t size, int numProducers = 1, uint8_t* data = nullptr) : _data(data), _capacity(size) {
        size_t ringbufSize;
        ringbuf_get_sizes(/*nworkers=*/numProducers, &ringbufSize, nullptr);
        _ringbuf = (ringbuf_t*)malloc(ringbufSize);
        ringbuf_setup(_ringbuf, /*nworkers=*/numProducers, size);
        for (int i = 0; i < numProducers; i++) {
            _producers.push_back(ringbuf_register(_ringbuf, /*worker_id=*/i));
        }
        if (_data == nullptr) _data = (uint8_t*)malloc(sizeof(int)*size);
    }

    bool produce(const int* data, size_t size, int prefixOrZero, bool appendZero, int producerId = 0) {
        
        bool addPrefix = prefixOrZero != 0;
        assert(producerId >= 0 || LOG_RETURN_FALSE("producer ID %i\n", producerId));
        assert(producerId < _producers.size() || LOG_RETURN_FALSE("producer ID %i\n", producerId));

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

        if (_consumed_size == 0) {
            // Fetch batch of clauses from ringbuffer
            size_t offset;
            _consumed_size = ringbuf_consume(_ringbuf, &offset);
            if (_consumed_size <= 0) return false;
            if (_consumed_buffer.size() < _consumed_size) 
                _consumed_buffer.resize(_consumed_size);
            memcpy(_consumed_buffer.data(), 
                    _data+sizeof(int)*offset, 
                    _consumed_size*sizeof(int));
            ringbuf_release(_ringbuf, _consumed_size);
        }
        
        // Extract a clause from the consumed buffer
        if (sizeOrZero > 0) {
            int start = _consumed_size-sizeOrZero;
            assert(start >= 0);
            elem.resize(elem.size()+sizeOrZero);
            memcpy(elem.data()+elem.size()-sizeOrZero, _consumed_buffer.data()+start, sizeOrZero*sizeof(int));
            //elem.insert(elem.end(), _consumed_buffer.begin()+start, _consumed_buffer.begin()+start+sizeOrZero);
            _consumed_size -= sizeOrZero; // "resize" consume buffer
        } else {
            // Read from the back until you hit a separation zero
            int start = _consumed_size-1;
            while (start > 0 && _consumed_buffer[start-1] != 0) start--;
            assert(_consumed_buffer[start] != 0);
            for (size_t i = start; i < _consumed_size; i++)
                elem.push_back(_consumed_buffer[i]);
            _consumed_size = start; // "resize" consume buffer
        }
        
        return true;
    }

    uint8_t* releaseBuffer() {
        if (isNull()) return nullptr;
        auto out = _data;
        free(_ringbuf);
        _ringbuf = nullptr;
        return out;
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
    UniformSizeClauseRingBuffer(size_t ringbufSize, int clauseSize, int numProducers = 1, uint8_t* data = nullptr) : 
        _ringbuf(ringbufSize, numProducers, data), _clause_size(clauseSize) {}
    
    bool isNull() {return _ringbuf.isNull();}
    virtual bool insertClause(const Clause& c, int producerId = 0) {
        assert(c.size == _clause_size);
        return _ringbuf.produce(c.begin, _clause_size, c.lbd, false, producerId);
    }
    virtual bool getClause(std::vector<int>& out) {
        return _ringbuf.consume(_clause_size+1, out);
    }
    uint8_t* releaseBuffer() {
        return _ringbuf.releaseBuffer();
    }
};

class UniformClauseRingBuffer : public UniformSizeClauseRingBuffer {

public:
    UniformClauseRingBuffer(size_t ringbufSize, int clauseSize, int numProducers = 1, uint8_t* data = nullptr) : 
        UniformSizeClauseRingBuffer(ringbufSize, clauseSize, numProducers, data) {}
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