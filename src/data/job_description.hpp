#ifndef DOMPASCH_CUCKOO_REBALANCER_JOB
#define DOMPASCH_CUCKOO_REBALANCER_JOB

#include <vector>
#include <cstring>
#include <memory>

#include "data/serializable.hpp"
#include "data/checksum.hpp"

typedef std::shared_ptr<std::vector<int>> VecPtr;

/**
 * The actual job structure, containing the full description.
 * JobDescription::_raw_data contains the complete information on an object in serialized form.
 * It is structured as follows:
 * [ meta data ] [ payload size ] [ num. assumptions ] [ payload ] [ assumptions ] [ size of each revision ]
 * If a new revision arrives, a cut is made like this:
 * [ meta data ] [ payload size ] [ num. assumptions ] [ payload ] || --> cut off: [ assumptions ] [ size of each revision ]
 * and the new revision/s is/are appended:
 * [ meta data ] [ payload size + new payload size ] [ num. assumptions' ] [ payload ] [ payload' ] <-- [ assumptions' ] [ size of each revision ] [ size of each new revision ]
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
    int _first_revision = -1;
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
    
    // Contains THE ENTIRE OBJECT and all payload / assumptions in serialized form.
    std::shared_ptr<std::vector<uint8_t>> _raw_data;
    
    // Stores the position (in bytes) and size (in integers) of each revision's payload.
    struct RevisionInfo {
        size_t pos;
        size_t fSize;
        size_t aSize;
    };
    std::vector<RevisionInfo> _revisions_pos_fsize_asize;

private:
    template <typename T>
    inline static void push_obj(std::shared_ptr<std::vector<uint8_t>>& vec, T x) {
        vec->resize(vec->size()+sizeof(T));
        memcpy(vec->data()+vec->size()-sizeof(T), &x, sizeof(T));
    }

public:

    JobDescription() = default;
    JobDescription(int id, float priority, bool incremental, bool computeChecksums = false) : _id(id), _root_rank(-1),
                _priority(priority), _incremental(incremental), _first_revision(0), _revision(0), 
                _use_checksums(computeChecksums) {}
    ~JobDescription() {}

    // Parse (initial) job description into this object

    void beginInitialization();
    void reserveSize(size_t size);
    inline void addLiteral(int lit) {
        // Push literal to raw data, update counter
        push_obj<int>(_raw_data, lit);
        _f_size++;
        if (_use_checksums) _checksum.combine(lit);
    }
    inline void addAssumption(int lit) {
        // Push literal to raw data, update counter
        push_obj<int>(_raw_data, lit);
        _a_size++;
        if (_use_checksums) _checksum.combine(-lit);
    }
    void endInitialization();
    // Returns the index in _raw_data after the written meta data.
    int writeMetadataAndPointers();

    // Add a further increment of the description into this object
    void applyUpdate(const std::shared_ptr<std::vector<uint8_t>>& packed);
    
    JobDescription& deserialize(const std::vector<uint8_t>& packed) override;
    JobDescription& deserialize(std::vector<uint8_t>&& packed);
    JobDescription& deserialize(const std::shared_ptr<std::vector<uint8_t>>& packed);
    void deserialize();

    int getId() const {return _id;}
    int getRootRank() const {return _root_rank;}
    float getPriority() const {return _priority;}
    int getFirstRevision() const {return _first_revision;}
    int getRevision() const {return _revision;}
    float getWallclockLimit() const {return _wallclock_limit;}
    float getCpuLimit() const {return _cpu_limit;}
    int getMaxDemand() const {return _max_demand;}
    Application getApplication() const {return _application;}
    
    float getArrival() const {return _arrival;}
    bool isIncremental() const {return _incremental;}
    constexpr int getMetadataSize() const;
    size_t getFullTransferSize() const {return _raw_data->size();}
    int getNumVars() {return _num_vars;}

    void setRootRank(int rootRank) {_root_rank = rootRank;}
    void setFirstRevision(int revision) {_first_revision = revision;}
    void setRevision(int revision) {_revision = revision;}
    void setWallclockLimit(float limit) {_wallclock_limit = limit;}
    void setCpuLimit(float limit) {_cpu_limit = limit;}
    void setMaxDemand(int maxDemand) {_max_demand = maxDemand;}
    void setNumVars(int numVars) {_num_vars = numVars;}
    void setArrival(float arrival) {_arrival = arrival;};
    void setApplication(Application app) {_application = app;}
    void clearPayload();

    Checksum getChecksum() const {return _checksum;}
    void setChecksum(const Checksum& checksum) {_checksum = checksum;}

    std::vector<uint8_t> serialize() const override;
    const std::shared_ptr<std::vector<uint8_t>>& getSerialization();

    size_t getNumFormulaLiterals() const {return _f_size;}
    size_t getNumAssumptionLiterals() const {return _a_size;}
    size_t getFormulaPayloadSize(int revision) const;
    const int* getFormulaPayload(int revision) const;
    size_t getAssumptionsSize(int revision) const;
    const int* getAssumptionsPayload(int revision) const;
    size_t getTransferSize(int firstIncludedRevision) const;
    std::shared_ptr<std::vector<uint8_t>> extractUpdate(int firstIncludedRevision) const;

};

#endif /* end of include guard: DOMPASCH_CUCKOO_REBALANCER_JOB */
