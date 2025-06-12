
#include "sweep_reader.hpp"
#include <fstream>
#include <iostream>


class JobDescription;

bool SweepReader::read(const std::vector<std::string>& filenames, JobDescription& desc) {

    // read the description and write serialized data
    // using desc.addPermanentData and desc.addTransientData
    std::string filename = filenames[0];
    std::cout << "[Sweep] reading: " << filename << std::endl;
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
        if (line[0] == 'c') continue;
        if (line[0] == 'p') {
            std::istringstream iss(line);
            std::string tmp;
            iss >> tmp;
            iss >> tmp;
            iss >> variables;
            iss >> clauses;
            break;
        }
    }
	auto& config = desc.getAppConfiguration();
	config.updateFixedSizeEntry("__NC", clauses);
	config.updateFixedSizeEntry("__NV", variables);
    printf("[Sweep] reading: %i variables, %i clauses\n",variables, clauses);
    // desc.getAppConfiguration().updateFixedSizeEntry()
	// desc.setAppConfigurationEntry("__NV", std::to_string(variables));
	// desc.setAppConfigurationEntry("__NC", std::to_string(clauses));
    // desc.addPermanentData(variables);
    // desc.addPermanentData(clauses);

    desc.beginInitialization(0);
    int lit;
    int seen_clauses = 0;
    while (ifile >> lit) {
        desc.addPermanentData(lit);
        if (lit==0) seen_clauses++;
    }
    if (seen_clauses == clauses) printf("[Sweep] reading: finished\n");
    else printf("\n \nWarning! SweepReader did not read the expected number of clauses. Read: %i, Expected: %i \n \n", seen_clauses, clauses);

    ifile.close();
    desc.endInitialization();

    // success
    return true;
}
