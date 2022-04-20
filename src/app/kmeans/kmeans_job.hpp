
#ifndef DOMPASCH_MALLOB_KMEANS_JOB_HPP
#define DOMPASCH_MALLOB_KMEANS_JOB_HPP

#include <iterator>

#include "app/job.hpp"
#include "kmeans_utils.hpp"

/*
Minimally compiling example "application" for a Mallob job.
Edit and extend for your application.
*/
typedef std::vector<float> Point;
class KMeansJob : public Job {
   private:
    std::vector<Point> points;
   public:
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
    KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId)
        : Job(params, commSize, worldRank, jobId, JobDescription::Application::KMEANS) {}
    void appl_start() override {
        const JobDescription& desc = getDescription();
        const int* payload = desc.getFormulaPayload(0);
        KMeansInstance instance = loadPoints(desc);

        ClusterCenters clusterCenters;
        clusterCenters.resize(instance.numClusters);
        for (int i = 0; i < instance.numClusters; ++i) {
            clusterCenters[i] = instance.data[static_cast<int>((static_cast<float>(i) / static_cast<float>(instance.numClusters)) * (instance.pointsCount - 1))];
        }

        for (int i = 0; i < 5; ++i) {
            ClusterMembership clusterMembership;
            clusterMembership = calcNearestCenter(instance.data,
                                                  clusterCenters,
                                                  instance.pointsCount,
                                                  instance.numClusters,
                                                  [&](Point p1, Point p2) {return eukild(p1, p2);} );
            std::vector<int> countMembers(instance.numClusters, 0);
            for (int clusterID : clusterMembership) {
                countMembers[clusterID] += 1;
            }
            std::stringstream countMembersString;
            std::copy(countMembers.begin(), countMembers.end(), std::ostream_iterator<int>(countMembersString, " "));
            clusterCenters = calcCurrentClusterCenters(instance.data,
                                                       clusterMembership,
                                                       instance.pointsCount,
                                                       instance.numClusters,
                                                       instance.dimension);
        }
        // ProcesswideThreadpool(work, data)
        
    }
    void appl_suspend() override {}
    void appl_resume() override {}
    void appl_terminate() override {}
    int appl_solved() override { return -1; }  // atomic bool
    JobResult&& appl_getResult() override { return JobResult(); }
    void appl_communicate() override {}
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override { return true; }
    void appl_memoryPanic() override {}

    KMeansInstance loadPoints(const JobDescription& desc) {
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

    ClusterMembership calcNearestCenter(const KMeansData& dataPoints, ClusterCenters clusters,
                                        int numDataPoints, int numClusters,
                                        std::function<float(Point, Point)> metric) {
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

    ClusterCenters calcCurrentClusterCenters(const KMeansData& dataPoints, ClusterMembership clusters,
                                             int numDataPoints, int numClusters, int dimension) {
        typedef std::vector<float> Dimension;                             // transposed data to reduce dimension by dimension
        typedef std::vector<std::vector<Dimension>> ClusteredDataPoints;  // ClusteredDataPoints[i] contains the points belonging to cluster i
        ClusteredDataPoints clusterdPoints;
        clusterdPoints.resize(numClusters);
        for (int cluster = 0; cluster < numClusters; ++cluster) {
            clusterdPoints[cluster].resize(dimension);
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

    float eukild(Point p1, Point p2) {
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
};

#endif
