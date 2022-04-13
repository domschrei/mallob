
#ifndef DOMPASCH_MALLOB_KMEANS_JOB_HPP
#define DOMPASCH_MALLOB_KMEANS_JOB_HPP

#include "app/job.hpp"

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
        int k = payload[0]; 
        int d = payload[1];  
        int n = payload[2];

        payload += 3; //pointer start at first point instead of metadata

        for (int point = 0; point < n; ++point) {
            Point p;
            for (int entry = 0; entry < d; ++entry) {
                p.push_back(*( (float*) (payload + entry)));
            }
            points.push_back(p);
            payload = payload + n*d;
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
    int appl_solved() override {return -1;} //atomic bool
    JobResult&& appl_getResult() override {return JobResult();}
    void appl_communicate() override {}
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override {}
    void appl_dumpStats() override {}
    bool appl_isDestructible() override {return true;}
    void appl_memoryPanic() override {}
};

#endif
