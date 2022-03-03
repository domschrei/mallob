
#ifndef DOMSCHREI_MALLOB_RINGBUFFER_HPP
#define DOMSCHREI_MALLOB_RINGBUFFER_HPP

#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "util/assert.hpp"
#include <list>
#include <atomic>

#include "util/sys/threading.hpp"
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







class RingBufferV2 {

public:
    enum OperationMode {UNIFORM_SIZE, INDIVIDUAL_SIZES};

private:
    OperationMode _mode = INDIVIDUAL_SIZES;
    uint8_t* _data = nullptr;
    ringbuf_t* _ringbuf = nullptr;
    std::vector<ringbuf_worker_t*> _producers;
    size_t _capacity_bytes = 0;

    size_t _bytes_per_elem; // UNIFORM_SIZE

    std::vector<uint8_t> _consumed_buffer;
    int _num_consumed_bytes = 0;
    Mutex _consume_mutex;

public:
    RingBufferV2() {}
    RingBufferV2(size_t size, int sizePerElem, int numProducers = 1, uint8_t* data = nullptr) :
            _mode(UNIFORM_SIZE), _data(data), _capacity_bytes(sizeof(int)*size), 
            _bytes_per_elem(sizeof(int)*sizePerElem) {
        init(numProducers);
    }
    RingBufferV2(size_t size, int numProducers = 1, uint8_t* data = nullptr) : 
            _mode(INDIVIDUAL_SIZES), _data(data), _capacity_bytes(sizeof(int)*size) {
        init(numProducers);
    }
    RingBufferV2(RingBufferV2&& moved) : _mode(moved._mode), _data(moved._data), _ringbuf(moved._ringbuf), 
        _producers(std::move(moved._producers)), _capacity_bytes(moved._capacity_bytes), 
        _bytes_per_elem(moved._bytes_per_elem), _consumed_buffer(std::move(moved._consumed_buffer)), 
        _num_consumed_bytes(moved._num_consumed_bytes) {
        
        moved._data = nullptr;
        moved._ringbuf = nullptr;
    }

    // Insert a data element consisting of an array of integers.
    // If the operation mode is UNIFORM_SIZE, only the data pointer and producer ID are to be provided.
    // Otherwise, numIntegers must indicate the size of the provided data, and headerByte
    // can be any kind of data from which the user is able to decode back the element's size.
    bool produce(const int* data, int producerId, int numIntegers = -1, uint8_t headerByte = 0) {
        
        if (numIntegers == -1) {
            assert(_mode == OperationMode::UNIFORM_SIZE);
            numIntegers = _bytes_per_elem/sizeof(int);
        } else {
            assert(_mode == OperationMode::INDIVIDUAL_SIZES);
            assert(0 <= numIntegers && numIntegers <= 255);
        }
        assert(producerId >= 0 || LOG_RETURN_FALSE("producer ID %i\n", producerId));
        assert(producerId < _producers.size() || LOG_RETURN_FALSE("producer ID %i\n", producerId));
        int numBytes = sizeof(int)*numIntegers;

        int requestedNumBytes = numBytes + (_mode == INDIVIDUAL_SIZES ? 1 : 0);
        ssize_t offset = ringbuf_acquire(_ringbuf, _producers[producerId], requestedNumBytes);
        if (offset == -1) return false;
        
        if (_mode == INDIVIDUAL_SIZES) {
            _data[offset] = headerByte;
            offset++;
        }
        memcpy(_data+offset, data, numBytes);
        ringbuf_produce(_ringbuf, _producers[producerId]);
        return true;
    }

    // Only for INDIVIDUAL_SIZES: Get the header byte of the next element to be consumed.
    // The user must be able to decode the size of the next element from it.
    // If no elements are ready for consumption, returns false.
    bool getNextHeaderByte(uint8_t& peekedNumber) {
        assert(_mode == OperationMode::INDIVIDUAL_SIZES);
        refillConsumeBuffer();
        if (_num_consumed_bytes == 0) return false;
        
        int start = _consumed_buffer.size() - _num_consumed_bytes;
        peekedNumber = _consumed_buffer[start];
        return true;
    }

    // Only for INDIVIDUAL_SIZES: Consume the next element. The size of this element
    // has to be provided via numIntegers (the user must be able to decode this size 
    // from the result of getNextHeaderByte). The provided vector will append an integer
    // containing the header byte followed by numIntegers integers with the actual payload.
    bool consume(int numIntegers, std::vector<int>& out) {
        refillConsumeBuffer();
        if (_num_consumed_bytes == 0) return false;

        int start = _consumed_buffer.size() - _num_consumed_bytes;
        assert(start >= 0);
        auto oldSize = out.size();
        int indicatorIndivSizes = (_mode == INDIVIDUAL_SIZES ? 1:0);
        out.resize(out.size()+indicatorIndivSizes+numIntegers);
        if (_mode == INDIVIDUAL_SIZES) out[oldSize] = (int) _consumed_buffer[start];
        memcpy(((uint8_t*)out.data())+(oldSize+indicatorIndivSizes)*sizeof(int), 
            _consumed_buffer.data()+start+indicatorIndivSizes, 
            numIntegers*sizeof(int));
        _num_consumed_bytes -= indicatorIndivSizes+numIntegers*sizeof(int); // "resize" consume buffer
        return true;
    }

