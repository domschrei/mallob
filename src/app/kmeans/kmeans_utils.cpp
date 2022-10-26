#include "kmeans_utils.hpp"

#include <x86intrin.h>

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
    //const int vecLen = 4;
    //const int d4 = dim / vecLen;
    float sum = 0;
    float diff;
    //float sumArr[d4 + vecLen -1];
    //__m128 v1, v2, vd;
    //for (int i = 0; i < d4; ++i) {
    //    v1 = _mm_loadu_ps(p1 + (i * vecLen));
    //    v2 = _mm_loadu_ps(p2 + (i * vecLen));
    //    vd = _mm_sub_ps(v1, v2);
    //    vd = _mm_dp_ps(vd, vd, 255);
    //    _mm_storeu_ps(sumArr + i, vd);
    //}
    //for (int i = d4 * vecLen; i < dim; ++i) {
    //    diff = p1[i] - p2[i];
    //    sum += diff * diff;
    //}
    //for (int j = 0; j < d4; j++) {
    //    sum += sumArr[j];
    //}

     for(const float* const p1End = p1 + dim; p1 < p1End; ++p1, ++p2 ) {
        diff = p1[0] - p2[0];
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