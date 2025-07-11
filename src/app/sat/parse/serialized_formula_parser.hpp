
#pragma once

#include "app/sat/data/formula_compressor.hpp"
#include "util/logger.hpp"

#define SERIALIZED_FORMULA_PARSER_BASE_CLS_CHKSUM 17

class SerializedFormulaParser {

private:
    Logger& _logger;

    const int* _payload;
    const size_t _size;

    size_t _pos {0};
    bool _last_lit_zero {true};
    bool _parsing_assumptions {false};

    int _chksum {1337};
    int _cls_chksum {SERIALIZED_FORMULA_PARSER_BASE_CLS_CHKSUM};

    bool _has_true_chksum {false};
    int _true_chksum {1337};

    bool _compressed {false};
    FormulaCompressor::CompressedFormulaView _compr_view;

public:
    SerializedFormulaParser(Logger& logger, const int* data, size_t size) : 
        _logger(logger), _payload(data), _size(size) {}

    void setCompressed() {
        _compr_view = FormulaCompressor::getView((const unsigned char*) _payload, sizeof(int) * _size);
        _compressed = true;
    }
    bool isCompressed() const {
        return _compressed;
    }

    bool getNextLiteral(int& lit) {

        if (_compressed) {
            return _compr_view.getNextLit(lit);
        }

        if (_pos == _size) return false; // done
        if (_parsing_assumptions) return false; // no clause lits left

        lit = _payload[_pos++];
        if (_last_lit_zero && lit == INT32_MAX) {
            _parsing_assumptions = true;
            return false;
        }
        _last_lit_zero = lit == 0;

        if (lit == 0) {
            _chksum ^= _cls_chksum;
            _cls_chksum = SERIALIZED_FORMULA_PARSER_BASE_CLS_CHKSUM;
        } else {
            _cls_chksum ^= lit;
        }

        return true; // success
    }

    bool getNextAssumption(int& lit) {

        if (_compressed) {
            return _compr_view.getNextAssumption(lit);
        }

        if (_pos == _size) return false; // done
        if (!_parsing_assumptions) return false;

        lit = _payload[_pos++];
        return (lit != 0);
    }

    const int* getRawPayload() const {
        return _payload;
    }
    size_t getPayloadSize() const {
        return _size;
    }

    void verifyChecksum() const {
        if (_has_true_chksum && _true_chksum != _chksum) {
            LOGGER(_logger, V0_CRIT, "[ERROR] Checksum fail: expected %i, got %i\n", _true_chksum, _chksum);
            abort();
        }
    }
};
