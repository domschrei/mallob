
#pragma once

#include <cmath>
#include <cstdint>
#include <type_traits>

#include "util/assert.hpp"

template <typename T>
int toVariableBytelength(T val, uint8_t* out) {
    constexpr int maxLength = (sizeof(T)*8) / 7 + 1;

    T remainder = val;
    if (std::is_signed_v<T>) {
        remainder = 2*std::abs(remainder) + (remainder<0);
    }
    int offset = 0;

    while (remainder != 0) {

        uint8_t nextByte = remainder & 0b01111111;
        remainder = remainder >> 7;
        nextByte |= (0b10000000 * (remainder != 0)); // continuation bit

        assert(offset < maxLength);
        out[offset++] = nextByte;
    }
    return offset;
}

template <typename T>
T fromVariableBytelength(const uint8_t* in, int& outLength) {
    constexpr int maxLength = (sizeof(T)*8) / 7 + 1;

    T out = 0;
    int offset = 0;

    bool continuing = true;
    while (continuing) {

        assert(offset < maxLength);
        uint8_t nextByte = in[offset++];

        continuing = nextByte & 0b10000000; // continuation bit
        out |= ((T) (nextByte & 0b01111111)) << 7*(offset-1);
    }
    outLength = offset;

    if (std::is_signed_v<T>) {
        bool negative = out % 2 == 1;
        out = (negative ? -1 : 1) * (out/2);
    }
    return out;
}
