
#include <assert.h>

#include "job_description.hpp"
#include "util/logger.hpp"


void JobDescription::beginInitialization() {
    _raw_data.reset(new std::vector<uint8_t>(getMetadataSize()));
    _revision = 0;
    _f_size = 0;
    _a_size = 0;
}

void JobDescription::reserveSize(size_t size) {
    _raw_data->reserve(getMetadataSize() + size);
}

void JobDescription::endInitialization() {
    
    _raw_data->shrink_to_fit();

    // Serialize meta data into the vector's beginning (place was reserved earlier)
    int i = 0, n;
    n = sizeof(int);    memcpy(_raw_data->data()+i, &_id, n); i += n;
    n = sizeof(int);    memcpy(_raw_data->data()+i, &_root_rank, n); i += n;
    n = sizeof(float);  memcpy(_raw_data->data()+i, &_priority, n); i += n;
    n = sizeof(bool);   memcpy(_raw_data->data()+i, &_incremental, n); i += n;
    n = sizeof(int);    memcpy(_raw_data->data()+i, &_num_vars, n); i += n;
    n = sizeof(int);    memcpy(_raw_data->data()+i, &_revision, n); i += n;
    n = sizeof(float);  memcpy(_raw_data->data()+i, &_wallclock_limit, n); i += n;
    n = sizeof(float);  memcpy(_raw_data->data()+i, &_cpu_limit, n); i += n;
    n = sizeof(int);    memcpy(_raw_data->data()+i, &_max_demand, n); i += n;
    n = sizeof(size_t); memcpy(_raw_data->data()+i, &_f_size, n); i += n;
    n = sizeof(size_t); memcpy(_raw_data->data()+i, &_a_size, n); i += n;

    // Set payload pointers
    n = sizeof(int)*_f_size; _f_payload = (int*) (_raw_data->data()+i); i += n;
    n = sizeof(int)*_a_size; _a_payload = (int*) (_raw_data->data()+i); i += n;
}



constexpr int JobDescription::getMetadataSize() const {
    return   5*sizeof(int)
            +3*sizeof(float)
            +sizeof(bool)
            +2*sizeof(size_t);
}



JobDescription& JobDescription::deserialize(std::vector<uint8_t>&& packed) {
    _raw_data.reset(new std::vector<uint8_t>(std::move(packed)));
    deserialize();
    return *this;
}

JobDescription& JobDescription::deserialize(const std::vector<uint8_t>& packed) {
    _raw_data.reset(new std::vector<uint8_t>(packed));
    deserialize();
    return *this;
}

JobDescription& JobDescription::deserialize(const std::shared_ptr<std::vector<uint8_t>>& packed) {
    _raw_data = packed;
    deserialize();
    return *this;
}

void JobDescription::deserialize() {
    int i = 0, n;

    // Basic data
    n = sizeof(int);     memcpy(&_id, _raw_data->data()+i, n);               i += n;
    n = sizeof(int);     memcpy(&_root_rank, _raw_data->data()+i, n);        i += n;
    n = sizeof(float);   memcpy(&_priority, _raw_data->data()+i, n);         i += n;
    n = sizeof(bool);    memcpy(&_incremental, _raw_data->data()+i, n);      i += n;
    n = sizeof(int);     memcpy(&_num_vars, _raw_data->data()+i, n);         i += n;
    n = sizeof(int);     memcpy(&_revision, _raw_data->data()+i, n);         i += n;
    n = sizeof(float);   memcpy(&_wallclock_limit, _raw_data->data()+i, n);  i += n;
    n = sizeof(float);   memcpy(&_cpu_limit, _raw_data->data()+i, n);        i += n;
    n = sizeof(int);     memcpy(&_max_demand, _raw_data->data()+i, n);       i += n;
    n = sizeof(size_t);  memcpy(&_f_size, _raw_data->data()+i, n);           i += n;
    n = sizeof(size_t);  memcpy(&_a_size, _raw_data->data()+i, n);           i += n;

    // Payload
    n = sizeof(int)*_f_size; _f_payload = (int*) (_raw_data->data()+i); i += n;
    n = sizeof(int)*_a_size; _a_payload = (int*) (_raw_data->data()+i); i += n;
}

std::vector<uint8_t> JobDescription::serialize() const {
    return *_raw_data;
}

const std::shared_ptr<std::vector<uint8_t>>& JobDescription::getSerialization() {
    return _raw_data;
}



void JobDescription::clearPayload() {
    _raw_data.reset();
}
