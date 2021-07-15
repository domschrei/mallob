
#include "dummy_reader.hpp"

bool DummyReader::read(const std::string& filename, JobDescription& desc) {
    int revision = desc.getRevision();
    desc.beginInitialization();
	desc.setFirstRevision(revision);
	desc.setRevision(revision);
	desc.endInitialization();
    return true;
}
