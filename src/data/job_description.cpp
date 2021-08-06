
#include <assert.h>

#include "job_description.hpp"
#include "util/logger.hpp"


void JobDescription::beginInitialization(int revision) {
    _revision = revision;
    while (_revision >= _data_per_revision.size()) _data_per_revision.emplace_back();
    _data_per_revision[_revision].reset(new std::vector<uint8_t>(
        getMetadataSize()
    ));
    _f_size = 0;
    _a_size = 0;
}

void JobDescription::reserveSize(size_t size) {
    getRevisionData(_revision)->reserve(getMetadataSize() + size);
}

void JobDescription::endInitialization() {
    // Add preloaded assumptions (if any)
    for (int a : _preloaded_assumptions) addAssumption(a);
    _preloaded_assumptions.clear();

    writeMetadata();
}

void JobDescription::writeMetadata() {

    auto& data = getRevisionData(_revision);

    // Serialize meta data into the vector's beginning (place was reserved earlier)
    int i = 0, n;
    n = sizeof(int);         memcpy(data->data()+i, &_id, n); i += n;
    n = sizeof(int);         memcpy(data->data()+i, &_revision, n); i += n;
    n = sizeof(size_t);      memcpy(data->data()+i, &_f_size, n); i += n;
    n = sizeof(size_t);      memcpy(data->data()+i, &_a_size, n); i += n;
    n = sizeof(int);         memcpy(data->data()+i, &_root_rank, n); i += n;
    n = sizeof(float);       memcpy(data->data()+i, &_priority, n); i += n;
    n = sizeof(bool);        memcpy(data->data()+i, &_incremental, n); i += n;
    n = sizeof(int);         memcpy(data->data()+i, &_num_vars, n); i += n;
    n = sizeof(float);       memcpy(data->data()+i, &_wallclock_limit, n); i += n;
    n = sizeof(float);       memcpy(data->data()+i, &_cpu_limit, n); i += n;
    n = sizeof(int);         memcpy(data->data()+i, &_max_demand, n); i += n;
    n = sizeof(Application); memcpy(data->data()+i, &_application, n); i += n;
    n = sizeof(Checksum);    memcpy(data->data()+i, &_checksum, n); i += n;
}

const std::shared_ptr<std::vector<uint8_t>>& JobDescription::getRevisionData(int revision) const {
    assert(revision >= 0 && revision < _data_per_revision.size());
    return _data_per_revision.at(revision);
}

std::shared_ptr<std::vector<uint8_t>>& JobDescription::getRevisionData(int revision) {
    assert(revision >= 0 && revision < _data_per_revision.size());
    return _data_per_revision.at(revision);
}

size_t JobDescription::getFormulaPayloadSize(int revision) const {
    size_t fSize;
    memcpy(&fSize, getRevisionData(revision)->data()+2*sizeof(int), sizeof(size_t));
    return fSize;
}

size_t JobDescription::getAssumptionsSize(int revision) const {
    size_t aSize;
    memcpy(&aSize, getRevisionData(revision)->data()+2*sizeof(int)+sizeof(size_t), sizeof(size_t));
    return aSize;
}

const int* JobDescription::getFormulaPayload(int revision) const {
    size_t pos = getMetadataSize();
    return (const int*) (getRevisionData(revision)->data()+pos);
}

const int* JobDescription::getAssumptionsPayload(int revision) const {
    size_t pos = getMetadataSize() + sizeof(int)*getFormulaPayloadSize(revision);
    return (const int*) (getRevisionData(revision)->data()+pos);
}

size_t JobDescription::getTransferSize(int revision) const {
    return getRevisionData(revision)->size();
}



constexpr int JobDescription::getMetadataSize() const {
    return 5*sizeof(int)
           +3*sizeof(float)
           +sizeof(bool)
           +2*sizeof(size_t)
           +sizeof(Checksum)
           +sizeof(Application);
}



int JobDescription::readRevisionIndex(const std::vector<uint8_t>& serialized) {
    assert(serialized.size() >= 2*sizeof(int)+2*sizeof(size_t));
    int revision;
    memcpy(&revision, serialized.data()+sizeof(int), sizeof(int));
    assert(revision >= 0);
    return revision;
}

int JobDescription::prepareRevision(const std::vector<uint8_t>& packed) {
    int revision = JobDescription::readRevisionIndex(packed);
    while (revision >= _data_per_revision.size()) _data_per_revision.emplace_back();
    return revision;
}

JobDescription& JobDescription::deserialize(std::vector<uint8_t>&& packed) {
    int revision = prepareRevision(packed);
    _data_per_revision[revision].reset(new std::vector<uint8_t>(std::move(packed)));
    deserialize();
    return *this;
}

JobDescription& JobDescription::deserialize(const std::vector<uint8_t>& packed) {
    int revision = prepareRevision(packed);
    _data_per_revision[revision].reset(new std::vector<uint8_t>(packed));
    deserialize();
    return *this;
}

JobDescription& JobDescription::deserialize(const std::shared_ptr<std::vector<uint8_t>>& packed) {
    int revision = prepareRevision(*packed.get());
    _data_per_revision[revision] = packed;
    deserialize();
    return *this;
}

void JobDescription::deserialize() {
    size_t i = 0, n;

    // Basic data
    auto& latestData = _data_per_revision.back();
    n = sizeof(int);         memcpy(&_id, latestData->data()+i, n);              i += n;
    n = sizeof(int);         memcpy(&_revision, latestData->data()+i, n);        i += n;
    n = sizeof(size_t);      memcpy(&_f_size, latestData->data()+i, n);          i += n;
    n = sizeof(size_t);      memcpy(&_a_size, latestData->data()+i, n);          i += n;
    n = sizeof(int);         memcpy(&_root_rank, latestData->data()+i, n);       i += n;
    n = sizeof(float);       memcpy(&_priority, latestData->data()+i, n);        i += n;
    n = sizeof(bool);        memcpy(&_incremental, latestData->data()+i, n);     i += n;
    n = sizeof(int);         memcpy(&_num_vars, latestData->data()+i, n);        i += n;
    n = sizeof(float);       memcpy(&_wallclock_limit, latestData->data()+i, n); i += n;
    n = sizeof(float);       memcpy(&_cpu_limit, latestData->data()+i, n);       i += n;
    n = sizeof(int);         memcpy(&_max_demand, latestData->data()+i, n);      i += n;
    n = sizeof(Application); memcpy(&_application, latestData->data()+i, n);     i += n;
    n = sizeof(Checksum);    memcpy(&_checksum, latestData->data()+i, n);        i += n;
}

std::vector<uint8_t> JobDescription::serialize() const {
    return *_data_per_revision[0];
}

const std::shared_ptr<std::vector<uint8_t>>& JobDescription::getSerialization(int revision) const {
    return getRevisionData(revision);
}

void JobDescription::clearPayload(int revision) {
    getRevisionData(revision)->clear();
}
