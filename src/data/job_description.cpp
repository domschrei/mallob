
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
    
    _raw_data->reserve(_raw_data->size()+2*sizeof(int));

    writeMetadataAndPointers();

    // Append size of first and only revision
    push_obj<size_t>(_raw_data, _f_size);
    push_obj<size_t>(_raw_data, _a_size);
    _revisions_pos_fsize_asize.push_back(
        RevisionInfo{(size_t)getMetadataSize(), _f_size, _a_size}
    );
}

int JobDescription::writeMetadataAndPointers() {

    // Serialize meta data into the vector's beginning (place was reserved earlier)
    int i = 0, n;
    n = sizeof(int);         memcpy(_raw_data->data()+i, &_id, n); i += n;
    n = sizeof(int);         memcpy(_raw_data->data()+i, &_root_rank, n); i += n;
    n = sizeof(float);       memcpy(_raw_data->data()+i, &_priority, n); i += n;
    n = sizeof(bool);        memcpy(_raw_data->data()+i, &_incremental, n); i += n;
    n = sizeof(int);         memcpy(_raw_data->data()+i, &_num_vars, n); i += n;
    n = sizeof(int);         memcpy(_raw_data->data()+i, &_first_revision, n); i += n;
    n = sizeof(int);         memcpy(_raw_data->data()+i, &_revision, n); i += n;
    n = sizeof(float);       memcpy(_raw_data->data()+i, &_wallclock_limit, n); i += n;
    n = sizeof(float);       memcpy(_raw_data->data()+i, &_cpu_limit, n); i += n;
    n = sizeof(int);         memcpy(_raw_data->data()+i, &_max_demand, n); i += n;
    n = sizeof(Application); memcpy(_raw_data->data()+i, &_application, n); i += n;
    n = sizeof(Checksum);    memcpy(_raw_data->data()+i, &_checksum, n); i += n;
    n = sizeof(size_t);      memcpy(_raw_data->data()+i, &_f_size, n); i += n;
    n = sizeof(size_t);      memcpy(_raw_data->data()+i, &_a_size, n); i += n;

    // Move "i" to 1st position after payloads and assumptions
    n = sizeof(int)*_f_size; i += n;
    n = sizeof(int)*_a_size; i += n;
    return i;
}

size_t JobDescription::getFormulaPayloadSize(int revision) const {
    int relativeIndex = _revisions_pos_fsize_asize.size() - (_revision - revision) - 1;
    return _revisions_pos_fsize_asize[relativeIndex].fSize;
}

size_t JobDescription::getAssumptionsSize(int revision) const {
    int relativeIndex = _revisions_pos_fsize_asize.size() - (_revision - revision) - 1;
    return _revisions_pos_fsize_asize[relativeIndex].aSize;
}

const int* JobDescription::getFormulaPayload(int revision) const {
    int relativeIndex = _revisions_pos_fsize_asize.size() - (_revision - revision) - 1;
    size_t pos = _revisions_pos_fsize_asize[relativeIndex].pos;
    return (const int*) (_raw_data->data()+pos);
}

const int* JobDescription::getAssumptionsPayload(int revision) const {
    int relativeIndex = _revisions_pos_fsize_asize.size() - (_revision - revision) - 1;
    size_t pos = _revisions_pos_fsize_asize[relativeIndex].pos;
    return (const int*) (_raw_data->data()+pos+sizeof(int)*_revisions_pos_fsize_asize[relativeIndex].fSize);
}

size_t JobDescription::getTransferSize(int firstIncludedRevision) const {
    size_t size = getMetadataSize();
    for (int rev = firstIncludedRevision; rev <= _revision; rev++) {
        size += sizeof(int) * getFormulaPayloadSize(rev) + sizeof(size_t);
        size += sizeof(int) * getAssumptionsSize(rev) + sizeof(size_t);
    }
    if (firstIncludedRevision == 0) assert(size == getFullTransferSize() || log_return_false("%i != %i\n", size, getFullTransferSize()));
    return size;
}



