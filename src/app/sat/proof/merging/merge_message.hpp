
#pragma once

#include "data/serializable.hpp"
#include "app/sat/proof/serialized_lrat_line.hpp"

struct MergeMessage : public Serializable {

    enum Type {REQUEST, RESPONSE_SUCCESS, RESPONSE_EXHAUSTED} type;
    std::vector<SerializedLratLine> lines;

    static Type getTypeOfMessage(const std::vector<uint8_t>& serializedMsg) {
        Type type;
        memcpy(&type, serializedMsg.data(), sizeof(Type));
        return type;
    }        

    virtual std::vector<uint8_t> serialize() const {

        std::vector<uint8_t> result;
        size_t i = 0, n;

        result.resize(sizeof(Type));
        n = sizeof(Type); memcpy(result.data()+i, &type, n); i += n;

        for (const auto& line : lines) {
            result.insert(result.end(), line.data().begin(), line.data().end());
        }

        return result;
    }

    virtual Serializable& deserialize(const std::vector<uint8_t>& packed) {

        size_t i = 0, n;
        n = sizeof(Type); memcpy(&type, packed.data()+i, n); i += n;

        while (i < packed.size()) {

            n = sizeof(int);
            auto offsetNumLits = SerializedLratLine::getDataPosOfNumLits();
            int numLits;
            memcpy(&numLits, packed.data()+i+offsetNumLits, n);
            auto offsetNumHints = SerializedLratLine::getDataPosOfNumHints(numLits);
            int numHints;
            memcpy(&numHints, packed.data()+i+offsetNumHints, n);

            int lineSize = SerializedLratLine::getSize(numLits, numHints);
            SerializedLratLine line(std::vector<uint8_t>(
                packed.data()+i,
                packed.data()+i+lineSize
            ));

            i += lineSize;
            lines.push_back(std::move(line));
        }

        return *this;
    }
};
