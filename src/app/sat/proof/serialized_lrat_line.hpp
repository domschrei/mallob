
#pragma once

#include <vector>
#include <stdint.h>
#include <cstring>

#include "lrat_line.hpp"
#include "util/assert.hpp"

class SerializedLratLine {

private:
    // LratClauseId id;
    // int numLiterals;
    // int literals[numLiterals];
    // int numHints;
    // LratClauseId hints[numHints];
    // bool signsOfHints[numHints];
    std::vector<uint8_t> _data;

public:
    SerializedLratLine() {}
    SerializedLratLine(SerializedLratLine&& moved) : _data(std::move(moved._data)) {}
    SerializedLratLine(std::vector<uint8_t>&& data) : _data(std::move(data)) {
        // Some sanity checks
        /*
        assert(getId() < 100000000L);
        auto [lits, numLits] = getLiterals();
        assert(numLits >= 0 && numLits < 100);
        for (size_t i = 0; i < numLits; i++) {
            assert(std::abs(lits[i]) < 1000000);
        }
        auto [hints, numHints] = getUnsignedHints();
        char* signs = (char*) getSignsOfHints();
        assert(numHints >= 0 && numHints < 1000);
        for (size_t i = 0; i < numHints; i++) {
            assert(hints[i] < 100000000L);
            if (signs[i] != 0 && signs[i] != 1) {
                std::string out;
                for (auto byte : _data) out += std::to_string(byte) + ",";
                out = out.substr(0, out.size()-1);
                LOG(V0_CRIT, "[ERROR] invalid sign in serialized lrat line: %s\n", out.c_str());
                abort();
            }
        }
        */
    }

    SerializedLratLine(const LratLine& line) {
        _data.resize(sizeof(LratClauseId) 
            + sizeof(int) 
            + line.literals.size()*sizeof(int) 
            + sizeof(int) 
            + line.hints.size()*(sizeof(LratClauseId)+sizeof(bool))
        );
        size_t i = 0, n;
        n = sizeof(LratClauseId); memcpy(_data.data()+i, &line.id, n); i += n;
        int numLits = line.literals.size();
        n = sizeof(int); memcpy(_data.data()+i, &numLits, n); i += n;
        n = numLits*sizeof(int); memcpy(_data.data()+i, line.literals.data(), n); i += n;
        int numHints = line.hints.size();
        n = sizeof(int); memcpy(_data.data()+i, &numHints, n); i += n;
        n = numHints*sizeof(LratClauseId); memcpy(_data.data()+i, line.hints.data(), n); i += n;
        for (bool sign : line.signsOfHints) {
            n = sizeof(bool); memcpy(_data.data()+i, &sign, n); i += n;
        }
        assert(i == _data.size());
    }

    SerializedLratLine& operator=(SerializedLratLine&& moved) {
        _data = std::move(moved._data);
        return *this;
    }

    bool empty() const {return _data.empty();}

    void clear() {
        _data.clear();
    }

    std::vector<uint8_t>& data() {
        return _data;
    }

    LratClauseId getId() const {
        LratClauseId id;
        memcpy(&id, _data.data(), sizeof(LratClauseId));
        return id;
    }

    int getNumLiterals() const {
        int numLits;
        memcpy(&numLits, _data.data() + getDataPosOfNumLits(), sizeof(int));
        return numLits;
    }
    int getNumHints() const {
        int numHints;
        memcpy(&numHints, _data.data() + getDataPosOfNumHints(getNumLiterals()), 
            sizeof(int));
        return numHints;
    }

    std::pair<const int*, int> getLiterals() const {
        return std::pair<const int*, int>(
            (const int*) (_data.data()+getDataPosOfNumLits()+sizeof(int)), 
            getNumLiterals()
        );
    }
    std::pair<const LratClauseId*, int> getUnsignedHints() const {
        return std::pair<const LratClauseId*, int>(
            (const LratClauseId*) (_data.data()+getDataPosOfNumHints(getNumLiterals())+sizeof(int)), 
            getNumHints()
        );
    }
    const bool* getSignsOfHints() const {
        return (const bool*) (_data.data()
            + getDataPosOfNumHints(getNumLiterals())
            + sizeof(int)
            + sizeof(LratClauseId)*getNumHints()
        );
    }

    std::string toStr() const {
        std::string out = std::to_string(getId());
        auto [literals, numLits] = getLiterals();
        for (size_t i = 0; i < numLits; i++) out += " " + std::to_string(literals[i]);
        out += " 0 ";
        auto [hints, numHints] = getUnsignedHints();
        auto signsOfHints = getSignsOfHints();
        for (size_t i = 0; i < numHints; i++) {
            out += (signsOfHints[i] ? "" : "-") + std::to_string(hints[i]) + " ";
        }
        out += "0\n";
        return out;
    }

    const std::vector<uint8_t>& data() const {
        return _data;
    }

    bool valid() const {
        return !_data.empty();
    }

    size_t size() const {
        return getSize(getNumLiterals(), getNumHints());
    }

    static size_t getSize(int numLits, int numHints) {
        return sizeof(LratClauseId)
            + sizeof(int)
            + sizeof(int)*numLits
            + sizeof(int)
            + sizeof(LratClauseId)*numHints
            + sizeof(bool)*numHints;
    }
    static int getDataPosOfNumLits() {
        return sizeof(LratClauseId);
    }
    static int getDataPosOfNumHints(int numLits) {
        return sizeof(LratClauseId)
            + sizeof(int)
            + sizeof(int)*numLits;
    }
};
