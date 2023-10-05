#include "qbf_job.hpp"
#include "app/qbf/execution/qbf_context_store.hpp"
#include "app/qbf/execution/qbf_notification.hpp"
#include "app/qbf/execution/qbf_ready_msg.hpp"
#include "app/qbf/options.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "comm/msgtags.h"
#include "data/app_configuration.hpp"
#include "qbf_context.hpp"
#include "app/sat/job/sat_constants.h"
#include "util/logger.hpp"
#include "bloqqer_caller.hpp"
#include "util/str_util.hpp"

void dbg_output_file(const int* data, size_t size, const std::string& outfile) {
    std::ofstream ofs(outfile);
    for (size_t i = 0; i < size; i++)
        ofs << data[i] << " ";
    ofs << std::endl;
}




QbfJob::QbfJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table) 
    : Job(params, setup, table), _job_log(Logger::getMainInstance().copy(
        "<" + std::string(toStr()) + ">",
        ".qbfjob." + std::to_string(getId())
                                                                         )) {

    // Just initializes this job context -- job description is NOT present yet!
    LOG(V3_VERB, "QBF Initialized job #%i\n", getId());
}

QbfJob::~QbfJob() {
    _bg_worker.stop();
}

void QbfJob::appl_start() {

    // The job description is present now.
    _bg_worker_done = false;
    _bg_worker.run([&]() {run();});
}

void QbfJob::appl_suspend() {}

void QbfJob::appl_resume() {}

void QbfJob::appl_terminate() {
    _bg_worker.stopWithoutWaiting();
}

int QbfJob::appl_solved() {
    return _bg_worker_done ? 0 : -1;
}

JobResult&& QbfJob::appl_getResult() {
    assert(_internal_result.id >= 1);
    return std::move(_internal_result);
}

void QbfJob::appl_communicate() {

    if (_initialized && !_sent_ready_msg_to_parent) {
        // Send a "ready" notification to your parent
        // so that it knows your address
        auto ctx = QbfContextStore::tryAcquire(getId());
        if (!ctx) return;
        int parentRank = ctx->parentRank;
        if (parentRank != -1) {
            LOG(V3_VERB, "Reporting ready QBF sub-job #%i (childidx=%i) to parent job [%i]\n", getId(), ctx->childIdx, parentRank);
            MyMpi::isend(parentRank, MSG_NOTIFY_JOB_READY,
                SubjobReadyMsg(ctx->rootJobId, ctx->parentJobId, ctx->childIdx, ctx->nodeJobId));
        }
        _sent_ready_msg_to_parent = true;
    }

    // Send messages from outgoing messages queue (if possible)
    if (!_mtx_out_msg_queue.tryLock()) return;
    processOutMessageQueue();
    _mtx_out_msg_queue.unlock();
}

void QbfJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {}

void QbfJob::appl_dumpStats() {}

bool QbfJob::appl_isDestructible() {
    if (!_bg_worker_done) return false;
    auto lock = _mtx_out_msg_queue.getLock();
    processOutMessageQueue();
    return _out_msg_queue.empty();
}
void QbfJob::appl_memoryPanic() {}

int QbfJob::getDemand() const {
    return 1; // for a default QBF job, we only want a single worker!
}

