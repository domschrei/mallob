#include "qbf_job.hpp"
#include "data/app_configuration.hpp"
#include "qbf_context.hpp"

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

void QbfJob::appl_communicate() {}

void QbfJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {}

void QbfJob::appl_dumpStats() {}

bool QbfJob::appl_isDestructible() {
    return _bg_worker_done;
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
void QbfJob::run() {

    // Fetch our formula
    auto [fSize, fData] = getFormulaWithQuantifications();
    auto nbQuantifications = getNumQuantifications(fSize, fData);
    LOGGER(_job_log, V3_VERB, "QBF fsize=%lu qsize=%lu\n", fSize, nbQuantifications);

    // Extract meta data for this particular job node
    // from the AppConfig which is part of the job description
    QbfContext ctx = fetchQbfContextFromAppConfig();

    // Decide what to do with the formula and how many children to spawn.
    ctx.nbDoneChildren = 0;
    ctx.nbTotalChildren = 1;
    const int* childFormulaData = fData+1; // remove one quantification
    bool isSatJob = childFormulaData[0] == 0; // List of quantifications ended!
    std::vector<int> payloadChild1(isSatJob ? childFormulaData+1 : childFormulaData, fData+fSize);

    // Store QbfContext in your job's app config (to be forwarded to children!)
    // and in the permanent cache of this process that exceeds this job's life time.
    storeQbfContext(ctx);

    // Install a callback for incoming messages of the tag MSG_QBF_NOTIFICATION_UPWARDS.
    // CAUTION: Callback may be executed AFTER the life time of this job instance!
    installMessageListener(ctx);

    // Spawn child job(s).
    // The job will be a QBF job if any quantifications are left in the formula
    // and will be a SAT job otherwise. 
    spawnChildJob(ctx, isSatJob?SAT:QBF, std::move(payloadChild1));

    // Non-root jobs should be cleaned up immediately again.
    if (!ctx.isRootNode) markDone();
}

void QbfJob::installMessageListener(QbfContext& submitCtx) {

    // Callback to be executed when a notification message arrives from a child
    // DANGER: This job instance may not be present any longer!
    auto cb = [&, submitCtx](MessageHandle& h) {

        // Extract payload of the incoming message
        QbfNotification incomingMsg = Serializable::get<QbfNotification>(h.getRecvData());
        // check that you are indeed the addressee!
        if (incomingMsg.rootJobId != submitCtx.rootJobId
                || incomingMsg.depth != submitCtx.depth+1) {
            return;
        }

        LOG(V3_VERB, "QBF #%i (local:#%i) notification of depth %i: result code %i\n",
            submitCtx.rootJobId, submitCtx.nodeJobId, submitCtx.depth+1, incomingMsg.resultCode);

        QbfContext currCtx = fetchQbfContextFromPermanentCache(submitCtx.nodeJobId);

        // TODO Logically apply the result you received!
        currCtx.nbDoneChildren++; // now another child is done
        // TODO set result code correctly according to all children's responses!
        int myResultCode = incomingMsg.resultCode;

        LOG(V3_VERB, "QBF #%i %i/%i done\n", currCtx.nodeJobId, currCtx.nbDoneChildren, currCtx.nbTotalChildren);

        // TODO more generic evaluation function on how to react
        if (currCtx.nbDoneChildren == currCtx.nbTotalChildren) {
            // All children done!
            LOG(V3_VERB, "QBF #%i done - cleaning cache\n", currCtx.nodeJobId);
            PermanentCache::getMainInstance().erase(currCtx.nodeJobId);
            if (currCtx.isRootNode) {
                // Job is completely done (and, in this case, still present!)
                markDone(myResultCode);
            } else {
                // Propagate notification upwards
                QbfNotification outMsg(currCtx.rootJobId, currCtx.depth, myResultCode);
                MyMpi::isend(currCtx.parentRank, MSG_QBF_NOTIFICATION_UPWARDS, outMsg);
            }
        } else {
            // Commit updated done children count
            storeQbfContext(currCtx);
        }
    };

    // Store the message subscription in the permanent cache of this process.
    // It will be erased once some notification from all children has arrived.
    PermanentCache::getMainInstance().putMsgSubscription(submitCtx.nodeJobId,
        MessageSubscription(MSG_QBF_NOTIFICATION_UPWARDS, cb));
}

void QbfJob::spawnChildJob(QbfContext& ctx, ChildJobApp app, std::vector<int>&& formula) {

    // Create an app configuration object for the child
    // and write it into the job submission JSON
    QbfContext childCtx = ctx.deriveChildContext(getMyMpiRank());
    AppConfiguration config(getDescription().getAppConfiguration());
    childCtx.writeToAppConfig(config);
    auto json = getJobSubmissionJson(app, config);

    // Access the API used to introduce a job from this job
    auto api = Client::getAnyAPIOrNull();
    assert(api || log_return_false("[ERROR] Could not access job submission API! Does this process have a client role?\n"));

    // Internally store the payload for the child (so that it will get
    // transferred as soon as a 1st worker for the job was found)
    api->storePreloadedRevision(json["user"], json["name"], 0, std::move(formula));

    // Submit child job.
    // DANGER: Callback may be executed AFTER the life time of this job instance!
    api->submit(json, [&, app, ctx](nlohmann::json& response) {

        LOG(V3_VERB, "QBF Child job done\n");

        // Only need to react to the callback if it was a SAT job.
        if (app == SAT) {
            // SAT job was done.
            int resultCode = response["result"]["resultcode"].get<int>();
            LOG(V3_VERB, "QBF SAT child job returned result code %i\n", resultCode);
            if (ctx.isRootNode) {
                // Root? => This job is actually still alive. Conclude it!
                markDone(resultCode);
            } else {
                // Propagate notification upwards
                QbfNotification outMsg(ctx.rootJobId, ctx.depth, resultCode);
                MyMpi::isend(ctx.parentRank, MSG_QBF_NOTIFICATION_UPWARDS, outMsg);
            }
        }
    });
}

void QbfJob::markDone(int resultCode) {
    _internal_result.id = getId();
    _internal_result.revision = 0;
    _internal_result.result = resultCode;
    _internal_result.setSolutionToSerialize(nullptr, 0);
    _bg_worker_done = true;
}

std::pair<size_t, const int*> QbfJob::getFormulaWithQuantifications() {
    return {
        getDescription().getFormulaPayloadSize(0),
        getDescription().getFormulaPayload(0)
    };
}

size_t QbfJob::getNumQuantifications(size_t fSize, const int* fData) {
    size_t size = 0;
    while (size < fSize && fData[size] != 0) ++size;
    return size;
}

QbfContext QbfJob::fetchQbfContextFromAppConfig() {

    // Extract meta data for this particular job node
    // from the AppConfig which is part of the job description
    AppConfiguration appConfig = getDescription().getAppConfiguration();
    QbfContext ctx(getId(), appConfig);
    LOGGER(_job_log, V3_VERB, "QBF #%i depth=%i parent [%i]\n", ctx.nodeJobId, ctx.depth, ctx.parentRank);
    return ctx;
}

QbfContext QbfJob::fetchQbfContextFromPermanentCache(int id) {

    // Fetch permanent cache entry (app config) for this job node
    PermanentCache& cache = PermanentCache::getMainInstance();
    AppConfiguration appConfig;
    appConfig.deserialize(cache.getData(id));
    return QbfContext(id, appConfig);
}

void QbfJob::storeQbfContext(const QbfContext& ctx) {

    // update AppConfig
    AppConfiguration appConfig = getDescription().getAppConfiguration();
    ctx.writeToAppConfig(appConfig);

    // Store the AppConfiguration instance permanently,
    // i.e., exceeding the life time of this job object.
    PermanentCache::getMainInstance().putData(getId(), appConfig.serialize());
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
