#include "kmeans_utils.hpp"

#include <numeric>
#include <cmath>
#include <vector>
#include <functional>

#include <iostream>
#include "util/logger.hpp"
#include "util/assert.hpp"
namespace KMeansUtils {

    typedef std::vector<float> Point;
    float eukild(Point& p1, Point& p2) {
        Point difference;
        float sum = 0;
        int dimension = p1.size();
        difference.resize(dimension);

        for (int d = 0; d < dimension; ++d) {
            difference[d] = p1[d] - p2[d];
        }

        for (auto entry : difference) {
            sum += entry * entry;
        }

        return std::sqrt(sum);
    }
}  // namespace KMeansUtils