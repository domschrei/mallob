
#include <assert.h>

#include "job_description.h"
#include "util/console.h"

JobDescription::~JobDescription() {
    clearPayload();
}

int JobDescription::getTransferSize(bool allRevisions) const {
    int size = 4*sizeof(int)
            +sizeof(float)
            +sizeof(bool);
    for (int x = 0; x <= _revision; x++) {
        size += sizeof(int) * (1 + _payloads[x]->size());
        size += sizeof(int) * (1 + _assumptions[x]->size());
        if (!allRevisions) break;
    }
    return size;
}

int JobDescription::getTransferSize(int firstRevision, int lastRevision) const {
    int size = 3 * sizeof(int);
    for (int x = firstRevision; x <= lastRevision; x++) {
        size += sizeof(int) * (1 + _payloads[x]->size());
        size += sizeof(int) * (1 + _assumptions[x]->size());
    }
    return size;
}

std::shared_ptr<std::vector<uint8_t>> JobDescription::serialize() const {
    return serialize(true);
}

void JobDescription::deserialize(const std::vector<uint8_t>& packed) {

    int i = 0, n;

    // Basic data
    n = sizeof(int); memcpy(&_id, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&_root_rank, packed.data()+i, n); i += n;
    n = sizeof(float); memcpy(&_priority, packed.data()+i, n); i += n;
    n = sizeof(bool); memcpy(&_incremental, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&_num_vars, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&_revision, packed.data()+i, n); i += n;

    // Payload
    for (int r = 0; r <= _revision; r++) {
        readRevision(packed, i);
    }
}

std::shared_ptr<std::vector<uint8_t>> JobDescription::serialize(int firstRevision, int lastRevision) const {

    std::shared_ptr<std::vector<uint8_t>> packed = std::make_shared<std::vector<uint8_t>>(
            getTransferSize(firstRevision, lastRevision));

    int i = 0, n;
    n = sizeof(int); memcpy(packed->data()+i, &_id, n); i += n;
    n = sizeof(int); memcpy(packed->data()+i, &firstRevision, n); i += n;
    n = sizeof(int); memcpy(packed->data()+i, &lastRevision, n); i += n;

    // Payload
    for (int r = firstRevision; r <= lastRevision; r++) {
        writeRevision(r, *packed, i);
    }

    return packed;
}

std::shared_ptr<std::vector<uint8_t>> JobDescription::serializeFirstRevision() const {
    return serialize(false);
}

std::shared_ptr<std::vector<uint8_t>> JobDescription::serialize(bool allRevisions) const {

    std::shared_ptr<std::vector<uint8_t>> packed = std::make_shared<std::vector<uint8_t>>(getTransferSize(allRevisions));

    // Basic data
    int i = 0, n;
    n = sizeof(int); memcpy(packed->data()+i, &_id, n); i += n;
    n = sizeof(int); memcpy(packed->data()+i, &_root_rank, n); i += n;
    n = sizeof(float); memcpy(packed->data()+i, &_priority, n); i += n;
    n = sizeof(bool); memcpy(packed->data()+i, &_incremental, n); i += n;
    n = sizeof(int); memcpy(packed->data()+i, &_num_vars, n); i += n;
    int rev = allRevisions ? _revision : 0;
    n = sizeof(int); memcpy(packed->data()+i, &rev, n); i += n;

    // Payload
    for (int r = 0; r <= _revision; r++) {
        writeRevision(r, *packed, i);
        if (!allRevisions) break;
    }

    return packed;
}

void JobDescription::clearPayload() {
    for (auto vec : _payloads) {
        vec.reset();
        vec = NULL;
    }
    for (auto vec : _assumptions) {
        vec.reset();
        vec = NULL;
    }
    _payloads.clear();
    _assumptions.clear();
}

void JobDescription::merge(const std::vector<uint8_t>& packed) {

    int i = 0, n;
    int id, firstRevision, lastRevision;
    n = sizeof(int); memcpy(&id, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&firstRevision, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&lastRevision, packed.data()+i, n); i += n;

    assert(id == _id);
    assert(firstRevision == _revision+1);

    for (int r = firstRevision; r <= lastRevision; r++) {
        readRevision(packed, i);
    }
    _revision = lastRevision;
}

void JobDescription::readRevision(const std::vector<uint8_t>& src, int& i) {
    int n;
    
    int clausesSize;
    n = sizeof(int); memcpy(&clausesSize, src.data()+i, n); i += n;
    VecPtr payload = std::make_shared<std::vector<int>>(clausesSize);
    n = clausesSize * sizeof(int); memcpy(payload->data(), src.data()+i, n); i += n;
    _payloads.push_back(payload);

    int asmptSize;
    n = sizeof(int); memcpy(&asmptSize, src.data()+i, n); i += n;
    VecPtr assumptions = std::make_shared<std::vector<int>>(asmptSize);
    n = asmptSize * sizeof(int); memcpy(assumptions->data(), src.data()+i, n); i += n;
    _assumptions.push_back(assumptions);
}

void JobDescription::writeRevision(int revision, std::vector<uint8_t>& dest, int& i) const {
    int n;

    const VecPtr& clauses = _payloads[revision];
    const VecPtr& assumptions = _assumptions[revision];

    int clausesSize = clauses->size();
    n = sizeof(int); memcpy(dest.data()+i, &clausesSize, n); i += n;
    n = clausesSize * sizeof(int); memcpy(dest.data()+i, clauses->data(), n); i += n;

    int asmptSize = assumptions->size();
    n = sizeof(int); memcpy(dest.data()+i, &asmptSize, n); i += n;
    n = asmptSize * sizeof(int); memcpy(dest.data()+i, assumptions->data(), n); i += n;
}

const std::vector<VecPtr> JobDescription::getPayloads(int firstRevision, int lastRevision) const {

    std::vector<VecPtr> payloads;
    for (int r = firstRevision; r <= lastRevision; r++) {
        payloads.push_back(_payloads[r]);
    }
    return payloads;
}