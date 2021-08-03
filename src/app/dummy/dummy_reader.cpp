
#include "dummy_reader.hpp"

bool DummyReader::read(const std::string& filename, JobDescription& desc) {

    // allocate necessary structs for the revision to read
    desc.beginInitialization(desc.getRevision());

    // read the description with desc.addLiteral and desc.addAssumption

    // finalize revision
	desc.endInitialization();

    // success
    return true;
}
