
#include "dummy_reader.hpp"

class JobDescription;

bool DummyReader::read(const std::vector<std::string>& filenames, JobDescription& desc) {

    // You can use the job's AppConfiguration to store configuration options.
    // This must be done **before** beginInitialization is called.
    desc.getAppConfiguration().updateFixedSizeEntry("__optA", 77);

    // This fixes the size of the description's meta data for our revision (0).
    desc.beginInitialization(0);

    // Now add the actual payload to the job, serialized as a sequence of integers.
    // (You would usually parse the provided files here.)
    desc.addData(1);
    desc.addData(2);
    desc.addData(3);

    // You may still update app configuration entries that already exist.
    desc.getAppConfiguration().updateFixedSizeEntry("__optA", 66);

    // Conclude the initialization.
    desc.endInitialization();

    // success
    return true;
}
