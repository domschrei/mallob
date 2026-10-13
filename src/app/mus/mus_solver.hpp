//
// Created by nicco on 10/13/26.
//

#ifndef MALLOB_MUS_SOLVER_HPP
#define MALLOB_MUS_SOLVER_HPP


#include "app/app_terminate_checker.hpp"
#include "app/maxsat/maxsat_instance.hpp"
#include "app/maxsat/maxsat_search_procedure.hpp"
#include "app/maxsat/parse/maxsat_reader.hpp"
#include "app/sat/data/definitions.hpp"
#include "app/sat/job/sat_constants.h"
#include "comm/msg_queue/message_subscription.hpp"
#include "comm/msgtags.h"
#include "comm/mympi.hpp"
#include "core/dtask_tracker.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_transfer.hpp"
#include "interface/api/api_connector.hpp"
#include <algorithm>
#include <climits>
#include <list>
#include <memory>
#include <unistd.h>
#include "robin_set.h"
#include "util/logger.hpp"
#include "util/sys/terminator.hpp"
#include "util/params.hpp"
#include "util/sys/watchdog.hpp"

class MusSolver {
private:
    const Parameters _params; // configuration, cmd line arguments
    APIConnector& _api; // for submitting jobs to Mallob
    JobDescription& _desc; // contains our instance to solve and all metadata
    AppTerminateChecker _term;

    MessageSubscription _sub_xtcs_incoming_clauses;
    Mutex _mtx_incoming_messages;
    std::list<MessageHandle> _incoming_messages;

public:
    // Initializes the solver instance and parses the description's formula.
    MusSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc), _term(_params, _desc),
        _sub_xtcs_incoming_clauses(MSG_SEND_APP_DATA_TO_CLIENT_JOB, [&](MessageHandle& h) {
            auto lock = _mtx_incoming_messages.getLock();
            _incoming_messages.push_back(h);
        }) {

        LOG(V2_INFO, "Mallob client-side MUS solver, by Niccolò Rigi-Luperti & Dominik Schreiber\n");
        // parseFormula();
        // pickEncodingStrategy();
    }
    MusSolver(MusSolver&& other) = delete;
    ~MusSolver() {}


    JobResult solve(int updateLayer = 0) {
        JobResult r;
        r.id = _desc.getId();
        r.revision = 0;
        r.result = RESULT_UNKNOWN;
        LOG(V2_INFO, "MUS solve()\n");
        return r;
    }
};




#endif //MALLOB_MUS_SOLVER_HPP