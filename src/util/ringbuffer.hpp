
#ifndef DOMSCHREI_MALLOB_RINGBUFFER_HPP
#define DOMSCHREI_MALLOB_RINGBUFFER_HPP

#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <assert.h>

#include "util/ringbuf/ringbuf.h"

class RingBuffer {

private:
    uint8_t* _data;
    ringbuf_t* _ringbuf;
    ringbuf_worker_t* _producer;
    size_t _capacity;

public:
    RingBuffer(size_t size) : _capacity(size) {
        size_t ringbufSize;
        ringbuf_get_sizes(/*nworkers=*/1, &ringbufSize, nullptr);
        _ringbuf = (ringbuf_t*)malloc(ringbufSize);
        ringbuf_setup(_ringbuf, /*nworkers=*/1, size);
        _producer = ringbuf_register(_ringbuf, /*worker_id=*/0);
        _data = (uint8_t*)malloc(sizeof(int)*size);
    }

    bool produce(const int* data, size_t size, bool addSeparationZero) {
        
        ssize_t offset = ringbuf_acquire(_ringbuf, _producer, size + (addSeparationZero ? 1 : 0));
        if (offset == -1) return false;
        
        memcpy(_data+sizeof(int)*offset, data, sizeof(int)*size);
        if (addSeparationZero) {
            int zeroToAppend = 0;
            memcpy(_data+sizeof(int)*(offset+size), &zeroToAppend, sizeof(int));
        }
        
        ringbuf_produce(_ringbuf, _producer);
        
        return true;
    }

    bool consume(std::vector<int>& elem) {

        size_t offset;
        size_t len = ringbuf_consume(_ringbuf, &offset);
        elem.resize(len);
        if (len == 0) return false;
        
        memcpy(elem.data(), _data+sizeof(int)*offset, len*sizeof(int));

        ringbuf_release(_ringbuf, len);
        
        return true;
    }

    size_t getCapacity() const {
        return _capacity;
    }

    ~RingBuffer() {
        free(_ringbuf);
        free(_data);
    }
};

#endif