// Separate thread: We can do long tasks here,
// but should frequently check if the job is suspended
// (via appl_suspend) and resumed (via appl_resume) again.
// We should also quickly stop when appl_terminate is called,
// which means that we should check _bg_worker.continueRunning()
// frequently.
// We should also try to not block the context object without
// requirement.
void QbfJob::run() {

    // Extract meta data for this particular job node
    // from the AppConfig which is part of the job description
    QbfContextStore::create(getId(), buildQbfContextFromAppConfig());

    {
        auto ctx = QbfContextStore::tryAcquire(getId());
        assert(ctx);
        LOGGER(_job_log, V3_VERB, "QBF #%i - parent #%i depth %i\n", getId(), ctx->parentJobId, ctx->depth);
        // Install a callback for incoming messages of the tag MSG_QBF_NOTIFICATION_UPWARDS.
        // CAUTION: Callback may be executed AFTER the life time of this job instance!
        installMessageListeners(*ctx);

        // Allow the main thread to send a "ready" notification to this job's parent.
        _initialized = true;
    }

    {
        auto ctx = QbfContextStore::tryAcquire(getId());
        assert(ctx);
        if (ctx->cancelled) {
            markDone(*ctx, true);
            return;
        }
    }


    // Prepare formula data

    auto data = getDescription().getFormulaPayload(0);
    auto dataSize = getDescription().getFormulaPayloadSize(0);

    //dbg_output_file(data, dataSize, "tmp-payload-jobid-" + std::to_string(getId()));

    _idx_begin_formula_prefix = _idx_begin_var_ordering;
    while (data[_idx_begin_formula_prefix++] != 0) {}
    _idx_begin_formula_matrix = _idx_begin_formula_prefix;
    while (data[_idx_begin_formula_matrix++] != 0) {}

    LOGGER(_job_log, V3_VERB, "QBF #%i varorder@%lu prefix@%lu matrix@%lu\n", getId(),
        _idx_begin_var_ordering, _idx_begin_formula_prefix, _idx_begin_formula_matrix);

    //dbg_output_file(data+_idx_begin_var_ordering, _idx_begin_formula_prefix-_idx_begin_var_ordering,
    //    "tmp-varorder-jobid-" + std::to_string(getId()));
    //dbg_output_file(data+_idx_begin_formula_prefix, _idx_begin_formula_matrix-_idx_begin_formula_prefix,
    //    "tmp-prefix-jobid-" + std::to_string(getId()));
    //dbg_output_file(data+_idx_begin_formula_matrix, dataSize-_idx_begin_formula_matrix,
    //    "tmp-matrix-jobid-" + std::to_string(getId()));

    assert(data[_idx_begin_formula_prefix-1] == 0 && data[_idx_begin_formula_prefix] != 0);
    assert(data[_idx_begin_formula_matrix-1] == 0 && data[_idx_begin_formula_matrix] != 0);


    // Apply our splitting strategy (may be expensive).
    auto maybeSplit = applySplittingStrategy();

    bool cleanup = false;
    {
        auto ctx = QbfContextStore::tryAcquire(getId());
        assert(ctx);
        if (ctx->cancelled) {
            markDone(*ctx, true);
            return;
        }
        if (!maybeSplit) {
            // Nothing to split left! We are done already.
            markDone(*ctx, true, _internal_result_code);
            // Clean up and report the result directly here.
            cleanup = true;
        }
    }
    if (cleanup) {
        eraseContextAndConclude(getId());
        return;
    }

    auto& [childApp, payloads] = *maybeSplit;

    {
        auto ctx = QbfContextStore::tryAcquire(getId());
        assert(ctx);
        // Spawn child job(s).
        // The job will be a QBF job if any quantifications are left in the formula
        // and will be a SAT job otherwise.
        for (int childIdx = 0; childIdx < payloads.size(); childIdx++) {
            auto& payload = payloads[childIdx];
            spawnChildJob(*ctx, childApp, childIdx, std::move(payload));
        }

        // Non-root jobs should be cleaned up immediately again.
        if (!ctx->isRootNode) markDone(*ctx, true);
    }
}

void QbfJob::installMessageListeners(QbfContext& submitCtx) {

    MessageSubscription subReady(MSG_NOTIFY_JOB_READY, [&, submitCtx](MessageHandle& h) {
        onJobReadyNotification(h, submitCtx);
    });
    MessageSubscription subResults(MSG_QBF_NOTIFICATION_UPWARDS, [&, submitCtx](MessageHandle& h) {
        onResultNotification(h, submitCtx);
    });
    MessageSubscription subCancel(MSG_QBF_CANCEL_CHILDREN, [&, submitCtx](MessageHandle& h) {
        onJobCancelled(h, submitCtx);
    });

    // Store the message subscriptions in the permanent cache of this process.
    PermanentCache::getMainInstance().putMsgSubscription(submitCtx.nodeJobId, std::move(subReady));
    PermanentCache::getMainInstance().putMsgSubscription(submitCtx.nodeJobId, std::move(subResults));
    PermanentCache::getMainInstance().putMsgSubscription(submitCtx.nodeJobId, std::move(subCancel));
}