constexpr int JobDescription::getMetadataSize() const {
    return   6*sizeof(int)
            +3*sizeof(float)
            +sizeof(bool)
            +2*sizeof(size_t)
            +sizeof(Checksum)
            +sizeof(Application);
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
    n = sizeof(int);         memcpy(&_id, _raw_data->data()+i, n);              i += n;
    n = sizeof(int);         memcpy(&_root_rank, _raw_data->data()+i, n);       i += n;
    n = sizeof(float);       memcpy(&_priority, _raw_data->data()+i, n);        i += n;
    n = sizeof(bool);        memcpy(&_incremental, _raw_data->data()+i, n);     i += n;
    n = sizeof(int);         memcpy(&_num_vars, _raw_data->data()+i, n);        i += n;
    n = sizeof(int);         memcpy(&_first_revision, _raw_data->data()+i, n);  i += n;
    n = sizeof(int);         memcpy(&_revision, _raw_data->data()+i, n);        i += n;
    n = sizeof(float);       memcpy(&_wallclock_limit, _raw_data->data()+i, n); i += n;
    n = sizeof(float);       memcpy(&_cpu_limit, _raw_data->data()+i, n);       i += n;
    n = sizeof(int);         memcpy(&_max_demand, _raw_data->data()+i, n);      i += n;
    n = sizeof(Application); memcpy(&_application, _raw_data->data()+i, n);     i += n;
    n = sizeof(Checksum);    memcpy(&_checksum, _raw_data->data()+i, n);        i += n;
    n = sizeof(size_t);      memcpy(&_f_size, _raw_data->data()+i, n);          i += n;
    n = sizeof(size_t);      memcpy(&_a_size, _raw_data->data()+i, n);          i += n;

    // Payload
    size_t pos = i; // position where revisions' payloads begin
    n = sizeof(int)*_f_size; i += n;
    n = sizeof(int)*_a_size; i += n;

    // Size and data of revisions
    n = sizeof(size_t);
    size_t fSize, aSize;
    while (i < _raw_data->size()) {
        memcpy(&fSize, _raw_data->data()+i, n); i += n;
        memcpy(&aSize, _raw_data->data()+i, n); i += n;
        _revisions_pos_fsize_asize.push_back(RevisionInfo{pos, fSize, aSize});
        pos += fSize*sizeof(int) + aSize*sizeof(int);
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

    if (firstIncludedRevision == 0) {
        // Query for the entire description:
        // Can just return the full serialization
        return _raw_data;
    }

    log(V4_VVER, "Extract revisions %i .. %i\n", firstIncludedRevision, _revision);

    // Create base object
    JobDescription desc(_id, _priority, _incremental);
    desc._root_rank = _root_rank;
    desc._num_vars = _num_vars;
    desc._wallclock_limit = _wallclock_limit;
    desc._cpu_limit = _cpu_limit;
    desc._max_demand = _max_demand;
    desc._arrival = _arrival;
    desc._application = _application;
    desc._checksum = _checksum;

    desc.beginInitialization(); // allocate space for meta data
    desc._first_revision = firstIncludedRevision;
    desc._revision = _revision;

    // Insert each desired revision's payload and assumptions
    size_t pos = getMetadataSize();
    for (int rev = firstIncludedRevision; rev <= _revision; rev++) {
        size_t fSize = getFormulaPayloadSize(rev);
        log(V4_VVER, "Revision %i: size %i\n", rev, fSize);
        const int* fData = getFormulaPayload(rev);
        size_t aSize = getAssumptionsSize(rev);
        const int* aData = getAssumptionsPayload(rev);
        desc._raw_data->insert(desc._raw_data->end(), (uint8_t*)fData, (uint8_t*)(fData+fSize));
        desc._raw_data->insert(desc._raw_data->end(), (uint8_t*)aData, (uint8_t*)(aData+aSize));
        desc._revisions_pos_fsize_asize.push_back(RevisionInfo{pos, fSize, aSize});
        desc._f_size += fSize;
        desc._a_size += aSize;
        pos += sizeof(int) * fSize + sizeof(int) * aSize;
    }

    // Append revisions' size and data
    for (const auto& [pos, fSize, aSize] : desc._revisions_pos_fsize_asize) {
        push_obj<size_t>(desc._raw_data, fSize);
        push_obj<size_t>(desc._raw_data, aSize);
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
        - 2 * sizeof(size_t) * _revisions_pos_fsize_asize.size() // size of each revision so far
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

    // Append new formula and assumptions payload
    const int* payloadPtr = (const int*) (desc._raw_data->data() + desc.getMetadataSize());
    for (int rev = firstNewRevision; rev <= _revision; rev++) {
        for (size_t i = 0; i < desc.getFormulaPayloadSize(rev); i++) {
            addLiteral(*payloadPtr); payloadPtr++;
        }
        for (size_t i = 0; i < desc.getAssumptionsSize(rev); i++) {
            addAssumption(*payloadPtr); payloadPtr++;
        }
    }

    // Check checksum
    if (desc._checksum.get() != _checksum.get()) {
        log(V0_CRIT, "ERROR: Checksum fail! Incoming count: %ld ; local count: %ld\n", desc._checksum.count(), _checksum.count());
        abort();
    }

    // Append old revision sizes
    for (const auto& [pos, fSize, aSize] : _revisions_pos_fsize_asize) {
        push_obj<size_t>(_raw_data, fSize);
        push_obj<size_t>(_raw_data, aSize);
    }
    // Append new revision sizes
    for (int rev = firstNewRevision; rev <= _revision; rev++) {
        push_obj<size_t>(_raw_data, desc.getFormulaPayloadSize(rev));
        push_obj<size_t>(_raw_data, desc.getAssumptionsSize(rev));
    }
    // Set correct size and data of each revision
    for (int rev = firstNewRevision; rev <= _revision; rev++) {
        _revisions_pos_fsize_asize.push_back(
            RevisionInfo{pos, desc.getFormulaPayloadSize(rev), desc.getAssumptionsSize(rev)}
        );
        pos += desc.getFormulaPayloadSize(rev) * sizeof(int) + desc.getAssumptionsSize(rev) * sizeof(int);
    }

    // Rewrite meta data, reset payload + assumptions pointers
    writeMetadataAndPointers();
}

void JobDescription::clearPayload() {
    _raw_data.reset();
}
