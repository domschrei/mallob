
#include <assert.h>

#include "job_description.hpp"
#include "util/logger.hpp"


void JobDescription::beginInitialization() {
    _raw_data.reset(new std::vector<uint8_t>(getMetadataSize()));
    _first_revision = 0;
    _revision = 0;
    _f_size = 0;
    _a_size = 0;
}

void JobDescription::reserveSize(size_t size) {
    _raw_data->reserve(getMetadataSize() + size);
}

void JobDescription::endInitialization() {
    
    _raw_data->reserve(_raw_data->size()+sizeof(int));

    writeMetadataAndPointers();

    // Append size of first and only revision
    push_obj<size_t>(_raw_data, _f_size);
    _revisions_pos_and_size.emplace_back(getMetadataSize(), _f_size);
}

int JobDescription::writeMetadataAndPointers() {

    // Serialize meta data into the vector's beginning (place was reserved earlier)
    int i = 0, n;
    n = sizeof(int);     memcpy(_raw_data->data()+i, &_id, n); i += n;
    n = sizeof(int);     memcpy(_raw_data->data()+i, &_root_rank, n); i += n;
    n = sizeof(float);   memcpy(_raw_data->data()+i, &_priority, n); i += n;
    n = sizeof(bool);    memcpy(_raw_data->data()+i, &_incremental, n); i += n;
    n = sizeof(int);     memcpy(_raw_data->data()+i, &_num_vars, n); i += n;
    n = sizeof(int);     memcpy(_raw_data->data()+i, &_first_revision, n); i += n;
    n = sizeof(int);     memcpy(_raw_data->data()+i, &_revision, n); i += n;
    n = sizeof(float);   memcpy(_raw_data->data()+i, &_wallclock_limit, n); i += n;
    n = sizeof(float);   memcpy(_raw_data->data()+i, &_cpu_limit, n); i += n;
    n = sizeof(int);     memcpy(_raw_data->data()+i, &_max_demand, n); i += n;
    n = sizeof(Checksum);memcpy(_raw_data->data()+i, &_checksum, n); i += n;
    n = sizeof(size_t);  memcpy(_raw_data->data()+i, &_f_size, n); i += n;
    n = sizeof(size_t);  memcpy(_raw_data->data()+i, &_a_size, n); i += n;

    // Set payload pointers, move "i" to 1st position after payload and assumptions
    n = sizeof(int)*_f_size; _f_payload = (int*) (_raw_data->data()+i); i += n;
    n = sizeof(int)*_a_size; _a_payload = (int*) (_raw_data->data()+i); i += n;

    return i;
}

size_t JobDescription::getFormulaPayloadSize(int revision) const {
    int relativeIndex = _revisions_pos_and_size.size() - (_revision - revision) - 1;
    return _revisions_pos_and_size[relativeIndex].second;
}

const int* JobDescription::getFormulaPayload(int revision) const {
    int relativeIndex = _revisions_pos_and_size.size() - (_revision - revision) - 1;
    size_t pos = _revisions_pos_and_size[relativeIndex].first;
    return (const int*) (_raw_data->data()+pos);
}

size_t JobDescription::getTransferSize(int firstIncludedRevision) const {
    size_t size = getMetadataSize();
    for (int rev = firstIncludedRevision; rev <= _revision; rev++) {
        size += sizeof(int) * getFormulaPayloadSize(rev) + sizeof(size_t);
    }
    size += sizeof(int) * getAssumptionsSize();
    if (firstIncludedRevision == 0) assert(size == getFullTransferSize() || log_return_false("%i != %i\n", size, getFullTransferSize()));
    return size;
}



constexpr int JobDescription::getMetadataSize() const {
    return   6*sizeof(int)
            +3*sizeof(float)
            +sizeof(bool)
            +2*sizeof(size_t)
            +sizeof(Checksum);
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
    size_t i = 0, n;

    // Basic data
    n = sizeof(int);     memcpy(&_id, _raw_data->data()+i, n);               i += n;
    n = sizeof(int);     memcpy(&_root_rank, _raw_data->data()+i, n);        i += n;
    n = sizeof(float);   memcpy(&_priority, _raw_data->data()+i, n);         i += n;
    n = sizeof(bool);    memcpy(&_incremental, _raw_data->data()+i, n);      i += n;
    n = sizeof(int);     memcpy(&_num_vars, _raw_data->data()+i, n);         i += n;
    n = sizeof(int);     memcpy(&_first_revision, _raw_data->data()+i, n);   i += n;
    n = sizeof(int);     memcpy(&_revision, _raw_data->data()+i, n);         i += n;
    n = sizeof(float);   memcpy(&_wallclock_limit, _raw_data->data()+i, n);  i += n;
    n = sizeof(float);   memcpy(&_cpu_limit, _raw_data->data()+i, n);        i += n;
    n = sizeof(int);     memcpy(&_max_demand, _raw_data->data()+i, n);       i += n;
    n = sizeof(Checksum);memcpy(&_checksum, _raw_data->data()+i, n);         i += n;
    n = sizeof(size_t);  memcpy(&_f_size, _raw_data->data()+i, n);           i += n;
    n = sizeof(size_t);  memcpy(&_a_size, _raw_data->data()+i, n);           i += n;

    // Payload
    size_t pos = i; // position where revisions' payloads begin
    n = sizeof(int)*_f_size; _f_payload = (int*) (_raw_data->data()+i); i += n;
    n = sizeof(int)*_a_size; _a_payload = (int*) (_raw_data->data()+i); i += n;

    // Size and data of revisions
    n = sizeof(size_t);
    size_t revSize;
    while (i < _raw_data->size()) {
        memcpy(&revSize, _raw_data->data()+i, n); i += n;
        _revisions_pos_and_size.emplace_back(pos, revSize);
        pos += revSize*sizeof(int);
    }
}

