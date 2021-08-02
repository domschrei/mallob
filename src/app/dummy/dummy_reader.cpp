
#include "dummy_reader.hpp"

bool DummyReader::read(const std::string& filename, JobDescription& desc) {
    desc.beginInitialization(desc.getRevision());
	desc.endInitialization();
    return true;
}
