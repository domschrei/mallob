
#ifndef DOMSCHREI_MALLOB_RINGBUFFER_HPP
#define DOMSCHREI_MALLOB_RINGBUFFER_HPP

#include <vector>

template <typename T>
class RingBuffer {

private:
    std::vector<T> _buffer;
    size_t _prod_idx;
    size_t _cons_idx;
    size_t _num_available;

public:
    RingBuffer(int size) : _buffer(size) {
        _prod_idx = 0;
        _cons_idx = 0;
        _num_available = 0;
    }

    bool produce(const T& elem) {
        if (_num_available == _buffer.size()) return false;
        _buffer[_prod_idx] = elem;
        _prod_idx = (_prod_idx+1) % _buffer.size();
        _num_available++;
        return true;
    }

    bool produce(T&& elem) {
        if (_num_available == _buffer.size()) return false;
        _buffer[_prod_idx] = std::move(elem);
        _prod_idx = (_prod_idx+1) % _buffer.size();
        _num_available++;
        return true;
    }

    bool consume(T& elem) {
        if (_num_available == 0) return false;
        elem = _buffer[_cons_idx];
        _cons_idx = (_cons_idx+1) % _buffer.size();
        _num_available--;
        return true;
    }

    bool empty() {
        return _num_available == 0;
    }
    size_t size() {
        return _num_available;
    }
    size_t capacity() {
        return _buffer.size();
    }
};

#endif