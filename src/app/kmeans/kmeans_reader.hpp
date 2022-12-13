
#ifndef DOMPASCH_MALLOB_KMEANS_READER_HPP
#define DOMPASCH_MALLOB_KMEANS_READER_HPP

#include <string>

#include "data/job_description.hpp"

namespace KMeansReader {
bool read(const std::string& filename, JobDescription& desc);
};

#endif
