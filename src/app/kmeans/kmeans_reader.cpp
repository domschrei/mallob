#include "kmeans_reader.hpp"
#include <fstream>
#include <vector>
#include <cstdlib>
#include <iostream>
typedef std::vector<double> Point;
//bool KMeansReader::read(const std::string &filename, JobDescription &desc) {
bool KMeansReader::read(const std::string &filename) {
    // allocate necessary structs for the revision to read
    //desc.beginInitialization(desc.getRevision());

    // Read file with mmap
    std::ifstream ifile(filename.c_str(), std::ios::in);
    std::vector<Point> points;
    off_t size;

    // check to see that the file was opened correctly:
    if (!ifile.is_open()) {
        std::cerr << "There was a problem opening the input file!\n";
        exit(1); // exit or do additional error checking
    }

    int k = 0;
    int dim = 0;
    int count = 0;
    ifile >> k;
    ifile >> dim;
    ifile >> count;
    
    double num = 0.0;
    // keep storing values from the text file so long as data exists:
    for (int point = 0; point < count; ++point) {
        Point p;
        for (int entry = 0; entry < dim; ++entry) {
            ifile >> num;
            p.push_back(num);
        }
        points.push_back(p);
    } 
    
    std::cout << "K: " << k << std::endl;
    std::cout << "dim: " << dim << std::endl;
    std::cout << "count: " << count << std::endl;
    // verify that the scores were stored correctly:
    for (int i = 0; i < points.size(); ++i) {
        std::cout << points[i][0] << std::endl;
    }

    // finalize revision
    //desc.endInitialization();

    // success
    return true;
}
