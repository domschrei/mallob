#include "kmeans_utils.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <numeric>
#include <vector>

#include "util/assert.hpp"
#include "util/logger.hpp"
namespace KMeansUtils {

typedef std::vector<float> Point;
float eukild(const float* p1, const float* p2, const size_t dim) {
    float sum = 0;
    float diff;
    for (int d = 0; d < dim; ++d) {
        diff = p1[d] - p2[d];
        sum += diff*diff;
    }

    return sum;
}
// childIndexesOf(1, 12) = [3, 4, 7, 8, 9, 10]
std::vector<int> childIndexesOf(int parentIndex, int jobVolume) {
    std::vector<int> indexList;
    std::vector<int> childBuffer;
    int currentIndex;
    childBuffer.push_back(parentIndex * 2 + 1);
    childBuffer.push_back(parentIndex * 2 + 2);
    while (!childBuffer.empty()) {
        currentIndex = childBuffer[childBuffer.size() - 1];
        childBuffer.pop_back();
        if (currentIndex < jobVolume) {
            indexList.push_back(currentIndex);
            childBuffer.push_back(currentIndex * 2 + 1);
            childBuffer.push_back(currentIndex * 2 + 2);
        }
    }
    return indexList;
}
}  // namespace KMeansUtils