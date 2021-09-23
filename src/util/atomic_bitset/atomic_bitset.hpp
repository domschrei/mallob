/*
 * Copyright 2019 Erik Garrison
 * Copyright 2019 Facebook, Inc. and its affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * 
 * ----
 * 
 * 
 * Taken from github.com/ekg/atomicbitvector
 * Modified by D. Schreiber
 */

#pragma once

//#include <array>
#include <vector>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>

#include "atom_wrapper.hpp"

namespace atomicbitvector {

/**
 * An atomic bitvector with size fixed at construction time
 */
class atomic_bv_t {
public:
    /**
     * Construct a atomic_bv_t; all bits are initially false.
     */
    atomic_bv_t(size_t N) : _size(N),
                            kNumBlocks((N + kBitsPerBlock - 1) / kBitsPerBlock) {
        data_.resize(kNumBlocks);
    }

    /**
     * Set bit idx to true, using the given memory order. Returns the
     * previous value of the bit.
     *
     * Note that the operation is a read-modify-write operation due to the use
     * of fetch_or.
     */
    bool set(size_t idx, std::memory_order order = std::memory_order_relaxed);

    /**
     * Set bit idx to false, using the given memory order. Returns the
     * previous value of the bit.
     *
     * Note that the operation is a read-modify-write operation due to the use
     * of fetch_and.
     */
    bool reset(size_t idx, std::memory_order order = std::memory_order_relaxed);

    /**
     * Set bit idx to the given value, using the given memory order. Returns
     * the previous value of the bit.
     *
     * Note that the operation is a read-modify-write operation due to the use
     * of fetch_and or fetch_or.
     *
     * Yes, this is an overload of set(), to keep as close to std::bitset's
     * interface as possible.
     */
    bool set(
        size_t idx,
        bool value,
        std::memory_order order = std::memory_order_relaxed);

    /**
     * Read bit idx.
     */
    bool test(size_t idx, std::memory_order order = std::memory_order_relaxed)
        const;

    /**
     * Same as test() with the default memory order.
     */
    bool operator[](size_t idx) const;

    /**
     * Return the size of the bitset.
     */
    constexpr size_t size() const {
        return _size;
    }

    void reset();

private:
    // Pick the largest lock-free type available
#if (ATOMIC_LLONG_LOCK_FREE == 2)
    typedef unsigned long long BlockType;
#elif (ATOMIC_LONG_LOCK_FREE == 2)
    typedef unsigned long BlockType;
#else
    // Even if not lock free, what can we do?
    typedef unsigned int BlockType;
#endif
    typedef atomwrapper<BlockType> AtomicBlockType;

    static constexpr size_t kBitsPerBlock =
        std::numeric_limits<BlockType>::digits;

    static constexpr size_t blockIndex(size_t bit) {
        return bit / kBitsPerBlock;
    }

    static constexpr size_t bitOffset(size_t bit) {
        return bit % kBitsPerBlock;
    }

    // avoid casts
    static constexpr BlockType kOne = 1;
    size_t _size; // dynamically set
    size_t kNumBlocks; // dynamically set
    std::vector<AtomicBlockType> data_; // filled at instantiation time
};

inline bool atomic_bv_t::set(size_t idx, std::memory_order order) {
    assert(idx < _size);
    BlockType mask = kOne << bitOffset(idx);
    return data_[blockIndex(idx)].ref().fetch_or(mask, order) & mask;
}

inline bool atomic_bv_t::reset(size_t idx, std::memory_order order) {
    assert(idx < _size);
    BlockType mask = kOne << bitOffset(idx);
    return data_[blockIndex(idx)].ref().fetch_and(~mask, order) & mask;
}

inline bool atomic_bv_t::set(size_t idx, bool value, std::memory_order order) {
    return value ? set(idx, order) : reset(idx, order);
}

inline bool atomic_bv_t::test(size_t idx, std::memory_order order) const {
    assert(idx < _size);
    BlockType mask = kOne << bitOffset(idx);
    return data_[blockIndex(idx)].const_ref().load(order) & mask;
}

inline bool atomic_bv_t::operator[](size_t idx) const {
    return test(idx);
}

inline void atomic_bv_t::reset() {
    for (size_t i = 0; i < data_.size(); i++) {
        data_[i].ref().store(0, std::memory_order_relaxed);
    }
}

}

typedef atomicbitvector::atomic_bv_t AtomicBitset;
