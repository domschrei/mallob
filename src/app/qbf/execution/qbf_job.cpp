#include "qbf_job.hpp"

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
  return _bg_worker_done ? 0 : -1;}
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
  size_t fSize = getDescription().getFormulaPayloadSize(0);
  LOGGER(_job_log, V3_VERB, "QBF Formula size: %lu\n", fSize);
  const int* fPayload = getDescription().getFormulaPayload(0);

  // Extract all quantifications
  std::vector<int> quantifications;
  for (size_t i = 0; i < fSize && fPayload[i] != 0; ++i) {
    quantifications.push_back(fPayload[i]);
  }
  LOGGER(_job_log, V3_VERB, "QBF Quantifier list: %s\n", StrUtil::vecToStr(quantifications).c_str());

  // Extract meta data for this particular job node
  // from the AppConfig which is part of the job description
  AppConfiguration appConfig = getDescription().getAppConfiguration();
  int depth = appConfig.getIntOrDefault("depth", 0);
  int parentRank = appConfig.getIntOrDefault("parent", -1);
  LOGGER(_job_log, V3_VERB, "QBF I am on depth %i; parent [%i]\n", depth, parentRank);
  bool isLogicalRoot = parentRank == -1; // actual root of the QBF solving effort?

  // Access the API used to introduce a job from this job
  auto api = Client::getAnyAPIOrNull();
  assert(api || log_return_false("[ERROR] Could not access job submission API! Does this process have a client role?\n"));

  // Access the app config and store it permanently,
  // i.e., exceeding the life time of this job object.
  appConfig.map["done_children"] = "0"; // count num. done children later
  PermanentCache& cache = PermanentCache::getMainInstance();
  cache.putData(getId(), appConfig.serialize());

  // Derive a new app config for the child job(s).
  int childDepth = depth+1;
  appConfig.map["depth"] = std::to_string(childDepth);
  appConfig.map["parent"] = std::to_string(getMyMpiRank()); // my own rank

  // Construct a JSON for the child job.
  nlohmann::json json;
  std::vector<int> payloadForChild;
  bool spawnSatJob = quantifications.empty();
  if (spawnSatJob) {
    // Quantifier-free, pure SAT problem!
    json = getJobSubmissionJson(ChildJobApp::SAT, appConfig);
    // remove the leading "0" from the payload which terminated the quantifications
    auto fStart = fPayload + (fPayload[0]==0 ? 1 : 0);
    payloadForChild = std::vector<int>(fStart, fPayload+fSize);
  } else {
    // Incorrect dummy simplification for now: just remove one quantifier
    json = getJobSubmissionJson(ChildJobApp::QBF, appConfig);
    payloadForChild = std::vector<int>(fPayload+1, fPayload+fSize);
  }

  // Internally store the payload for the child (so that it will get
  // transferred as soon as a 1st worker for the job was found)
  api->storePreloadedRevision(json["user"], json["name"], 0, std::move(payloadForChild));

  // How many children do I spawn (and have to wait for later)?
  int nbTotalChildren = 1;

  // Install a callback for incoming messages of the tag MSG_QBF_NOTIFICATION_UPWARDS.
  // CAUTION: Callback may be executed AFTER the life time of this job instance!

  auto cb = [&, id=getId(), isLogicalRoot, nbTotalChildren, depth, parentRank](MessageHandle& h) {

    // Extract payload of the incoming message
    IntVec data = Serializable::get<IntVec>(h.getRecvData());
    if (data[0] != depth+1) return; // check that you are indeed the addressee!

    LOG(V3_VERB, "QBF #%i notification of depth %i\n", id, depth+1);

    // Fetch permanent cache entry (app config) for this job node
    PermanentCache& cache = PermanentCache::getMainInstance();
    AppConfiguration appConfig;
    appConfig.deserialize(cache.getData(id));

    // TODO Logically apply the result you received!

    // How many children were done before?
    int nbDoneChildren = appConfig.getIntOrDefault("done_children", 0);
    nbDoneChildren++; // now one more is done

    LOG(V3_VERB, "QBF #%i %i/%i done\n", id, nbDoneChildren, nbTotalChildren);

    if (nbDoneChildren == nbTotalChildren) {
      // All children done!
      LOG(V3_VERB, "QBF #%i done - cleaning cache\n", id);
      cache.erase(id);
      if (isLogicalRoot) {
        // Job is completely done
        markDone();
      } else {
        // Propagate notification upwards
        // TODO Send actual data about the result upwards!
        MyMpi::isend(parentRank, MSG_QBF_NOTIFICATION_UPWARDS, IntVec({depth}));
      }
    } else {
      // Commit updated done children count
      appConfig.map["done_children"] = nbDoneChildren;
      cache.putData(id, appConfig.serialize());
    }
  };
  
  cache.putMsgSubscription(getId(), MessageSubscription(MSG_QBF_NOTIFICATION_UPWARDS, cb));

  // Submit child job.
  // CAUTION: Callback may be executed AFTER the life time of this job instance!
  api->submit(json, [
                     &,
                     id=getId(),
                     isLogicalRoot,
                     spawnSatJob,
                     parentRank,
                     depth]
              (nlohmann::json& response) {

    LOGGER(_job_log, V3_VERB, "QBF Child job done\n");

    // Only need to react to the callback if it was a SAT job.
    if (spawnSatJob) {
      // SAT job was done
      // TODO extract proper result

      if (isLogicalRoot) {
        // Root? => This job is actually still alive. Conclude it!
        markDone();
      } else {
        // Propagate notification upwards
        // TODO Send actual data about the result upwards!
        MyMpi::isend(parentRank, MSG_QBF_NOTIFICATION_UPWARDS, IntVec({depth}));
      }
    }
  });

  // Non-root jobs should be cleaned up immediately again.
  if (!isLogicalRoot) markDone();
}


void QbfJob::markDone() {
  _internal_result.id = getId();
  _internal_result.revision = 0;
  _internal_result.result = 0;
  _internal_result.setSolutionToSerialize(nullptr, 0);
  _bg_worker_done = true;
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
