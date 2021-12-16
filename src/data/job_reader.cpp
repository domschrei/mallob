
#include "job_reader.hpp"

#include "app/dummy/dummy_reader.hpp"

bool JobReader::read(const std::vector<std::string>& files, SatReader::ContentMode contentMode, JobDescription& desc) {
    switch (desc.getApplication()) {
    case JobDescription::DUMMY:
        return DummyReader::read(files, desc);
    case JobDescription::ONESHOT_SAT:
    case JobDescription::INCREMENTAL_SAT:
        return SatReader(files.front(), contentMode).read(desc);
    default:
        return false;
    }
}