void QbfJob::processOutMessageQueue() {
    while (!_out_msg_queue.empty()) {
        auto outMsg = _out_msg_queue.front();
        MyMpi::isend(outMsg.rank, outMsg.tag, std::move(outMsg.data));
        _out_msg_queue.pop_front();
    }
}

std::optional<std::pair<QbfJob::ChildJobApp, std::vector<QbfJob::ChildPayload>>> QbfJob::applySplittingStrategy() {
    switch (_params.qbfSplitStrategy()) {
    case QBF_SPLIT_TRIVIAL:
        return applyTrivialSplittingStrategy();
    case QBF_SPLIT_ITERATIVE_DEEPENING:
    default:
        return applyIterativeDeepeningSplittingStrategy();
    }
}

std::optional<std::pair<QbfJob::ChildJobApp, std::vector<QbfJob::ChildPayload>>> QbfJob::applyTrivialSplittingStrategy() {

    std::vector<ChildPayload> payloads;
    // Basic splitting
    auto [fSize, fData] = getFormulaWithPrefix();
    const int* childDataBegin = fData+1;
    const int* childDataEnd = fData+fSize;
    int quantification = fData[0];
    bool childJobsArePureSat = quantification == 0;
    auto ctx_ = QbfContextStore::tryAcquire(getId());
    assert(ctx_);
    auto &ctx = *ctx_;

    int vars = getAppConfig().getFixedSizeIntOrDefault("__NV", -1);
    assert(vars > -1);

    if (childJobsArePureSat) {

        // No quantifications left: Pure SAT!
        ctx.nodeType = QbfContext::AND;
        payloads = prepareSatChildJobs(ctx, childDataBegin, childDataEnd, vars, ctx.depth+1);

    } else {

        // Branch over the first quantification
        int quantifiedVar = std::abs(quantification);
        if (quantification > 0) {
            // existential quantification
            ctx.nodeType = QbfContext::OR;
        } else {
            // universal quantification
            ctx.nodeType = QbfContext::AND;
        }
        payloads = prepareQbfChildJobs(ctx, childDataBegin, childDataEnd, quantifiedVar, vars, ctx.depth+1);
    }

    return std::make_pair(childJobsArePureSat?SAT:QBF, std::move(payloads));
}

