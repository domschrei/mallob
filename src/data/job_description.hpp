
#ifndef DOMPASCH_MALLOB_JOB_DESCRIPTION_HPP
#define DOMPASCH_MALLOB_JOB_DESCRIPTION_HPP

#include <vector>
#include <cstring>
#include <memory>

#include "data/serializable.hpp"
#include "data/checksum.hpp"
#include "data/app_configuration.hpp"

typedef std::shared_ptr<std::vector<int>> VecPtr;

/**
 * The actual job structure, containing the full description.
 */
class JobDescription : public Serializable {

public:
    struct Statistics {
        float timeOfScheduling;
        float parseTime;
        float schedulingTime;
        float processingTime;
        float usedWallclockSeconds;
        float usedCpuSeconds;
        float latencyOf1stVolumeUpdate;
    };

private:

    // Global meta data
    int _id;
    int _root_rank;
    float _priority = 1.0;
    int _revision = -1;
    int _client_rank = -1;
    float _wallclock_limit = 0; // in seconds
    float _cpu_limit = 0; // in CPU seconds
    int _max_demand = 0;
    int _application_id; // see app_registry
    bool _incremental = false;
    int _group_id {0};

    Checksum _checksum;
    const bool _use_checksums = false;

    float _arrival; // only for introducing a job

    // configuration options
    AppConfiguration _app_config;

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
    std::vector<int> _preloaded_literals;
    std::vector<int> _preloaded_assumptions;

    // just for scheduling
    Statistics* _stats = nullptr;

private:
    template <typename T>
    inline static void push_obj(std::shared_ptr<std::vector<uint8_t>>& vec, T x) {
        vec->resize(vec->size()+sizeof(T));
        memcpy(vec->data()+vec->size()-sizeof(T), &x, sizeof(T));
    }

public:

    JobDescription() = default;
    JobDescription(int id, float priority, int applicationId, bool computeChecksums = false) : _id(id), _root_rank(-1),
                _priority(priority), _application_id(applicationId), _revision(0), 
                _use_checksums(computeChecksums) {}
    ~JobDescription() {
        if (_stats != nullptr) delete _stats;
        for (auto& data : _data_per_revision)
            data.reset();
    }

    // Moving job descriptions is okay
    JobDescription& operator=(JobDescription&& other) {
        _id = other._id;
        _root_rank = other._root_rank;
        _priority = std::move(other._priority);
        _incremental = std::move(other._incremental);
        _group_id = std::move(other._group_id);
        _revision = std::move(other._revision);
        _client_rank = std::move(other._client_rank);
        _wallclock_limit = std::move(other._wallclock_limit);
        _cpu_limit = std::move(other._cpu_limit);
        _max_demand = std::move(other._max_demand);
        _application_id = std::move(other._application_id);
        _checksum = std::move(other._checksum);
        _arrival = std::move(other._arrival);
        _app_config = std::move(other._app_config);
        _num_vars = std::move(other._num_vars);
        _f_size = std::move(other._f_size);
        _a_size = std::move(other._a_size);
        _data_per_revision = std::move(other._data_per_revision);
        _preloaded_literals = std::move(other._preloaded_literals);
        _preloaded_assumptions = std::move(other._preloaded_assumptions);
        _stats = std::move(other._stats);
        other._id = -1;
        other._data_per_revision.clear();
        other._stats = nullptr;
        return *this;
    }
    JobDescription(JobDescription&& other) {
        *this = std::move(other);
    }
    
    // Copying is NOT okay
    JobDescription(const JobDescription& other) = delete;
    JobDescription& operator=(const JobDescription& other) = delete;


    // Parse (initial) job description into this object

    void beginInitialization(int revision);
    void reserveSize(size_t size);
    inline void addPermanentData(int lit) {
        // Push literal to raw data, update counter
        push_obj<int>(_data_per_revision[_revision], lit);
        _f_size++;
        if (_use_checksums) _checksum.combine(lit);
    }
    inline void addPermanentData(float data) {
        static_assert(sizeof(float) == sizeof(int));
        push_obj<float>(_data_per_revision[_revision], data);
        _f_size++;
        if (_use_checksums) _checksum.combine(data);
    }

    inline void addTransientData(int lit) {
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
    int getClientRank() const {return _client_rank;}
    float getWallclockLimit() const {return _wallclock_limit;}
    float getCpuLimit() const {return _cpu_limit;}
    int getMaxDemand() const {return _max_demand;}
    int getApplicationId() const {return _application_id;}
    const AppConfiguration& getAppConfiguration() const {return _app_config;}
    
    float getArrival() const {return _arrival;}
    void setIncremental(bool incremental) {_incremental = incremental;}
    bool isIncremental() const {return _incremental;}
    int getGroupId() const {return _group_id;}
    int getMetadataSize() const;
    
    size_t getFullNonincrementalTransferSize() const {return _data_per_revision[0]->size();}
    int getNumVars() {return _num_vars;}

    void setRootRank(int rootRank) {_root_rank = rootRank;}
    void setRevision(int revision) {_revision = revision;}
    void setClientRank(int clientRank) {_client_rank = clientRank;}
    void setWallclockLimit(float limit) {_wallclock_limit = limit;}
    void setCpuLimit(float limit) {_cpu_limit = limit;}
    void setMaxDemand(int maxDemand) {_max_demand = maxDemand;}
    void setNumVars(int numVars) {_num_vars = numVars;}
    void setArrival(float arrival) {_arrival = arrival;};
    void setAppConfiguration(AppConfiguration&& appConfig) {_app_config = std::move(appConfig);}
    void setPreloadedLiterals(std::vector<int>&& lits) {_preloaded_literals = std::move(lits);}
    void setPreloadedAssumptions(std::vector<int>&& asmpt) {_preloaded_assumptions = std::move(asmpt);}
    void setGroupId(int groupId) {_group_id = groupId;}

    Checksum getChecksum() const {return _checksum;}
    void setChecksum(const Checksum& checksum) {_checksum = checksum;}

    std::vector<uint8_t> serialize() const override;
    const std::shared_ptr<std::vector<uint8_t>>& getSerialization(int revision) const;
    void clearPayload(int revision);

    int getMaxConsecutiveRevision() const;

    size_t getNumFormulaLiterals() const {return _f_size;}
    size_t getNumAssumptionLiterals() const {return _a_size;}

    size_t getFormulaPayloadSize(int revision) const;
    const int* getFormulaPayload(int revision) const;
    size_t getAssumptionsSize(int revision) const;
    const int* getAssumptionsPayload(int revision) const;
    
    size_t getTransferSize(int revision) const;
    
    static int readRevisionIndex(const std::vector<uint8_t>& serialized);

    Statistics& getStatistics() {
        if (_stats == nullptr) _stats = new Statistics();
        return *_stats;
    }

private:
    std::shared_ptr<std::vector<uint8_t>>& getRevisionData(int revision);
    const std::shared_ptr<std::vector<uint8_t>>& getRevisionData(int revision) const;
    int prepareRevision(const std::vector<uint8_t>& packed);
    
};

#endif
