
#include "sweep_reader.hpp"
#include <fstream>
#include <iostream>


class JobDescription;

bool SweepReader::read(const std::vector<std::string>& filenames, JobDescription& desc) {

    printf("ß In Sweep reader\n");
    // read the description and write serialized data 
    // using desc.addPermanentData and desc.addTransientData
    std::string filename = filenames[0];
    std::cout << "Sweep reader reading: " << filename << std::endl;
    desc.beginInitialization(0);
    std::ifstream ifile(filename.c_str(), std::ios::in);
    if (!ifile.is_open()) {
        std::cerr << "There was a problem opening the input file!\n";
        return false;
    }

    std::string line;
    int variables = 0;
    int clauses = 0;

    while (std::getline(ifile, line)) {
        if (line.empty()) continue;
        if (line[0] == 'c') continue; // comment line
        if (line[0] == 'p') {
            std::istringstream iss(line);
            std::string tmp;
            iss >> tmp; // 'p'
            iss >> tmp; // 'cnf'
            iss >> variables;
            iss >> clauses;
            break;
        }
    }
    printf("Reading: %i variables, %i clauses\n",variables, clauses);
    desc.addPermanentData(variables);
    desc.addPermanentData(clauses);

    int lit;
    int seen_clauses = 0;
    while (ifile >> lit) {
        desc.addPermanentData(lit);
        if (lit==0) seen_clauses++;
    }
    if (seen_clauses == clauses) {
        printf("Finished reading %i clauses\n", clauses);
    } else {
        printf("\n \nWarning! SweepReader did not read the expected number of clauses. Read: %i, Expected: %i \n \n", seen_clauses, clauses);
    }

    ifile.close();
    desc.endInitialization();

    // success
    return true;
}
