
#pragma once

#include "hmac_sha256.h"

#include <string>

class HMAC {

public:
    static int sign_data_128bit(const uint8_t* data, size_t nbBytes, const uint8_t* key, int keySize, uint8_t* out) {
        hmac_sha256(key, keySize, data, nbBytes, out, 16); // truncates
        return 16;
    }

    static int sign_data_256bit(const uint8_t* data, size_t nbBytes, const uint8_t* key, int keySize, uint8_t* out) {
        hmac_sha256(key, keySize, data, nbBytes, out, 32);
        return 32;
    }
};
