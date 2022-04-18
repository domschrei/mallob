
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

#include "util/random.hpp"
#include "app/kmeans/kmeans_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

int main() {
    Timer::init();
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    auto files = {"benign_traffic.csv"};
    
    for (const auto& file : files) {
        auto f = std::string("instances/") + file;
        LOG(V2_INFO, "Reading test KMeans File %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        JobDescription desc;
        bool success = KMeansReader::read(f, desc);
        assert(success);
        const int* payload = desc.getFormulaPayload(0);
        int numClusters = payload[0]; 
        int dimension = payload[1];  
        int pointsCount = payload[2];

        LOG(V2_INFO, "K: %d \n", numClusters);
        LOG(V2_INFO, "Dimension %d \n", dimension);
        LOG(V2_INFO, "Count of points %d \n", pointsCount);

        typedef std::vector<float> Point;
        std::vector<Point> points;
        std::string lastPoint;
        payload += 3; //pointer start at first point instead of metadata

        for (int point = 0; point < pointsCount; ++point) {
            Point p;
            for (int entry = 0; entry < dimension; ++entry) {
                p.push_back(*( (float*) (payload + entry)));
            }
            points.push_back(p);
            payload = payload + dimension;
        } 

        lastPoint = "";
        for (int i = 0; i < std::min(5, pointsCount); ++i) {
            lastPoint.append(std::to_string(points[pointsCount-1][i]));
        }
        LOG(V2_INFO, "Last Point is: \n %s \n", lastPoint);
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
        assert(desc.getNumFormulaLiterals() > 0);
    }
}