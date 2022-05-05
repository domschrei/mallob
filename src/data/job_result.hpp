
#ifndef DOMPASCH_MALLOB_JOB_RESULT_H
#define DOMPASCH_MALLOB_JOB_RESULT_H

#include <memory>
#include <vector>
#include <cstring>

#include "serializable.hpp"
#include "util/assert.hpp"

struct JobResult : public Serializable {

    int id = 0;
    int revision;
    int result;
    enum EncodedType {INT, FLOAT} encodedType = INT;

private:
    std::vector<int> solution;
    std::vector<uint8_t> packedData;

public:
    JobResult() {}
    JobResult(std::vector<uint8_t>&& packedData);

    int getTransferSize() const {return sizeof(int)*3 + sizeof(int)*solution.size();}

    JobResult& deserialize(const std::vector<uint8_t>& packed) override;

    std::vector<uint8_t> serialize() const override;
    std::vector<uint8_t>&& moveSerialization();
    void updateSerialization();

    void setSolution(std::vector<int>&& solution);
    void setSolutionToSerialize(const int* begin, size_t size);

    size_t getSolutionSize() const;
    inline int getSolution(size_t pos) const {
        assert(pos < getSolutionSize());
        static_assert(sizeof(int) == sizeof(EncodedType));
        if (!packedData.empty()) {
            return *(
                (int*) (packedData.data() + (4+pos)*sizeof(int))
            );
        }
        return solution[pos];
    }

    std::vector<int> extractSolution();
};

#endif