#include <string>

#include "data/job_description.hpp"

namespace KMeansUtils {
    typedef std::vector<float> Point;
    float eukild(Point& p1, Point& p2);
    std::vector<int> childIndexesOf(int parentIndex, int jobVolume);
};  // namespace KMeansUtils
