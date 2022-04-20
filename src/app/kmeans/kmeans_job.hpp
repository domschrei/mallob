
#ifndef DOMPASCH_MALLOB_KMEANS_JOB_HPP
#define DOMPASCH_MALLOB_KMEANS_JOB_HPP

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
    KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId)
        : Job(params, commSize, worldRank, jobId, JobDescription::Application::KMEANS) {}
    void appl_start() override {
        const JobDescription& desc = getDescription();
        const int* payload = desc.getFormulaPayload(0);
        KMeansUtils::KMeansInstance instance = KMeansUtils::loadPoints(desc);

        KMeansUtils::ClusterCenters clusterCenters;
        clusterCenters.resize(instance.numClusters);
        for (int i = 0; i < instance.numClusters; ++i) {
            clusterCenters[i] = instance.data[static_cast<int>((static_cast<float>(i) / static_cast<float>(instance.numClusters)) * (instance.pointsCount - 1))];
        }

        for (int i = 0; i < 5; ++i) {
            KMeansUtils::ClusterMembership clusterMembership;
            clusterMembership = KMeansUtils::calcNearestCenter(instance.data,
                                                               clusterCenters,
                                                               instance.pointsCount,
                                                               instance.numClusters,
                                                               KMeansUtils::eukild);
            std::vector<int> countMembers(instance.numClusters, 0);
            for (int clusterID : clusterMembership) {
                countMembers[clusterID] += 1;
            }
            std::stringstream countMembersString;
            std::copy(countMembers.begin(), countMembers.end(), std::ostream_iterator<int>(countMembersString, " "));
            clusterCenters = KMeansUtils::calcCurrentClusterCenters(instance.data,
                                                                    clusterMembership,
                                                                    instance.pointsCount,
                                                                    instance.numClusters,
                                                                    instance.dimension);
        }
        // ProcesswideThreadpool(work, data)
        /*
        std::cout << "K: " << k << std::endl;
        std::cout << "dim: " << dim << std::endl;
        std::cout << "cols: " << col << std::endl;
        std::cout << "count: " << count << std::endl;
        for (int point = 0; point < count; ++point) {
            for (int entry = 0; entry < dim; ++entry) {
                std::cout << points[point][entry] << ' ';
            }
            std::cout << std::endl;
        }
        */
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
};

#endif