std::optional<std::pair<QbfJob::ChildJobApp, std::vector<QbfJob::ChildPayload>>> QbfJob::applyIterativeDeepeningSplittingStrategy() {

    // Assemble formula for each child to spawn
    std::vector<Formula> payloads;
    bool childJobsArePureSat;

    int depth = 0;
    int id = 0;
    int vars = -1;

    {
      auto ctx = QbfContextStore::tryAcquire(getId());
      assert(ctx);
      depth = ctx->depth;
      id = ctx->nodeJobId;
      vars = getAppConfig().getFixedSizeIntOrDefault("__NV", -1);
    }
    assert(vars > -1);

    // Data which ONLY contains the formula's matrix.
    auto pair = getFormulaMatrix();
    auto matrixSize = pair.first;
    auto matrixData = pair.second;
    
    // Data which contains the formula's prefix and matrix (but NOT the global variable order).
    auto [prefixSize, prefixData] = getFormulaWithPrefix();
    Formula formula(prefixData, prefixData+prefixSize);

    // Helper function for checking if a literal is in the formula's matrix.
    auto is_in_matrix = [&](int lit) -> bool {
        int var = abs(lit);
        return std::find_if(matrixData, matrixData+matrixSize,
                          [var](int l) {
                            return abs(l) == var;
                        }) != matrixData+matrixSize;
    };

    // Try to find the next expansion variable which is part of the current formula.
    int expansionVar = extractNextExpansionVariable();
    while (expansionVar != 0 && !is_in_matrix(expansionVar)) {
      LOGGER(_job_log, V3_VERB, "QBF #%i increase depth %d ~> %d as var %d was not in matrix\n", id, depth, (depth+1), expansionVar);
      ++depth;
      expansionVar = extractNextExpansionVariable();
    }

    // Each bloqqer call modifies the prefix and the matrix. Variable indices stay the same.
    BloqqerCaller *bloqqerCaller = nullptr;
    {
      auto ctx = QbfContextStore::tryAcquire(getId());
      assert(ctx);
      ctx->bloqqerCaller = std::make_unique<BloqqerCaller>();
      bloqqerCaller = ctx->bloqqerCaller.get();
    }

    int res = 1;
    while (res == 1 && expansionVar != 0) {
        LOGGER(_job_log, V3_VERB, "QBF #%i calling bloqqer at depth %i with %i vars, lit %i\n", id, depth, vars, expansionVar);
        res = bloqqerCaller->process(formula,
                                     vars,
                                     id,
                                     expansionVar,
                                     _params.expansionCostThreshold());

        LOGGER(_job_log, V3_VERB, "QBF #%i bloqqer returned with result %i\n", id, res);

        switch (res) {
        case 1:
            // Bloqqer was able to expand the variable.
            // This means that this quantification was universal.
            LOGGER(_job_log, V3_VERB, "QBF #%i depth=%i++ var %i expanded\n", getId(), depth, expansionVar);
            assert(expansionVar < 0);
            //formula[depth] = -formula[depth]; // TODO do we still need to perform a flip? I don't think so.
            vars = bloqqerCaller->getVars();
            depth++;
            expansionVar = extractNextExpansionVariable();
            break;
        case 2: {
            // Bloqqer could not expand the variable, or the variable was
            // existential. Anyway, use the new formula to split.
            auto ctx = QbfContextStore::tryAcquire(getId());
            vars = bloqqerCaller->getVars();
            assert(ctx);
            LOGGER(_job_log, V3_VERB, "QBF #%i depth=%i var %i unexpanded - break\n", getId(), depth, expansionVar);
            if (expansionVar < 0) {
                // Universal quantifier.
                // Flip quantifier! But we need to remember that we had a universal at this space.
                // We are in an AND-node.
                //formula[depth] = -formula[depth]; // TODO do we still need to perform this flip?
                expansionVar = -expansionVar;
                ctx->nodeType = QbfContext::AND;
            } else {
                ctx->nodeType = QbfContext::OR;
            }
            break; }
        case 3:
            // Job was cancelled through a kill that propagated a SIGINT
            // through the bloqqer sub-process.
            return {};
        case 10:
            // SAT result directly through Bloqqer
            // TODO: Assign SAT to the Job.
            _internal_result_code = 10;
            return {};
        case 20:
            // UNSAT result directly through Bloqqer
            // TODO: Assign UNSAT to the Job.
            _internal_result_code = 20;
            return {};
        }

        auto ctx = QbfContextStore::tryAcquire(getId());
        assert(ctx);
        if (ctx->cancelled) break;
    }

    auto ctx_ = QbfContextStore::tryAcquire(getId());
    assert(ctx_);
    auto &ctx = *ctx_;

    if (ctx.cancelled) {
        markDone(ctx, true);
        return {};
    }

    // Find out if there are any universal quantifiers left in the formula
    auto matrixBeginIdx = findIdxOfFirstZero(formula.data(), formula.size());
    ++matrixBeginIdx;
    auto universal_quantifiers_in_formula = [&formula, matrixBeginIdx]() {
        auto it = std::find_if(formula.begin(), formula.begin()+matrixBeginIdx, [](int q) {
            return q < 0;
        });

        // our "end"
        return it != formula.begin()+matrixBeginIdx;
    }();

    // Prepare child jobs.
    if (expansionVar != 0 && (ctx.nodeType == QbfContext::AND || universal_quantifiers_in_formula)) {
        // Spawn two new QBF jobs.
        int quantification = expansionVar;
        LOGGER(_job_log, V3_VERB, "QBF #%i spawning two QBF children over var %i @ depth %i\n", getId(), quantification, depth);
        assert(quantification > 0); // existential!

        // Assemble formula consisting of the remaining variable ordering
        // and the current formula (including both prefix and matrix).
        Formula childFormula(
            getDescription().getFormulaPayload(0)+_idx_begin_var_ordering,
            getDescription().getFormulaPayload(0)+_idx_begin_formula_prefix
        );
        assert(!childFormula.empty() && childFormula.back() == 0);
        childFormula.insert(childFormula.end(), formula.begin(), formula.end());

        return std::make_pair(QBF, prepareQbfChildJobs(ctx, childFormula.data(), childFormula.data()+childFormula.size(), quantification, vars, depth+1));
    } else {
        // Spawn a single SAT job.
        LOGGER(_job_log, V3_VERB, "QBF #%i spawning one SAT child\n", getId());
        ctx.nodeType = QbfContext::AND;
        // payload begins at matrix_begin
        return std::make_pair(SAT, prepareSatChildJobs(ctx, formula.data()+matrixBeginIdx, formula.data()+formula.size(), vars, depth+1));
    }
}

