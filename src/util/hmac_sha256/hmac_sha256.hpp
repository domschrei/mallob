
#pragma once

#include "hmac_sha256.h"

#include <sys/types.h>
#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cassert>

class HMAC {

public:
    static int sign_data_128bit(const uint8_t* data, size_t nbBytes, unsigned long key, uint8_t* out) {
        hmac_sha256(&key, sizeof(key), data, nbBytes, out, 16); // truncates
        return 16;
    }

    static int sign_data_256bit(const uint8_t* data, size_t nbBytes, unsigned long key, uint8_t* out) {
        hmac_sha256(&key, sizeof(key), data, nbBytes, out, 32);
        return 32;
    }
};
