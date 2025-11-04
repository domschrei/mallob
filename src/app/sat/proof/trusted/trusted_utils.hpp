
#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib> // exit
#include <stdio.h> // file I/O
#include <string>
#include <unistd.h> // getpid

#ifdef _MSC_VER
#    define MALLOB_LIKELY(condition) condition
#    define MALLOB_UNLIKELY(condition) condition
#else
#    define MALLOB_LIKELY(condition) __builtin_expect(condition, 1)
#    define MALLOB_UNLIKELY(condition) __builtin_expect(condition, 0)
#endif

#define UNLOCKED_IO(fun) fun##_unlocked
//#define UNLOCKED_IO(fun) fun

typedef unsigned long u64;
typedef uint32_t u32;
typedef unsigned char u8;

static constexpr int SIG_SIZE_BYTES = 16;
typedef u8 signature[SIG_SIZE_BYTES];

struct TrustedUtils {
    static void doAbort() {
        log("ABORT");
        while (true) {}
    }
    static void doAssert(bool exp) {
        if (!exp) doAbort();
    }

    static void log(const char* msg) {
        printf("[IMPCHK %i] %s\n", getpid(), msg);
    }
    static void log(const char* msg1, const char* msg2) {
        printf("[IMPCHK %i] %s %s\n", getpid(), msg1, msg2);
    }

    static bool beginsWith(const char* str, const char* prefix) {
        u64 i = 0;
        while (true) {
            if (prefix[i] == '\0') return true;
            if (str[i] == '\0') return prefix[i] == '\0';
            if (str[i] != prefix[i]) return false;
            i++;
        }
    }

    static void copyBytes(u8* to, const u8* from, size_t nbBytes) {
        for (size_t i = 0; i < nbBytes; i++) to[i] = from[i];
    }

    static bool equalSignatures(const u8* left, const u8* right) {
        for (size_t i = 0; i < SIG_SIZE_BYTES; i++) {
            if (left[i] != right[i]) return false;
        }
        return true;
    }

    static std::string sigToStr(const u8* sig) {
        std::string out(2*SIG_SIZE_BYTES, '0');
        for (int charpos = 0; charpos < SIG_SIZE_BYTES; charpos++) {
            char val1 = (sig[charpos] >> 4) & 0x0f;
            char val2 = sig[charpos] & 0x0f;
            assert(val1 >= 0 && val1 < 16);
            assert(val2 >= 0 && val2 < 16);
            out[2*charpos+0] = val1>=10 ? 'a'+val1-10 : '0'+val1;
            out[2*charpos+1] = val2>=10 ? 'a'+val2-10 : '0'+val2;
        }
        return out;
    }
    static bool strToSig(const std::string& str, u8* out) {
        for (int bytepos = 0; bytepos < SIG_SIZE_BYTES; bytepos++) {
            const char* hex_pair = str.data() + bytepos*2;
            char hex1 = hex_pair[0]; char hex2 = hex_pair[1];
            int byte = 16 * (hex1 >= '0' && hex1 <= '9' ? hex1-'0' : 10+hex1-'a')
                        + (hex2 >= '0' && hex2 <= '9' ? hex2-'0' : 10+hex2-'a');
            if (byte < 0 || byte >= 256) return false;
            out[bytepos] = (u8) byte;
        }
        return true;
    }
};

class ImpCheckIO {

private:
    bool _encountered_eof {false};

public:
    bool encounteredEOF() const {
        return _encountered_eof;
    }
    void setEof() {
        TrustedUtils::log("encountered end-of-file");
        _encountered_eof = true;
        //::exit(0);
    }

    void readSignature(u8* outSig, FILE* file) {
        signature dummy;
        if (!outSig) outSig = dummy;
        u64 nbRead = UNLOCKED_IO(fread)(outSig, sizeof(int), 4, file);
        if (nbRead < 4) setEof();
    }
    void writeSignature(const u8* sig, FILE* file) {
        UNLOCKED_IO(fwrite)(sig, sizeof(int), 4, file);
    }

    u64 readUnsignedLong(FILE* file) {
        u64 u;
        u64 nbRead = UNLOCKED_IO(fread)(&u, sizeof(u64), 1, file);
        if (nbRead < 1) setEof();
        return u;
    }
    void writeUnsignedLong(u64 u, FILE* file) {
        UNLOCKED_IO(fwrite)(&u, sizeof(u64), 1, file);
    }
    void writeUnsignedLongs(const u64* data, size_t nbHints, FILE* file) {
        UNLOCKED_IO(fwrite)(data, sizeof(u64), nbHints, file);
    }

    int readInt(FILE* file) {
        int i;
        u64 nbRead = UNLOCKED_IO(fread)(&i, sizeof(int), 1, file);
        if (nbRead < 1) setEof();
        return i;
    }
    u32 readUint(FILE* file) {
        u32 i;
        u64 nbRead = UNLOCKED_IO(fread)(&i, sizeof(u32), 1, file);
        if (nbRead < 1) setEof();
        return i;
    }
    void readInts(int* data, size_t nbInts, FILE* file) {
        u64 nbRead = UNLOCKED_IO(fread)(data, sizeof(int), nbInts, file);
        if (nbRead < nbInts) setEof();
    }
    void writeInt(int i, FILE* file) {
        UNLOCKED_IO(fwrite)(&i, sizeof(int), 1, file);
    }
    void writeUint(u32 i, FILE* file) {
        UNLOCKED_IO(fwrite)(&i, sizeof(u32), 1, file);
    }
    void writeInts(const int* data, size_t nbInts, FILE* file) {
        UNLOCKED_IO(fwrite)(data, sizeof(int), nbInts, file);
    }

    int readChar(FILE* file) {
        int res = UNLOCKED_IO(fgetc)(file);
        if (res == EOF) setEof();
        return res;
    }
    void writeChar(char c, FILE* file) {
        UNLOCKED_IO(fputc)(c, file);
    }
};