std::vector<QbfJob::ChildPayload> QbfJob::prepareSatChildJobs(QbfContext& ctx, const int* begin, const int* end, int vars, int depth) {
    ctx.appendChild(false, -1, -1);
    return std::vector<ChildPayload> {{vars, depth, Formula(begin, end)}};
}

std::vector<QbfJob::ChildPayload> QbfJob::prepareQbfChildJobs(QbfContext& ctx, const int* begin, const int* end, int varToSplitOn, int vars, int depth) {

    std::vector<QbfJob::ChildPayload> payloads;
    {
        Formula childTruePayload(begin, end);
        childTruePayload.push_back(varToSplitOn);
        childTruePayload.push_back(0);
        
        std::string tail;
        for (long i = 0; i<std::min(10UL, childTruePayload.size()); i++) {
            tail += std::to_string(childTruePayload.at(i)) + " ";
        }
        tail += "... ";
        for (long i = std::max(0L, ((long)childTruePayload.size())-10); i<childTruePayload.size(); i++) {
            tail += std::to_string(childTruePayload.at(i)) + " ";
        }
        LOGGER(_job_log, V3_VERB, "QBF #%i payload: %s\n", getId(), tail.c_str());

        payloads.push_back({vars, depth, std::move(childTruePayload)});
        ctx.appendChild(true, -1, -1);
    }
    {
        Formula childFalsePayload(begin, end);
        childFalsePayload.push_back(-varToSplitOn);
        childFalsePayload.push_back(0);

        std::string tail;
        for (long i = 0; i<std::min(10UL, childFalsePayload.size()); i++) {
            tail += std::to_string(childFalsePayload.at(i)) + " ";
        }
        tail += "... ";
        for (long i = std::max(0L, ((long)childFalsePayload.size())-10); i<childFalsePayload.size(); i++) {
            tail += std::to_string(childFalsePayload.at(i)) + " ";
        }
        LOGGER(_job_log, V3_VERB, "QBF #%i payload: %s\n", getId(), tail.c_str());

        payloads.push_back({vars, depth, std::move(childFalsePayload)});
        ctx.appendChild(true, -1, -1);
    }
    return payloads;
}

void QbfJob::spawnChildJob(QbfContext& ctx, ChildJobApp app, int childIdx, ChildPayload&& childPayload) {

    // Create an app configuration object for the child
    // and write it into the job submission JSON
    QbfContext childCtx = ctx.deriveChildContext(childIdx, childPayload.depth, getMyMpiRank());
    AppConfiguration config(getAppConfig());
    childCtx.writeToAppConfig(app==QBF, config);
    config.setFixedSizeInt("__NV", childPayload.nbVars);
    auto json = getJobSubmissionJson(app, config);

    // Access the API used to introduce a job from this job
    auto api = Client::getAnyAPIOrNull();
    assert(api || log_return_false("[ERROR] Could not access job submission API! Does this process have a client role?\n"));

    // Internally store the payload for the child (so that it will get
    // transferred as soon as a 1st worker for the job was found)
    api->storePreloadedRevision(json["user"], json["name"], 0, std::move(childPayload.formula));

    LOGGER(_job_log, V3_VERB, "QBF SPAWNING CHILD\n");

    // Submit child job.
    // DANGER: Callback may be executed AFTER the life time of this job instance!
    api->submit(json, [&, app](nlohmann::json& response) mutable {
        // Only need to react to the callback if it was a SAT job.
        if (app == SAT) {
            LOG(V3_VERB, "QBF SAT child job done\n");
            onSatJobDone(response, ctx);
        }
    });

    ctx.markChildAsSpawned(childCtx.childIdx);
}

