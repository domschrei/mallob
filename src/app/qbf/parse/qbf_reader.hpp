
#include "util/params.hpp"
#include "data/job_description.hpp"

class QbfReader {

public:
    QbfReader(const Parameters& params, const std::string& filename) {}
    bool read(JobDescription& desc) {return false;}
};