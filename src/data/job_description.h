#ifndef DOMPASCH_CUCKOO_REBALANCER_JOB
#define DOMPASCH_CUCKOO_REBALANCER_JOB

#include <vector>
#include <cstring>

#include "serializable.h"

typedef std::shared_ptr<std::vector<int>> VecPtr;

/**
 * The actual job structure, containing the full description.
 */
class JobDescription : public Serializable {

private:

    // Global meta data
    int id;
    int rootRank;
    float priority;
    bool incremental;
    int revision;

    float arrival; // only for introducing a job

    // Payload (logic to solve)
    std::vector<VecPtr> _payloads;
    std::vector<VecPtr> _assumptions;
    VecPtr nullVec = NULL;

public:

    JobDescription() = default;
    JobDescription(int id, float priority, bool incremental) : id(id), rootRank(-1),
                priority(priority), incremental(incremental), revision(0) {}
    ~JobDescription();

    int getId() const {return id;}
    int getRootRank() const {return rootRank;}
    float getPriority() const {return priority;}
    int getRevision() const {return revision;}
    const std::vector<VecPtr>& getPayloads() const {return _payloads;}
    const std::vector<VecPtr>& getAssumptions() const {return _assumptions;}
    const VecPtr& getPayload(int revision) const {return revision >= _payloads.size() ? nullVec : _payloads[revision];}
    const VecPtr& getAssumptions(int revision) const {return revision >= _assumptions.size() ? nullVec : _assumptions[revision];}
    float getArrival() const {return arrival;}
    bool isIncremental() const {return incremental;}
    int getTransferSize(bool allRevisions) const;
    int getTransferSize(int firstRevision, int lastRevision) const;
    const std::vector<VecPtr> getPayloads(int firstRevision, int lastRevision) const;

    void setRootRank(int rootRank) {this->rootRank = rootRank;}
    void setRevision(int revision) {this->revision = revision;}
    void addPayload(const VecPtr& payload) {_payloads.push_back(payload);}
    void addAssumptions(const VecPtr& assumptions) {_assumptions.push_back(assumptions);}
    void setArrival(float arrival) {this->arrival = arrival;};
    void clearPayload();

    std::shared_ptr<std::vector<uint8_t>> serialize() const override;
    void deserialize(const std::vector<uint8_t>& packed) override;

    std::shared_ptr<std::vector<uint8_t>> serialize(int firstRevision, int lastRevision) const;
    void merge(const std::vector<uint8_t>& packed);

    std::shared_ptr<std::vector<uint8_t>> serializeFirstRevision() const;

private:
    std::shared_ptr<std::vector<uint8_t>> serialize(bool allRevisions) const;
    void writeRevision(int revision, std::vector<uint8_t>& dest, int& i) const;
    void readRevision(const std::vector<uint8_t>& src, int& i);
};

#endif /* end of include guard: DOMPASCH_CUCKOO_REBALANCER_JOB */
