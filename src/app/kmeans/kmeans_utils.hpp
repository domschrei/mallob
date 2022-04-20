#include <string>

#include "data/job_description.hpp"

namespace KMeansUtils {
typedef std::vector<float> Point;
typedef std::vector<Point> KMeansData;
typedef std::vector<Point> ClusterCenters;   // The centers of cluster 0..n
typedef std::vector<int> ClusterMembership;  // A point KMeansData[i] belongs to cluster ClusterMembership[i]
typedef struct KMeansInstance {
    int numClusters;
    int dimension;
    int pointsCount;
    KMeansData data;
};

KMeansInstance loadPoints(const JobDescription& desc);

ClusterMembership calcNearestCenter(const KMeansData& dataPoints, ClusterCenters clusters,
                                    int numDataPoints, int numClusters,
                                    std::function<float(Point, Point)> metric);

ClusterCenters calcCurrentClusterCenters(const KMeansData& dataPoints, ClusterMembership clusters,
                                         int numDataPoints, int numClusters, int dimension);

std::string pointsToString(KMeansData dataPoints);

float eukild(KMeansUtils::Point p1, KMeansUtils::Point p2);
};  // namespace KMeansUtils
