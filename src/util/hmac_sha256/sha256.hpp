
#pragma once

#include "sha256.h"

#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>
#include <string>

class Sha256Builder {

private:
    Sha256Context ctx;
    unsigned long _nb_bytes {0};

public:
    Sha256Builder() {
        Sha256Initialise(&ctx);
    }

    Sha256Builder& update(const uint8_t* data, int nbBytes) {
        Sha256Update(&ctx, data, nbBytes);
        _nb_bytes += nbBytes;
        return *this;
    }

    std::vector<uint8_t> get() {
        SHA256_HASH hash;
        Sha256Finalise(&ctx, &hash);
        std::vector<uint8_t> res(SHA256_HASH_SIZE);
        memcpy(res.data(), hash.bytes, SHA256_HASH_SIZE);
        return res;
    }

    std::string getAsHexStr() {
        SHA256_HASH hash;
        Sha256Finalise(&ctx, &hash);
        std::stringstream stream;
        for (int i = 0; i < SHA256_HASH_SIZE; i++) {
            stream << std::hex << std::setfill('0') << std::setw(2) << (int) hash.bytes[i];
        }
        return stream.str();
    }
};
