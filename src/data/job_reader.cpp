
#include "job_reader.hpp"

#include "app/dummy/dummy_reader.hpp"

bool JobReader::read(const std::string& file, SatReader::ContentMode contentMode, JobDescription& desc) {
    switch (desc.getApplication()) {
    case JobDescription::DUMMY:
        return DummyReader::read(file, desc);
    case JobDescription::ONESHOT_SAT:
    case JobDescription::INCREMENTAL_SAT:
        return SatReader(file, contentMode).read(desc);
    default:
        return false;
    }
}
