
#ifndef DOMPASCH_MALLOB_SERIALIZABLE_HPP
#define DOMPASCH_MALLOB_SERIALIZABLE_HPP

#include <vector>
#include <cstdint>
#include <cstring>
#include <cassert>

/*
Generic interface for serializing and deserializing arbitrary data, mainly
for the purpose of seamlessly sending it around via MPI.

To serialize a Serializable object, call the serialize() method returning
a shared_ptr to a byte vector.

To deserialize a byte vector, call Serializable::get<T>(byteVector); thereby,
T must be a fitting Serializable object or primitive data type.
Alternatively, you can call deserialize(byteVector) on an "empty" Serializable object. 
*/
class Serializable {

public:
    virtual ~Serializable() = default;
    virtual std::vector<uint8_t> serialize() const = 0;
    virtual Serializable& deserialize(const std::vector<uint8_t>& packed) = 0;
    
    template<typename T>
    static T get(const std::vector<uint8_t>& packed);

    template <typename T>
    static void appendValue(std::vector<std::uint8_t>& packed, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        auto* p = reinterpret_cast<const std::uint8_t*>(&value);
        packed.insert(packed.end(), p, p + sizeof(T));
    }

    template <typename T>
    static void appendVec(std::vector<std::uint8_t>& packed, const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        appendValue(packed, static_cast<std::int32_t>(v.size()));
        auto* p = reinterpret_cast<const std::uint8_t*>(v.data());
        packed.insert(packed.end(), p, p + v.size() * sizeof(T));
    }

    template<typename T>
    static T readValue(const std::vector<std::uint8_t> &packed, size_t &offset) {
        static_assert(std::is_trivially_copyable_v<T>);
        assert(offset + sizeof(T) <= packed.size());
        T value;
        std::memcpy(&value, packed.data() + offset, sizeof(T));
        offset += sizeof(T);
        return value;
    }

    template<typename T>
    static std::vector<T> readVec(const std::vector<std::uint8_t> &packed, size_t &offset) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto count = readValue<std::int32_t>(packed, offset);
        assert(count >= 0 && static_cast<size_t>(count) <= (packed.size() - offset) / sizeof(T));
        std::vector<T> vec(count);
        if (count > 0)
            std::memcpy(vec.data(), packed.data() + offset, count * sizeof(T));
        offset += count * sizeof(T);
        return vec;
    }

    // Bytes -> int32s, zero-padding the tail up to the next 4-byte boundary.
    static std::vector<std::int32_t> bytesToInts(const std::vector<std::uint8_t> &bytes) {
        std::vector<std::int32_t> ints((bytes.size() + sizeof(std::int32_t) - 1) / sizeof(std::int32_t), 0);
        if (!bytes.empty())
            std::memcpy(ints.data(), bytes.data(), bytes.size());
        return ints;
    }

    // int32s -> bytes, in principle just a redefinition, but for code simplicity we use a copy
    static std::vector<std::uint8_t> intsToBytes(const std::vector<std::int32_t> &ints) {
        std::vector<std::uint8_t> bytes(ints.size() * sizeof(std::int32_t));
        if (!ints.empty())
            std::memcpy(bytes.data(), ints.data(), bytes.size());
        return bytes;
    }
    
};

template<typename T> 
T Serializable::get(const std::vector<uint8_t>& packed) {
    if constexpr (std::is_base_of<Serializable, T>()) {
        return T().deserialize(packed);
    } else {
        T elem;
        memcpy(&elem, packed.data(), sizeof(T));
        return elem;
    }
}

#endif
