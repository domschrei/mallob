
#pragma once

#include <cstdio> // printf (error printing)
#include <stdlib.h> // malloc, free, abort

class SipHash {

    //static void sign_data_128bit(const u8* data, int nbBytes, const u8* key128Bit, u8* out128Bit) {
    //    siphash(data, nbBytes, key128Bit, out128Bit, 128 / 8);
    //}
    //static void sign_data_64bit(const u8* data, int nbBytes, const u8* key64Bit, u8* out64Bit) {
    //    halfsiphash(data, nbBytes, key64Bit, out64Bit, 64 / 8);
    //}

#define cROUNDS 2
#define dROUNDS 4

typedef unsigned long size_t;
typedef unsigned long u64;
typedef unsigned int uint32_t;
typedef unsigned char u8;
#define SH_UINT64_C(c) c##UL

#define ROTL(x, b) (u64)(((x) << (b)) | ((x) >> (64 - (b))))

#define U32TO8_LE(p, v)                                                        \
    (p)[0] = (u8)((v));                                                   \
    (p)[1] = (u8)((v) >> 8);                                              \
    (p)[2] = (u8)((v) >> 16);                                             \
    (p)[3] = (u8)((v) >> 24);

#define U64TO8_LE(p, v)                                                        \
    U32TO8_LE((p), (uint32_t)((v)));                                           \
    U32TO8_LE((p) + 4, (uint32_t)((v) >> 32));

#define U8TO64_LE(p)                                                           \
    (((u64)((p)[0])) | ((u64)((p)[1]) << 8) |                        \
     ((u64)((p)[2]) << 16) | ((u64)((p)[3]) << 24) |                 \
     ((u64)((p)[4]) << 32) | ((u64)((p)[5]) << 40) |                 \
     ((u64)((p)[6]) << 48) | ((u64)((p)[7]) << 56))

#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1;                                                              \
        v1 = ROTL(v1, 13);                                                     \
        v1 ^= v0;                                                              \
        v0 = ROTL(v0, 32);                                                     \
        v2 += v3;                                                              \
        v3 = ROTL(v3, 16);                                                     \
        v3 ^= v2;                                                              \
        v0 += v3;                                                              \
        v3 = ROTL(v3, 21);                                                     \
        v3 ^= v0;                                                              \
        v2 += v1;                                                              \
        v1 = ROTL(v1, 17);                                                     \
        v1 ^= v2;                                                              \
        v2 = ROTL(v2, 32);                                                     \
    } while (0)

private:
    const unsigned char* kk;
    u8* out;
    const int outlen {128 / 8};
    u64 v0;
    u64 v1;
    u64 v2;
    u64 v3;
    u64 k0;
    u64 k1;
    u64 m;
    int i;
    size_t inlen;

    u8* buf;
    unsigned char buflen {0};

public:
    SipHash(const unsigned char* key_128bit) : kk(key_128bit),
            out((u8*) malloc(128 / 8)) {
        buf = (u8*) malloc(8);
        if (kk) reset();
    }
    ~SipHash() {
        free(buf);
        free(out);
    }

    SipHash& reset() {
        v0 = SH_UINT64_C(0x736f6d6570736575);
        v1 = SH_UINT64_C(0x646f72616e646f6d);
        v2 = SH_UINT64_C(0x6c7967656e657261);
        v3 = SH_UINT64_C(0x7465646279746573);
        k0 = U8TO64_LE(kk);
        k1 = U8TO64_LE(kk + 8);
        v3 ^= k1;
        v2 ^= k0;
        v1 ^= k1;
        v0 ^= k0;
        inlen = 0;
        buflen = 0;
        if (outlen == 16)
            v1 ^= 0xee;
        return *this;
    }

    SipHash& update(const unsigned char* data, size_t nbBytes) {
        int datapos {0};
        while (true) {
            while (buflen < 8 && datapos < nbBytes) {
                buf[buflen++] = data[datapos++];
            }
            if (buflen < 8) {
                break;
            } else {
                processNextBlock();
                buflen = 0;
            }
        }
        inlen += nbBytes;
        return *this;
    }

    u8* digest() {
        processFinalBlock();
        return out;
    }

private:
    void processNextBlock() {
        m = U8TO64_LE(buf);
        v3 ^= m;
        for (i = 0; i < cROUNDS; ++i)
            SIPROUND;
        v0 ^= m;
    }

    void processFinalBlock() {
        const int left = inlen & 7;
        if (left != buflen) {
            printf("[ERROR] SipHash : wrong residual buffer length! (%i vs %i)\n", left, buflen);
            abort();
        }
        u64 b = ((u64)inlen) << 56;
        auto ni = buf;

        switch (left) {
        case 7:
            b |= ((u64)ni[6]) << 48;
            /* FALLTHRU */
        case 6:
            b |= ((u64)ni[5]) << 40;
            /* FALLTHRU */
        case 5:
            b |= ((u64)ni[4]) << 32;
            /* FALLTHRU */
        case 4:
            b |= ((u64)ni[3]) << 24;
            /* FALLTHRU */
        case 3:
            b |= ((u64)ni[2]) << 16;
            /* FALLTHRU */
        case 2:
            b |= ((u64)ni[1]) << 8;
            /* FALLTHRU */
        case 1:
            b |= ((u64)ni[0]);
            break;
        case 0:
            break;
        }

        v3 ^= b;

        for (i = 0; i < cROUNDS; ++i)
            SIPROUND;

        v0 ^= b;

        if (outlen == 16)
            v2 ^= 0xee;
        else
            v2 ^= 0xff;

        for (i = 0; i < dROUNDS; ++i)
            SIPROUND;

        b = v0 ^ v1 ^ v2 ^ v3;
        U64TO8_LE(out, b);

        v1 ^= 0xdd;

        for (i = 0; i < dROUNDS; ++i)
            SIPROUND;

        b = v0 ^ v1 ^ v2 ^ v3;
        U64TO8_LE(out + 8, b);
    }

#undef SH_UINT64_C

};
