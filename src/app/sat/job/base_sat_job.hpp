
#pragma once

#include "app/job.hpp"
#include "data/checksum.hpp"

class BaseSatJob : public Job {

public:
    BaseSatJob(const Parameters& params, const JobSetup& setup) : 
        Job(params, setup) {}
    virtual ~BaseSatJob() {}

    // Methods common to all BaseSatJob instances

    virtual bool isInitialized() = 0;
    
    virtual void prepareSharing(int maxSize) = 0;
    virtual bool hasPreparedSharing() = 0;
    virtual std::vector<int> getPreparedClauses(Checksum& checksum) = 0;
    virtual std::pair<int, int> getLastAdmittedClauseShare() = 0;

    virtual void filterSharing(std::vector<int>& clauses) = 0;
    virtual bool hasFilteredSharing() = 0;
    virtual std::vector<int> getLocalFilter() = 0;
    virtual void applyFilter(std::vector<int>& filter) = 0;
    
    virtual void digestSharingWithoutFilter(std::vector<int>& clauses) = 0;
    virtual void returnClauses(std::vector<int>& clauses) = 0;

    // Methods common to all Job instances

    virtual void appl_start() = 0;
    virtual void appl_suspend() = 0;
    virtual void appl_resume() = 0;
    virtual void appl_terminate() = 0;

    virtual int appl_solved() = 0;
    virtual JobResult&& appl_getResult() = 0;
    
    virtual void appl_communicate() = 0;
    virtual void appl_communicate(int source, int mpiTag, JobMessage& msg) = 0;
    
    virtual void appl_dumpStats() = 0;
    virtual bool appl_isDestructible() = 0;
    virtual void appl_memoryPanic() = 0;

private:
    float _compensation_factor = 1.0f;

public:
    // Helper methods

    float getCompensationFactor() const {
        return _compensation_factor;
    }
    void setSharingCompensationFactor(float compensationFactor) {
        _compensation_factor = compensationFactor;
    }

    size_t getBufferLimit(int numAggregatedNodes, MyMpi::BufferQueryMode mode) {
        if (mode == MyMpi::SELF) return _compensation_factor * _params.clauseBufferBaseSize();
        return _compensation_factor * MyMpi::getBinaryTreeBufferLimit(numAggregatedNodes, 
            _params.clauseBufferBaseSize(), _params.clauseBufferDiscountFactor(), mode);
    }

};
