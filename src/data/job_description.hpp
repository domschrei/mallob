
#ifndef DOMPASCH_MALLOB_JOB_DESCRIPTION_HPP
#define DOMPASCH_MALLOB_JOB_DESCRIPTION_HPP

#include <vector>
#include <cstring>
#include <memory>

#include "data/serializable.hpp"
#include "data/checksum.hpp"

typedef std::shared_ptr<std::vector<int>> VecPtr;

/**
 * The actual job structure, containing the full description.
 */
class JobDescription : public Serializable {

public:
    enum Application {SAT, DUMMY};

private:

    // Global meta data
    int _id;
    int _root_rank;
    float _priority = 1.0;
    bool _incremental;
    int _revision = -1;
    float _wallclock_limit = 0; // in seconds
    float _cpu_limit = 0; // in CPU seconds
    int _max_demand = 0;
    Application _application = SAT;

    Checksum _checksum;
    const bool _use_checksums = false;

    float _arrival; // only for introducing a job

    // Payload (logic to solve)
    int _num_vars = -1;
    
    size_t _f_size;
    size_t _a_size;
    
    // For each revision, the shared_ptr contains the full serialization
    // of this revision including all meta data of this object.
    std::vector<std::shared_ptr<std::vector<uint8_t>>> _data_per_revision;
    
    // Stores the position (in bytes) and size (in integers) of each revision's payload.
    struct RevisionInfo {
        int jobId;
        int revision;
        size_t fSize;
        size_t aSize;
    };

    // just for parsing
    std::vector<int> _preloaded_assumptions;

private:
    template <typename T>
    inline static void push_obj(std::shared_ptr<std::vector<uint8_t>>& vec, T x) {
        vec->resize(vec->size()+sizeof(T));
        memcpy(vec->data()+vec->size()-sizeof(T), &x, sizeof(T));
    }

public:

    JobDescription() = default;
    JobDescription(int id, float priority, bool incremental, bool computeChecksums = false) : _id(id), _root_rank(-1),
                _priority(priority), _incremental(incremental), _revision(0), 
                _use_checksums(computeChecksums) {}
    ~JobDescription() {}

    // Parse (initial) job description into this object

    void beginInitialization(int revision);
    void reserveSize(size_t size);
    inline void addLiteral(int lit) {
        // Push literal to raw data, update counter
        push_obj<int>(_data_per_revision[_revision], lit);
        _f_size++;
        if (_use_checksums) _checksum.combine(lit);
    }
    inline void addAssumption(int lit) {
        // Push literal to raw data, update counter
        push_obj<int>(_data_per_revision[_revision], lit);
        _a_size++;
        if (_use_checksums) _checksum.combine(-lit);
    }
    void endInitialization();
    void writeMetadata();

    // Add a further increment of the description into this object
    void applyUpdate(const std::shared_ptr<std::vector<uint8_t>>& packed);
    
    JobDescription& deserialize(const std::vector<uint8_t>& packed) override;
    JobDescription& deserialize(std::vector<uint8_t>&& packed);
    JobDescription& deserialize(const std::shared_ptr<std::vector<uint8_t>>& packed);
    void deserialize();

    int getId() const {return _id;}
    int getRootRank() const {return _root_rank;}
    float getPriority() const {return _priority;}
    int getRevision() const {return _revision;}
    float getWallclockLimit() const {return _wallclock_limit;}
    float getCpuLimit() const {return _cpu_limit;}
    int getMaxDemand() const {return _max_demand;}
    Application getApplication() const {return _application;}
    
    float getArrival() const {return _arrival;}
    bool isIncremental() const {return _incremental;}
    constexpr int getMetadataSize() const;
    
    size_t getFullNonincrementalTransferSize() const {return _data_per_revision[0]->size();}
    int getNumVars() {return _num_vars;}

    void setRootRank(int rootRank) {_root_rank = rootRank;}
    void setRevision(int revision) {_revision = revision;}
    void setWallclockLimit(float limit) {_wallclock_limit = limit;}
    void setCpuLimit(float limit) {_cpu_limit = limit;}
    void setMaxDemand(int maxDemand) {_max_demand = maxDemand;}
    void setNumVars(int numVars) {_num_vars = numVars;}
    void setArrival(float arrival) {_arrival = arrival;};
    void setApplication(Application app) {_application = app;}
    void setPreloadedAssumptions(std::vector<int>&& asmpt) {_preloaded_assumptions = std::move(asmpt);}

    Checksum getChecksum() const {return _checksum;}
    void setChecksum(const Checksum& checksum) {_checksum = checksum;}

    std::vector<uint8_t> serialize() const override;
    const std::shared_ptr<std::vector<uint8_t>>& getSerialization(int revision) const;
    void clearPayload(int revision);

    size_t getNumFormulaLiterals() const {return _f_size;}
    size_t getNumAssumptionLiterals() const {return _a_size;}

    size_t getFormulaPayloadSize(int revision) const;
    const int* getFormulaPayload(int revision) const;
    size_t getAssumptionsSize(int revision) const;
    const int* getAssumptionsPayload(int revision) const;
    
    size_t getTransferSize(int revision) const;
    
    static int readRevisionIndex(const std::vector<uint8_t>& serialized);

private:
    std::shared_ptr<std::vector<uint8_t>>& getRevisionData(int revision);
    const std::shared_ptr<std::vector<uint8_t>>& getRevisionData(int revision) const;
    int prepareRevision(const std::vector<uint8_t>& packed);
    
};

#endif
