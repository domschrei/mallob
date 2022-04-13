#include "kmeans_reader.hpp"
#include <fstream>
#include <vector>
#include <cstdlib>
#include <iostream>
bool KMeansReader::read(const std::string &filename, JobDescription &desc) {
    /* 
    files have to be in format: 
    k = k of kmeans
    dim = dimension of points
    col = number of columns (often there are more than wanted)
    count = count of points

    k dim col count
    0.0 0.1 0.2 
    1.0 1.1 1.2
    one point per row
    */

    // allocate necessary structs for the revision to read
  
    desc.beginInitialization(0);

    std::ifstream ifile(filename.c_str(), std::ios::in);

    // check to see that the file was opened correctly:
    if (!ifile.is_open()) {
        std::cerr << "There was a problem opening the input file!\n";
        return false;
    }

    int numClusters = 0;
    int dimension = 0;
    int col = 0;
    int count = 0;
    int skipCols = 0;

    ifile >> numClusters;
    ifile >> dimension;
    ifile >> col;
    ifile >> count;
    skipCols = col - dimension;
    
    desc.addLiteral(numClusters);
    desc.addLiteral(dimension);
    desc.addLiteral(count);
    
    float num = 0.0;
    for (int point = 0; point < count; ++point) {
        for (int entry = 0; entry < dimension; ++entry) {
            ifile >> num;
            desc.addFloatData(num);
        }
        for (int skip = 0; skip < skipCols; ++skip) {
            ifile >> num;
        }
    } 

    ifile.close();
    desc.endInitialization();
    // success
    return true;
}