std::vector<uint8_t> JobDescription::serialize() const {
    return *_raw_data;
}

const std::shared_ptr<std::vector<uint8_t>>& JobDescription::getSerialization() {
    return _raw_data;
}

std::shared_ptr<std::vector<uint8_t>> JobDescription::extractUpdate(int firstIncludedRevision) const {

    assert(firstIncludedRevision <= _revision || log_return_false("%i != %i\n", firstIncludedRevision, _revision));

    log(V4_VVER, "Extract revisions %i .. %i\n", firstIncludedRevision, _revision);

    // Create base object
    JobDescription desc(_id, _priority, _incremental);
    desc._root_rank = _root_rank;
    desc._num_vars = _num_vars;
    desc._wallclock_limit = _wallclock_limit;
    desc._cpu_limit = _cpu_limit;
    desc._max_demand = _max_demand;
    desc._arrival = _arrival;
    desc._checksum = _checksum;

    desc.beginInitialization(); // allocate space for meta data
    desc._first_revision = firstIncludedRevision;
    desc._revision = _revision;

    // Insert each desired revision's payload
    size_t pos = getMetadataSize();
    for (int rev = firstIncludedRevision; rev <= _revision; rev++) {
        size_t fSize = getFormulaPayloadSize(rev);
        log(V4_VVER, "Revision %i: size %i\n", rev, fSize);
        const int* fData = getFormulaPayload(rev);
        desc._raw_data->insert(desc._raw_data->end(), (uint8_t*)fData, (uint8_t*)(fData+fSize));
        desc._revisions_pos_and_size.emplace_back(pos, fSize);
        desc._f_size += fSize;
        pos += sizeof(int) * fSize;
    }

    // Insert assumptions
    desc._raw_data->insert(desc._raw_data->end(), (uint8_t*)_a_payload, (uint8_t*)(_a_payload+_a_size));
    desc._a_size = _a_size;

    // Append revisions' size and data
    for (const auto& [_, size] : desc._revisions_pos_and_size) {
        push_obj<size_t>(desc._raw_data, size);
    }

    // Write all meta data into raw data field
    desc.writeMetadataAndPointers();

    assert(desc._raw_data->size() == getTransferSize(firstIncludedRevision) || 
            log_return_false("%i != %i\n", desc._raw_data->size(), 
            getTransferSize(firstIncludedRevision)));
    assert(desc.getRevision() == _revision);
    return desc._raw_data;
}

void JobDescription::applyUpdate(const std::shared_ptr<std::vector<uint8_t>>& packed) {

    JobDescription desc;
    desc.deserialize(packed);

    // Cut off raw data after the main payload (so far)
    _raw_data->resize(_raw_data->size() 
        - sizeof(int) * _a_size                           // old assumptions
        - sizeof(size_t) * _revisions_pos_and_size.size() // size of each revision so far
    );

    int firstNewRevision = _revision+1;
    log(V4_VVER, "Got revisions %i .. %i, locally have rev. %i .. %i\n", 
            desc._first_revision, desc._revision, 
            _first_revision, _revision);
    assert(_revision < desc._revision);
    assert(firstNewRevision == desc._first_revision);

    // Update revision
    _revision = desc._revision;
    _num_vars = std::max(_num_vars, desc._num_vars);
    size_t pos = _raw_data->size();

    // Append new permanent payload (clauses)
    const int* newPayload = desc.getFormulaPayload();
    for (size_t i = 0; i < desc.getFormulaSize(); i++) {
        addLiteral(*(newPayload+i));
    }

    // Check checksum
    if (desc._checksum.get() != _checksum.get()) {
        log(V0_CRIT, "ERROR: Checksum fail! Incoming count: %ld ; local count: %ld\n", desc._checksum.count(), _checksum.count());
        abort();
    }

    // Append new assumptions
    const int* newAssumptions = desc.getAssumptionsPayload();
    _a_size = 0;
    for (size_t i = 0; i < desc.getAssumptionsSize(); i++) {
        addAssumption(*(newAssumptions+i));
    }

    // Append old revision sizes
    for (const auto& [_, size] : _revisions_pos_and_size) {
        push_obj<size_t>(_raw_data, size);
    }
    // Append new revision sizes
    for (int rev = firstNewRevision; rev <= _revision; rev++) {
        push_obj<size_t>(_raw_data, desc.getFormulaPayloadSize(rev));
    }
    // Set correct size and data of each revision
    for (int rev = firstNewRevision; rev <= _revision; rev++) {
        _revisions_pos_and_size.emplace_back(
            pos, desc.getFormulaPayloadSize(rev) 
        );
        pos += desc.getFormulaPayloadSize(rev) * sizeof(int);
    }

    // Rewrite meta data, reset payload + assumptions pointers
    writeMetadataAndPointers();
}

void JobDescription::clearPayload() {
    _raw_data.reset();
}