void QbfJob::reportJobDone(QbfContext& ctx, int resultCode) {
    _internal_result.id = getId();
    _internal_result.revision = 0;
    _internal_result.result = std::max(resultCode, 0);
    _internal_result.setSolutionToSerialize(nullptr, 0);
    _bg_worker_done = true;
}

void QbfJob::markDone(QbfContext& ctx, bool jobAlive, int resultCode) {
    if (jobAlive && resultCode != 0) _internal_result_code = resultCode;
    if (ctx.isRootNode) return;
    if (jobAlive) reportJobDone(ctx, resultCode);
    if (resultCode < 0) return;
    // Propagate notification upwards
    QbfNotification noti(ctx.rootJobId, ctx.parentJobId, ctx.depth, ctx.childIdx, resultCode);
    OutMessage outMsg {ctx.parentRank, MSG_QBF_NOTIFICATION_UPWARDS, noti.serialize()};
    if (jobAlive) {
        // Job still alive: May not be in the main thread! => Send via message queue.
        auto lock = _mtx_out_msg_queue.getLock();
        _out_msg_queue.push_back(std::move(outMsg));
    } else {
        // Job no longer alive: Cannot use message queue but running in main thread.
        // Can send the message normally.
        MyMpi::isend(outMsg.rank, outMsg.tag, std::move(outMsg.data));
    }
    ctx.cancelled = true;
}

void QbfJob::eraseContextAndConclude(int nodeJobId) {
    {
        auto ctx = QbfContextStore::tryAcquire(nodeJobId);
        assert(ctx);
        if (ctx->isRootNode) reportJobDone(*ctx, _internal_result_code);
    }
    QbfContextStore::erase(nodeJobId);
}

QbfContext QbfJob::buildQbfContextFromAppConfig() {

    // Extract meta data for this particular job node
    // from the AppConfig which is part of the job description
    AppConfiguration appConfig = getAppConfig();
    QbfContext ctx(getId(), appConfig);
    LOGGER(_job_log, V3_VERB, "QBF #%i depth=%i parent [%i]\n", ctx.nodeJobId, ctx.depth, ctx.parentRank);
    return ctx;
}

AppConfiguration QbfJob::getAppConfig() {
    auto lock = _mtx_app_config.getLock();
    return getDescription().getAppConfiguration();
}

nlohmann::json QbfJob::getJobSubmissionJson(ChildJobApp app, const AppConfiguration& appConfig) {
    auto userIdentifier = "#" + std::to_string(getId()) + ":"
    + std::to_string(getIndex()) + "@" + std::to_string(getMyMpiRank());
    nlohmann::json json = {
        {"user", userIdentifier},
        {"name", "child" + std::to_string(_internal_job_counter++)},
        {"preloaded_revisions", {0}},
        {"priority", getPriority()},
        {"application", app == SAT ? "SAT" : "QBF"},
        {"configuration", appConfig.map}
    };
    // Optional: Set wallclock or CPU limit in seconds
    // json["wallclock-limit"] = std::to_string(params.jobWallclockLimit()) + "s";
    // json["cpu-limit"] = std::to_string(params.jobCpuLimit()) + "s";
    return json;
}

