
#ifndef DOMPASCH_MALLOB_JOB_AMENDMENT_H
#define DOMPASCH_MALLOB_JOB_AMENDMENT_H

#include <vector>
#include <memory>
#include <map>
#include <cstring>
#include <assert.h>

#include "data/serializable.h"
#include "util/console.h"

typedef std::shared_ptr<std::vector<int>> VecPtr;

class JobAmendment : public Serializable {

private:
    int _job_id;
    int _first_revision;
    int _last_revision;
    std::vector<VecPtr> _revision_clauses;
    std::vector<VecPtr> _revision_assumptions;

public:
    JobAmendment(int jobId) : _job_id(jobId), _first_revision(-1), _last_revision(-1) {}
    JobAmendment(int jobId, int firstRevision, VecPtr clauses, VecPtr assumptions) 
                : _job_id(jobId), _first_revision(firstRevision), _last_revision(firstRevision) {
        _revision_clauses.push_back(clauses);
        _revision_assumptions.push_back(assumptions);
    }
    JobAmendment(const std::vector<uint8_t>& serialized) {
        deserialize(serialized);
    }
    int getJobId() const {return _job_id;}
    int getFirstRevision() const {return _first_revision;}
    int getLastRevision() const {return _last_revision;}
    int getTransferSize() const {
        int size = 3*sizeof(int);
        for (int x = 0; x < _revision_clauses.size(); x++) {
            size += sizeof(int) * (1 + _revision_clauses[x]->size());
        }
        size += sizeof(int) * _revision_assumptions.back()->size();
        return size;
    }
    std::vector<int>& getRevisionClauses(int revision) {
        return *_revision_clauses[revision-_first_revision];
    }
    VecPtr& getRevisionClauseVecPtr(int revision) {
        return _revision_clauses[revision-_first_revision];
    }
    std::vector<int>& getCurrentAssumptions() {
        return *_revision_assumptions.back();
    }

    void bump(VecPtr clauses, VecPtr assumptions) {
        _last_revision++;
        if (_first_revision < 0) _first_revision = _last_revision;
        _revision_clauses.push_back(clauses);
        _revision_assumptions.push_back(assumptions);
    }
    void bump(JobAmendment& other) {
        assert(_last_revision+1 == other._first_revision);
        for (int rev = 0; rev < other._revision_clauses.size(); rev++) {
            bump(other._revision_clauses[rev], other._revision_assumptions[rev]);
        }
    }
    JobAmendment revisionRange(int first, int last) {
        assert(first >= _first_revision && last <= _last_revision);
        JobAmendment other(_job_id, first, _revision_clauses[first-_first_revision], _revision_assumptions[first-_first_revision]);
        for (int revision = first+1; revision <= last; revision++) {
            other.bump(_revision_clauses[revision-_first_revision], _revision_assumptions[revision-_first_revision]);
        }
        assert(other.getFirstRevision() == first && other.getLastRevision() == last 
            || Console::fail("(%i,%i) != (%i,%i)", other.getFirstRevision(), other.getLastRevision(), first, last));
        return other;
    }
    std::shared_ptr<std::vector<uint8_t>> serialize() const override {
        int size = getTransferSize();
        std::shared_ptr<std::vector<uint8_t>> packed = std::make_shared<std::vector<uint8_t>>(size);

        int i = 0, n;
        n = sizeof(int); memcpy(packed->data()+i, &_job_id, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &_first_revision, n); i += n;
        n = sizeof(int); memcpy(packed->data()+i, &_last_revision, n); i += n;
        assert(_revision_clauses.size() == _last_revision-_first_revision+1 
            || Console::fail("%i %i %i", _revision_clauses.size(), _first_revision, _last_revision));
        for (int x = 0; x < _revision_clauses.size(); x++) {
            const VecPtr& clauses = _revision_clauses[x];
            int clausesSize = clauses->size();
            n = sizeof(int); memcpy(packed->data()+i, &clausesSize, n); i += n;
            n = clauses->size() * sizeof(int); memcpy(packed->data()+i, clauses->data(), n); i += n;
        }
        n = _revision_assumptions.back()->size() * sizeof(int); memcpy(packed->data()+i, _revision_assumptions.back()->data(), n); i += n;
        return packed;
    }
    void deserialize(const std::vector<uint8_t>& packed) override {
        int i = 0, n;
        _revision_clauses.clear();
        _revision_assumptions.clear();
        n = sizeof(int); memcpy(&_job_id, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&_first_revision, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&_last_revision, packed.data()+i, n); i += n;
        for (int r = _first_revision; r <= _last_revision; r++) {
            // #clauses in this revision
            int clausesSize;
            n = sizeof(int); memcpy(&clausesSize, packed.data()+i, n); i += n;
            Console::log(Console::VVERB, "#%i rev. %i : %i clauses", _job_id, r, clausesSize);
            VecPtr clauses = std::make_shared<std::vector<int>>(clausesSize);
            n = clausesSize * sizeof(int); memcpy(clauses->data(), packed.data()+i, n); i += n;
            _revision_clauses.push_back(clauses);
            _revision_assumptions.push_back(std::make_shared<std::vector<int>>());

            for (int l : *clauses) Console::append(Console::VVERB, "%i ", l);
            Console::log(Console::VVERB, " ");
        }
        n = packed.size()-i; _revision_assumptions.back() = std::make_shared<std::vector<int>>(n/sizeof(int));
        memcpy(_revision_assumptions.back()->data(), packed.data()+i, n); i += n;
        Console::log(Console::VVERB, "#%i rev. %i : %i assumptions", _job_id, _last_revision, _revision_assumptions.back()->size());
    }
};

#endif