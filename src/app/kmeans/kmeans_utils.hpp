#include <string>

#include "data/job_description.hpp"

namespace KMeansUtils {
    typedef std::vector<float> Point;
    float eukild(const float* p1, const float* p2, const size_t dim);
    std::vector<int> childIndexesOf(int parentIndex, int jobVolume);
};  // namespace KMeansUtils