void QbfJob::onJobReadyNotification(MessageHandle& h, const QbfContext& submitCtx) {

    SubjobReadyMsg msg = Serializable::get<SubjobReadyMsg>(h.getRecvData());
    if (msg.rootJobId != submitCtx.rootJobId || msg.parentJobId != submitCtx.nodeJobId)
        return;

    bool destruct;
    {
        auto currCtx = QbfContextStore::tryAcquire(submitCtx.nodeJobId);
        if (!currCtx) return; // context not present any more!
        LOG(V3_VERB, "QBF #%i Received ready msg from childidx %i <= [%i]\n", submitCtx.nodeJobId, msg.childIdx, h.source);
        currCtx->markChildAsReady(msg.childIdx, h.source, msg.childJobId);
        destruct = currCtx->isDestructible();
    }
    if (destruct) eraseContextAndConclude(submitCtx.nodeJobId);
}

void QbfJob::onJobCancelled(MessageHandle& h, const QbfContext& submitCtx) {

    // Extract payload of the incoming message
    QbfNotification incomingMsg = Serializable::get<QbfNotification>(h.getRecvData());
    // check that you are indeed the addressee!
    if (incomingMsg.rootJobId != submitCtx.rootJobId
            || incomingMsg.parentJobId != submitCtx.parentJobId
            || incomingMsg.childIdx != submitCtx.childIdx) {
        return;
    }

    bool destruct;
    {
        auto currCtx = QbfContextStore::tryAcquire(submitCtx.nodeJobId);
        if (!currCtx) return; // context not present any more!
        LOG(V3_VERB, "QBF #%i (local:#%i) cancelled\n", submitCtx.rootJobId, submitCtx.nodeJobId);
        currCtx->cancelled = true;
        if(currCtx->bloqqerCaller)
          currCtx->bloqqerCaller->kill();
        destruct = currCtx->isDestructible();
    }
    if (destruct) eraseContextAndConclude(submitCtx.nodeJobId);
}

void QbfJob::onResultNotification(MessageHandle& h, const QbfContext& submitCtx) {

    // Extract payload of the incoming message
    QbfNotification incomingMsg = Serializable::get<QbfNotification>(h.getRecvData());
    // check that you are indeed the addressee!
    if (incomingMsg.rootJobId != submitCtx.rootJobId
            || incomingMsg.parentJobId != submitCtx.nodeJobId) {
        return;
    }

    LOG(V3_VERB, "QBF #%i (local:#%i) notification depth %i->%i childidx %i: result code %i\n",
        submitCtx.rootJobId, submitCtx.nodeJobId, incomingMsg.depth, submitCtx.depth,
        incomingMsg.childIdx, incomingMsg.resultCode);

    handleSubjobDone(submitCtx.nodeJobId, incomingMsg);
}

void QbfJob::onSatJobDone(const nlohmann::json& response, QbfContext& ctx) {

    // SAT job was done.
    int resultCode = response["result"]["resultcode"].get<int>();
    LOG(V3_VERB, "QBF SAT child job done, result code %i\n", resultCode);
    QbfNotification outMsg;
    {
        auto wrappedCtx = QbfContextStore::tryAcquire(ctx.nodeJobId);
        if (!wrappedCtx) return; // context not present any more - must've been cancelled in the mean time
        outMsg = QbfNotification(ctx.rootJobId, ctx.parentJobId, ctx.depth, 0, resultCode);
    }
    handleSubjobDone(ctx.nodeJobId, outMsg);
}

void QbfJob::handleSubjobDone(int nodeJobId, QbfNotification& msg) {

    bool destruct {false};
    {
        auto ctx = QbfContextStore::tryAcquire(nodeJobId);
        if (!ctx) return; // context not present any more!
        int resultCode = ctx->handleNotification(msg);
        if (resultCode >= 0) {
            LOG(V3_VERB, "QBF #%i childidx %i depth %i forwarding my result %i to #%i [%i]\n",
                ctx->nodeJobId, ctx->childIdx, ctx->depth, resultCode, ctx->parentJobId, ctx->parentRank);
            markDone(*ctx, ctx->isRootNode, resultCode);
        }
        destruct = ctx->isDestructible();
    }
    if (destruct) eraseContextAndConclude(nodeJobId);
}

size_t QbfJob::findIdxOfFirstZero(const int* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (data[i] == 0) return i;
    }
    return -1UL;
}
