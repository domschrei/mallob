
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

#include "util/random.hpp"
#include "app/kmeans/kmeans_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

int main() {
    std::cout << "until here\n";

    Timer::init();
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    std::cout << "until here\n";

    auto files = {"benign_traffic.csv"};
    
    
    for (const auto& file : files) {
        auto f = std::string("instances/") + file;
        LOG(V2_INFO, "Reading test CNF %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        JobDescription desc;
        bool success = KMeansReader::read(f, desc);
        assert(success);
        const int* payload = desc.getFormulaPayload(0);
        std::cout << "K: " << payload[0] << std::endl;
        std::cout << "dim: " << payload[1] << std::endl;
        std::cout << "n: " << payload[2] << std::endl;
        typedef std::vector<float> Point;
        std::vector<Point> points;
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
        std::cout << "K: " << k << std::endl;
        std::cout << "dim: " << d << std::endl;
        std::cout << "count: " << n << std::endl;
        for (int point = 0; point < n; ++point) {
            for (int entry = 0; entry < d; ++entry) {
                std::cout << points[point][entry] << ' ';
            }
            std::cout << std::endl;
        } 
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
        assert(desc.getNumFormulaLiterals() > 0);
    }
}