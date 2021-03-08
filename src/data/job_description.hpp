#ifndef DOMPASCH_CUCKOO_REBALANCER_JOB
#define DOMPASCH_CUCKOO_REBALANCER_JOB

#include <vector>
#include <cstring>

#include "data/serializable.hpp"

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
    int _revision = -1;
    float _wallclock_limit = 0; // in seconds
    float _cpu_limit = 0; // in CPU seconds

    float _arrival; // only for introducing a job

    // Payload (logic to solve)
    int _num_vars = -1;
    
    size_t _f_size;
    size_t _a_size;
    
    // Contains THE ENTIRE OBJECT and all payload / assumptions in serialized form.
    std::shared_ptr<std::vector<uint8_t>> _raw_data;
   
    const int* _f_payload;
    const int* _a_payload;

private:
    inline static void push_int(std::shared_ptr<std::vector<uint8_t>>& vec, int x) {
        vec->resize(vec->size()+sizeof(int));
        memcpy(vec->data()+vec->size()-sizeof(int), &x, sizeof(int));
    }

public:

    JobDescription() = default;
    JobDescription(int id, float priority, bool incremental) : _id(id), _root_rank(-1),
                _priority(priority), _incremental(incremental), _revision(0) {}
    ~JobDescription() {}

    void beginInitialization();
    void reserveSize(size_t size);
    inline void addLiteral(int lit) {
        // Push literal to raw data, update counter
        push_int(_raw_data, lit);
        _f_size++;
    }
    inline void addAssumption(int lit) {
        // Push literal to raw data, update counter
        push_int(_raw_data, lit);
        _a_size++;
    }
    void endInitialization();
    
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
    
    size_t getFormulaSize() const {return _f_size;}
    const int* getFormulaPayload() const {return _f_payload;}
    size_t getAssumptionsSize() const {return _a_size;}
    const int* getAssumptionsPayload() const {return _a_payload;}
    
    float getArrival() const {return _arrival;}
    bool isIncremental() const {return _incremental;}
    constexpr int getMetadataSize() const;
    int getFullTransferSize() const {return _raw_data->size();}
    int getNumVars() {return _num_vars;}

    void setRootRank(int rootRank) {_root_rank = rootRank;}
    void setRevision(int revision) {_revision = revision;}
    void setWallclockLimit(float limit) {_wallclock_limit = limit;}
    void setCpuLimit(float limit) {_cpu_limit = limit;}
    void setNumVars(int numVars) {_num_vars = numVars;}
    void setArrival(float arrival) {_arrival = arrival;};
    void clearPayload();

    std::vector<uint8_t> serialize() const override;
    const std::shared_ptr<std::vector<uint8_t>>& getSerialization();

};

#endif /* end of include guard: DOMPASCH_CUCKOO_REBALANCER_JOB */
