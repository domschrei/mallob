#ifndef MSCHICK_FAILED_ASSUMPTION_COMMUNICATOR_H
#define MSCHICK_FAILED_ASSUMPTION_COMMUNICATOR_H

#include <unordered_set>

#include "../hordesat/utilities/clause_filter.hpp"
#include "dynamic_cube_sat_job.hpp"

const int MSG_FAILED_ASSUMPTION_GATHER = 2048;
const int MSG_FAILED_ASSUMPTION_DISTRIBUTE = 2049;

const int MSG_FAILED_ASSUMPTION_SEND_TO_ROOT = 2050;

class FailedAssumptionCommunicator {
   private:
    // The correspondig job instance
    DynamicCubeSatJob &_job;

    LoggingInterface &_logger;

    // Counts how many messages were received since last send
    // Is reset after every sendMessageToParent
    int _messageCounter = 0;

    // Accumulated received failed assumptions for reduction
    std::vector<int> _received_failed_assumptions;

    // Filter for received failed assumption
    std::unordered_set<std::vector<int>, ClauseFilter::ClauseHasher> _clause_filter;

    // New clauses that were learnt in the last call of persist
    std::vector<int> _new_clauses;

    // Root member
    // Start index from all clauses for distribute
    int _distribute_start_index = 0;

    // Root member
    // Storing all clauses from the clause filter in a serial form
    std::vector<int> _all_clauses;

    // Add the failed assumptions to the local filter and add them to the corresponding lib
    void persist(std::vector<int> &failed_assumptions);

    void startDistribute();

    // Send the failed assumptions to all children
    void distribute(std::vector<int> &failed_assumptions);

    void sendToRoot(std::vector<int> &failed_assumptions);

    void log_send(int destRank, std::vector<int> &payload, const char *str, ...);
    static std::string payloadToString(std::vector<int> &payload);

   public:
    FailedAssumptionCommunicator(DynamicCubeSatJob &_job, LoggingInterface &logger);

    void gather();

    void handle(int source, JobMessage &msg);

    void release();

    static bool isFailedAssumptionMessage(int tag);
};

#endif /* MSCHICK_FAILED_ASSUMPTION_COMMUNICATOR_H */