    // Only for UNIFORM_SIZE: Consume the next element and append it to the provided vector.
    bool consume(std::vector<int>& out) {
        assert(_mode == UNIFORM_SIZE);
        return consume(_bytes_per_elem/sizeof(int), out);
    }

    uint8_t* releaseBuffer() {
        if (isNull()) return nullptr;
        auto out = _data;
        free(_ringbuf);
        _ringbuf = nullptr;
        return out;
    }

    size_t flushBuffer(uint8_t* out) {

        auto lock = _consume_mutex.getLock();

        // Flush ring buffer into provided swap buffer
        size_t offset;
        size_t consumedSize = ringbuf_consume(_ringbuf, &offset);
        size_t writePosition = 0;
        while (consumedSize > 0) {
            memcpy((void*)(otherBuffer+writePosition), 
                    (void*)(_data+offset), 
                    consumedSize);
            writePosition += consumedSize;
            ringbuf_release(_ringbuf, consumedSize);
            consumedSize = ringbuf_consume(_ringbuf, &offset);
        }
        // Return end of written data
        return writePosition;
    }

    size_t getCapacity() const {
        return _capacity_bytes/sizeof(int);
    }

    ~RingBufferV2() {
        if (!isNull()) {
            free(_ringbuf);
            free(_data);
        }
    }

    bool isNull() const {return _ringbuf == nullptr;}

private:
    void init(int numProducers) {
        size_t ringbufSize;
        ringbuf_get_sizes(/*nworkers=*/numProducers, &ringbufSize, nullptr);
        _ringbuf = (ringbuf_t*)malloc(ringbufSize);
        ringbuf_setup(_ringbuf, /*nworkers=*/numProducers, _capacity_bytes);
        for (int i = 0; i < numProducers; i++) {
            _producers.push_back(ringbuf_register(_ringbuf, /*worker_id=*/i));
        }
        if (_data == nullptr) _data = (uint8_t*)malloc(_capacity_bytes);
    }
    void refillConsumeBuffer() {
        if (_num_consumed_bytes > 0) return;
        // Fetch batch of clauses from ringbuffer
        if (!_consume_mutex.tryLock()) return;
        size_t offset;
        _num_consumed_bytes = ringbuf_consume(_ringbuf, &offset);
        if (_num_consumed_bytes > 0) {
            _consumed_buffer.resize(_num_consumed_bytes);
            memcpy(_consumed_buffer.data(), 
                    _data+offset, 
                    _num_consumed_bytes);
            ringbuf_release(_ringbuf, _num_consumed_bytes);   
        }
        _consume_mutex.unlock();
    }
};










class RingBuffer {

private:
    uint8_t* _data = nullptr;
    ringbuf_t* _ringbuf = nullptr;
    std::vector<ringbuf_worker_t*> _producers;
    size_t _capacity = 0;

    std::vector<int> _consumed_buffer;
    int _consumed_size = 0;
    Mutex _consume_mutex;

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
    RingBuffer(RingBuffer&& moved) : _data(moved._data), _ringbuf(moved._ringbuf), 
        _producers(std::move(moved._producers)), _capacity(moved._capacity), 
        _consumed_buffer(std::move(moved._consumed_buffer)), _consumed_size(moved._consumed_size) {
        
        moved._data = nullptr;
        moved._ringbuf = nullptr;
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
            if (!_consume_mutex.tryLock()) return false;
            size_t offset;
            _consumed_size = ringbuf_consume(_ringbuf, &offset);
            if (_consumed_size > 0) {
                if (_consumed_buffer.size() < _consumed_size) 
                    _consumed_buffer.resize(_consumed_size);
                memcpy(_consumed_buffer.data(), 
                        _data+sizeof(int)*offset, 
                        _consumed_size*sizeof(int));
                ringbuf_release(_ringbuf, _consumed_size);   
            }
            _consume_mutex.unlock();
            if (_consumed_size <= 0) return false; 
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

    size_t flushBuffer(uint8_t* otherBuffer) {

        auto lock = _consume_mutex.getLock();

        // Flush ring buffer into provided swap buffer
        size_t offset;
        size_t consumedSize = ringbuf_consume(_ringbuf, &offset);
        size_t writePosition = 0;
        while (consumedSize > 0) {
            memcpy((void*)(otherBuffer+sizeof(int)*writePosition), 
                    (void*)(_data+sizeof(int)*offset), 
                    consumedSize*sizeof(int));
            writePosition += consumedSize;
            ringbuf_release(_ringbuf, consumedSize);
            consumedSize = ringbuf_consume(_ringbuf, &offset);
        }
        // Return end of written data
        return writePosition;
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

#endif