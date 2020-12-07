#ifndef DOMPASCH_CUCKOO_REBALANCER_JOB
#define DOMPASCH_CUCKOO_REBALANCER_JOB

#include <vector>
#include <cstring>

#include "serializable.hpp"

typedef std::shared_ptr<std::vector<int>> VecPtr;

/**
 * The actual job structure, containing the full description.
 */
class JobDescription : public Serializable {

private:

    // Global meta data
    int _id;
    int _root_rank;
    float _priority = 1.0;
    bool _incremental;
    int _revision;
    float _wallclock_limit = 0; // in seconds
    float _cpu_limit = 0; // in CPU seconds

    float _arrival; // only for introducing a job

    // Payload (logic to solve)
    int _num_vars = -1;
    std::vector<VecPtr> _payloads;
    std::vector<VecPtr> _assumptions;
    VecPtr nullVec = nullptr;

public:

    JobDescription() = default;
    JobDescription(int id, float priority, bool incremental) : _id(id), _root_rank(-1),
                _priority(priority), _incremental(incremental), _revision(0) {}
    ~JobDescription();

    int getId() const {return _id;}
    int getRootRank() const {return _root_rank;}
    float getPriority() const {return _priority;}
    int getRevision() const {return _revision;}
    float getWallclockLimit() const {return _wallclock_limit;}
    float getCpuLimit() const {return _cpu_limit;}
    const std::vector<VecPtr>& getPayloads() const {return _payloads;}
    const std::vector<VecPtr>& getAssumptions() const {return _assumptions;}
    const VecPtr& getPayload(size_t revision) const {return revision >= _payloads.size() ? nullVec : _payloads[revision];}
    const VecPtr& getAssumptions(size_t revision) const {return revision >= _assumptions.size() ? nullVec : _assumptions[revision];}
    float getArrival() const {return _arrival;}
    bool isIncremental() const {return _incremental;}
    int getTransferSize(bool allRevisions) const;
    int getTransferSize(int firstRevision, int lastRevision) const;
    const std::vector<VecPtr> getPayloads(int firstRevision, int lastRevision) const;
    int getNumVars() {return _num_vars;}

    void setRootRank(int rootRank) {_root_rank = rootRank;}
    void setRevision(int revision) {_revision = revision;}
    void setWallclockLimit(float limit) {_wallclock_limit = limit;}
    void setCpuLimit(float limit) {_cpu_limit = limit;}
    void setNumVars(int numVars) {_num_vars = numVars;}
    void addPayload(const VecPtr& payload) {_payloads.push_back(payload);}
    void addAssumptions(const VecPtr& assumptions) {_assumptions.push_back(assumptions);}
    void setArrival(float arrival) {_arrival = arrival;};
    void clearPayload();

    std::shared_ptr<std::vector<uint8_t>> serialize() const override;
    JobDescription& deserialize(const std::vector<uint8_t>& packed) override;

    std::shared_ptr<std::vector<uint8_t>> serialize(int firstRevision, int lastRevision) const;
    void merge(const std::vector<uint8_t>& packed);

    std::shared_ptr<std::vector<uint8_t>> serializeFirstRevision() const;

private:
    std::shared_ptr<std::vector<uint8_t>> serialize(bool allRevisions) const;
    void writeRevision(int revision, std::vector<uint8_t>& dest, int& i) const;
    void readRevision(const std::vector<uint8_t>& src, int& i);
};

#endif /* end of include guard: DOMPASCH_CUCKOO_REBALANCER_JOB */
