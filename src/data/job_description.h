#ifndef DOMPASCH_CUCKOO_REBALANCER_JOB
#define DOMPASCH_CUCKOO_REBALANCER_JOB

#include <vector>
#include <cstring>

#include "serializable.h"

/**
 * The actual job structure, containing the full description.
 */
class JobDescription : public Serializable {

private:

    // Global meta data
    int id;
    int rootRank;
    float priority;

    // Payload (logic to solve)
    std::vector<int> formula; // if non-incremental / first job in stream
    std::vector<int> assumptions; // optional

public:

    JobDescription() : id(-1), rootRank(-1), priority(-1) {}
    JobDescription(int id, float priority) : id(id), rootRank(-1), priority(priority) {}

    int getId() const {return id;};
    int getRootRank() const {return rootRank;};
    float getPriority() const {return priority;};
    int getFormulaSize() const {
        return formula.size() + 1;
    };
    const std::vector<int>& getFormula() const {return formula;};
    int getAssumptionsSize() const {
        return assumptions.size();
    }

    void setRootRank(int rootRank) {this->rootRank = rootRank;}
    void setFormula(const std::vector<int>& formula) {this->formula = formula;};

    void deserialize(const std::vector<int>& packed) override;
    std::vector<int> serialize() const override;
};

#endif /* end of include guard: DOMPASCH_CUCKOO_REBALANCER_JOB */
