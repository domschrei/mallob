#include "kmeans_utils.hpp"

#include <numeric>
#include <cmath>
#include <vector>
namespace KMeansUtils {
KMeansInstance loadPoints(JobDescription& desc) {
    const int* payload = desc.getFormulaPayload(0);
    KMeansInstance result;
    result.numClusters = payload[0];
    result.dimension = payload[1];
    result.pointsCount = payload[2];
    result.data.reserve(result.pointsCount);
    payload += 3;  // pointer start at first datapoint instead of metadata
    for (int point = 0; point < result.pointsCount; ++point) {
        Point p;
        p.reserve(result.dimension);
        for (int entry = 0; entry < result.dimension; ++entry) {
            p.push_back(*((float*)(payload + entry)));
        }
        result.data.push_back(p);
        payload = payload + result.dimension;
    }
    return result;
}

ClusterMembership calcNearestCenter(KMeansData dataPoints, ClusterCenters clusters,
                                    int numDataPoints, int numClusters,
                                    float metric(Point, Point)) {
    struct centerDistance {
        int cluster;
        float distance;
    } currentNearestCenter;

    ClusterMembership memberships;
    memberships.resize(numDataPoints);
    float distanceToCluster;
    for (int pointID = 0; pointID < numDataPoints; ++pointID) {
        currentNearestCenter.cluster = -1;
        currentNearestCenter.distance = std::numeric_limits<float>::infinity();
        for (int clusterID = 0; clusterID < numClusters; ++clusterID) {
            distanceToCluster = metric(dataPoints[pointID], clusters[clusterID]);
            if (distanceToCluster < currentNearestCenter.distance) {
                currentNearestCenter.cluster = clusterID;
                currentNearestCenter.distance = distanceToCluster;
            }
        }
        memberships[pointID] = currentNearestCenter.cluster;
    }
    return memberships;
}

ClusterCenters calcCurrentClusterCenters(KMeansData dataPoints, ClusterMembership clusters,
                                         int numDataPoints, int numClusters, int dimension) {
    typedef std::vector<float> Dimension;                             // transposed data to reduce dimension by dimension
    typedef std::vector<std::vector<Dimension>> ClusteredDataPoints;  // ClusteredDataPoints[i] contains the points belonging to cluster i
    ClusteredDataPoints clusterdPoints;
    clusterdPoints.resize(numClusters);
    for (auto cluster : clusterdPoints) {
        cluster.resize(dimension);
    }
    for (int pointID = 0; pointID < numDataPoints; ++pointID) {
        for (int d = 0; d < dimension; ++d) {
            clusterdPoints[clusters[pointID]][d].push_back(dataPoints[pointID][d]);
        }
    }
    ClusterCenters clusterCenters;
    clusterCenters.resize(numClusters);
    for (int cluster = 0; cluster < numClusters; ++cluster) {
        for (int d = 0; d < dimension; ++d) {
            clusterCenters[cluster].push_back(std::reduce(clusterdPoints[cluster][d].begin(),
                                                          clusterdPoints[cluster][d].end()) /
                                              static_cast<float>(clusterdPoints[cluster][0].size()));
        }
    }
    return clusterCenters;
}

std::string pointsToString(KMeansData dataPoints) {
    std::stringstream result;

    for (auto point : dataPoints) {
        for (auto entry : point) {
            result << entry << " ";
        }
        result << "\n";
    }
    return result.str();
}
float eukild(KMeansUtils::Point p1, KMeansUtils::Point p2) {
    KMeansUtils::Point difference;
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