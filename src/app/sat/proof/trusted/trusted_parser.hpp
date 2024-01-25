
#pragma once

#include "secret.hpp"
#include "siphash/siphash.hpp"
#include "trusted_utils.hpp"

class TrustedParser {

private:
    FILE* _f;
    FILE* _out;

    int* _data;
    unsigned long _datalen {0};
    unsigned long _datacap {0};

    bool _comment {false};
    bool _header {false};
    bool _input_finished {false};
    bool _input_invalid {false};
    bool _began_num {false};

    int _num {0};
    int _sign {1};
    int _nb_read_cls {0};

    int _nb_vars {-1};
    int _nb_cls {-1};

    SipHash _hash;

public:
    TrustedParser(const char* filename, FILE* out) : _hash(Secret::SECRET_KEY) {
        _f = fopen(filename, "r");
        _out = out;
        _datacap = 1<<14;
        _data = (int*) malloc(_datacap * sizeof(int));
    }
    ~TrustedParser() {
        free(_data);
    }

    bool parse() {
        while (true) {
            int cInt = UNLOCKED_IO(fgetc)(_f);
            if (process((char) cInt)) break;
        }
        if (_began_num) appendInteger();
        if (_datalen > 0) outputLiteralBuffer();
        TrustedUtils::writeSignature(_hash.digest(), _out);
        return _input_finished && !_input_invalid;
    }

private:
    inline bool process(char c) {

        if (_comment && c != '\n' && c != '\r') return false;

        signed char uc = *((signed char*) &c);
        switch (uc) {
        case EOF:
            _input_finished = true;
            return true;
        case '\n':
        case '\r':
            _comment = false;
            if (_began_num) appendInteger();
            break;
        case 'p':
            _header = true;
            break;
        case 'c':
            if (!_header) _comment = true;
            break;
        case ' ':
            if (_began_num) appendInteger();
            break;
        case '-':
            _sign = -1;
            _began_num = true;
            break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            // Add digit to current number
            _num = _num*10 + (c-'0');
            _began_num = true;
            break;
        default:
            break;
        }
        return false;
    }

    inline void appendInteger() {
        if (_header) {
            if (_nb_vars == -1) {
                _nb_vars = _num;
                TrustedUtils::writeInt(_nb_vars, _out);
            } else if (_nb_cls == -1) {
                _nb_cls = _num;
                TrustedUtils::writeInt(_nb_cls, _out);
                _header = false;
            } else TrustedUtils::doAbort();
            _num = 0;
            _began_num = false;
            return;
        }

        const int lit = _sign * _num;
        if (lit == 0) _nb_read_cls++;
        _num = 0;
        _sign = 1;
        _began_num = false;

        if (_datalen == _datacap) outputLiteralBuffer();
        _data[_datalen++] = lit;
    }

    inline void outputLiteralBuffer() {
        _hash.update((unsigned char*) _data, _datalen * sizeof(int));
        TrustedUtils::writeInts(_data, _datalen, _out);
        //for (size_t i = 0; i < _datalen; i++) printf("PARSED %i\n", _data[i]);
        _datalen = 0;
    }
};
