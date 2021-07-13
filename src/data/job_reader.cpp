
#include "job_reader.hpp"

bool JobReader::read(const std::string& file, JobDescription& desc) {
    switch (desc.getApplication()) {
    case JobDescription::DUMMY:
        return true;
    case JobDescription::SAT:
        return SatReader(file).read(desc);
    }
}
