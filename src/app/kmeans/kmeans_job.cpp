#include "kmeans_job.hpp"

#include <iostream>
#include <random>
#include <thread>

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "comm/mympi.hpp"
#include "kmeans_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"
KMeansJob::KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId)
    : Job(params, commSize, worldRank, jobId, JobDescription::Application::KMEANS) {
}

void KMeansJob::appl_start() {
    myRank = getJobTree().getRank();
    myIndex = getJobTree().getIndex();
    iAmRoot = getJobTree().isRoot();

    countCurrentWorkers = 1;
    if (iAmRoot) {
        LOG(V0_CRIT, "                           loaded\n");
    }
    LOG(V3_VERB, "                           COMMSIZE: %i myRank: %i myIndex: %i\n",
        countCurrentWorkers, myRank, myIndex);
    LOG(V3_VERB, "                           Children: %i\n",
        this->getJobTree().getNumChildren());
    data = getSerializedDescription(0)->data();
    pointsStart = (float*)(data + getDescription().getMetadataSize() + 3 * sizeof(int));
    LOG(V3_VERB, "                           myIndex: %i getting ready\n", myIndex);
    baseMsg = JobMessage(getId(),
                         getRevision(),
                         epoch,
                         MSG_BROADCAST_DATA);

    loadInstance();
    clusterMembership.assign(pointsCount, -1);

    localClusterCenters.resize(countClusters);
    clusterCenters.resize(countClusters);
    for (int cluster = 0; cluster < countClusters; ++cluster) {
        localClusterCenters[cluster].assign(dimension, 0);
        clusterCenters[cluster].resize(dimension);
    }
    LOG(V3_VERB, "                           myIndex: %i Ready!\n", myIndex);
    loaded = true;
    if (iAmRoot) {
        doInitWork();
    }
}
void KMeansJob::initReducer(JobMessage& msg) {
    JobTree& tempJobTree = getJobTree();

    int lIndex = myIndex * 2 + 1;
    int rIndex = lIndex + 1;

    LOG(V3_VERB, "                           myIndex: %i !tempJobTree.hasLeftChild() %i lIndex in %i\n", myIndex, !tempJobTree.hasLeftChild(), (lIndex < countCurrentWorkers));
    LOG(V3_VERB, "                           myIndex: %i !tempJobTree.hasRightChild() %i rIndex in %i\n", myIndex, !tempJobTree.hasRightChild(), (rIndex < countCurrentWorkers));
    work.clear();
    workDone.clear();
    workDone.push_back(myIndex);
    if (!tempJobTree.hasLeftChild()) {
        leftDone = true;
        if (lIndex < countCurrentWorkers) {
            auto grandChilds = KMeansUtils::childIndexesOf(lIndex, countCurrentWorkers);
            work.push_back(lIndex);
            LOG(V3_VERB, "                           myIndex: %i push in %i\n", myIndex, lIndex);
            for (auto child : grandChilds) {
                work.push_back(child);
                LOG(V3_VERB, "                           myIndex: %i push in %i\n", myIndex, child);
            }
        }
    }
    if (!tempJobTree.hasRightChild()) {
        rightDone = true;
        if ((rIndex < countCurrentWorkers)) {
            auto grandChilds = KMeansUtils::childIndexesOf(rIndex, countCurrentWorkers);
            work.push_back(rIndex);
            LOG(V3_VERB, "                           myIndex: %i push in %i\n", myIndex, rIndex);
            for (auto child : grandChilds) {
                work.push_back(child);
                LOG(V3_VERB, "                           myIndex: %i push in %i\n", myIndex, child);
            }
        }
    }
    // msg.payload = clusterCentersToBroadcast(clusterCenters);
    // msg.payload.push_back(countCurrentWorkers);

    reducer.reset(new JobTreeAllReduction(tempJobTree,
                                          JobMessage(getId(),
                                                     getRevision(),
                                                     epoch,
                                                     MSG_ALLREDUCE_CLAUSES),
                                          std::vector<int>(allReduceElementSize, 0),
                                          folder));

    reducer->setTransformationOfElementAtRoot(rootTransform);
    reducer->disableBroadcast();
    LOG(V3_VERB, "                           myIndex: %i advanceCollective\n", myIndex);
    advanceCollective(msg, tempJobTree);
    if (tempJobTree.hasLeftChild() && !(lIndex < countCurrentWorkers)) {
        leftDone = true;
        baseMsg.tag = MSG_ALLREDUCE_CLAUSES;
        baseMsg.payload = std::vector<int>(allReduceElementSize, 0);

        LOG(V3_VERB, "                           myIndex: %i send fill left\n", myIndex);
        reducer->receive(tempJobTree.getLeftChildNodeRank(), MSG_JOB_TREE_REDUCTION, baseMsg);
    }
    if (tempJobTree.hasRightChild() && !(rIndex < countCurrentWorkers)) {
        rightDone = true;
        baseMsg.tag = MSG_ALLREDUCE_CLAUSES;
        baseMsg.payload = std::vector<int>(allReduceElementSize, 0);
        LOG(V3_VERB, "                           myIndex: %i send fill right\n", myIndex);
        reducer->receive(tempJobTree.getRightChildNodeRank(), MSG_JOB_TREE_REDUCTION, baseMsg);
    }

    hasReducer = true;
}

void KMeansJob::sendRootNotification() {
    initSend = false;
    baseMsg.tag = MSG_BROADCAST_DATA;
    baseMsg.payload = clusterCentersToBroadcast(clusterCenters);
    baseMsg.payload.push_back(countCurrentWorkers);
    LOG(V3_VERB, "                           myIndex: %i sendRootNotification0\n", myIndex);
    MyMpi::isend(getJobTree().getRootNodeRank(), MSG_SEND_APPLICATION_MESSAGE, baseMsg);
}
void KMeansJob::doInitWork() {
    initMsgTask = ProcessWideThreadPool::get().addTask([&]() {
        setRandomStartCenters();
        initSend = true;
    });

    /* Result
    internal_result.result = RESULT_SAT;
    internal_result.id = getId();
    internal_result.revision = getRevision();
    std::vector<int> transformSolution;

    internal_result.encodedType = JobResult::EncodedType::FLOAT;
    auto solution = clusterCentersToSolution();
    internal_result.setSolutionToSerialize((int*)(solution.data()), solution.size());
    finished = true;
    */
}
void KMeansJob::appl_suspend() {
    LOG(V3_VERB, "                           myIndex: %i i got SUSPENDED :( iter: %i\n", myIndex, iterationsDone);
    baseMsg.tag = MSG_ALLREDUCE_CLAUSES;
    baseMsg.returnedToSender = true;
    if (myIndex < 0) {
        myIndex = getJobTree().getIndex();
    }
    baseMsg.payload.assign(1, myIndex);
    MyMpi::isend(getJobTree().getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, baseMsg);
}
void KMeansJob::appl_resume() {
    LOG(V3_VERB, "                           myIndex: %i i got RESUMED :D iter: %i\n", myIndex, iterationsDone);

    reset();
}

void KMeansJob::reset() {
    iterationsDone = 0;
    maxDemand = -1;
    maxDemandCalculated = false;
    finishedJob = false;
    iAmRoot = false;
    loaded = false;
    initSend = false;
    calculatingFinished = false;
    hasReducer = false;
    leftDone = false;
    rightDone = false;
    skipCurrentIter = false;
    myIndex = -2;
    work.clear();
    workDone.clear();
    terminate = true;
    if (initMsgTask.valid()) initMsgTask.get();
    if (calculatingTask.valid()) calculatingTask.get();
    reducer.reset();

    terminate = false;
    appl_start();
}

JobResult&& KMeansJob::appl_getResult() {
    return std::move(internal_result);
}
void KMeansJob::appl_terminate() {
    LOG(V3_VERB, "                           start terminate\n");
    terminate = true;
    reducer.reset();
}

KMeansJob::~KMeansJob() {
    if (initMsgTask.valid()) initMsgTask.get();
    if (calculatingTask.valid()) calculatingTask.get();
    LOG(V3_VERB, "                           end terminate\n");
}

void KMeansJob::appl_communicate() {
    if (!loaded || finishedJob || terminate) return;

    if (iAmRoot && initSend) {
        sendRootNotification();
        LOG(V3_VERB, "                           Send Init ONCE!!!\n");
    }
    if (!work.empty()) {
        // LOG(V3_VERB, "                           myIndex: %i !work.empty() TRUE\n", myIndex);
        if (calculatingFinished) {
            LOG(V3_VERB, "                           myIndex: %i calculatingFinished TRUE\n", myIndex);
            calculatingFinished = false;
            calculatingTask.get();
            const int currentIndex = work[work.size() - 1];
            work.pop_back();
            workDone.push_back(currentIndex);
            calculatingTask = ProcessWideThreadPool::get().addTask([&, cI = currentIndex]() {
                if (!leftDone) leftDone = (currentIndex == (myIndex * 2 + 1));
                if (!rightDone) rightDone = (currentIndex == (myIndex * 2 + 2));
                LOG(V3_VERB, "                           myIndex: %i Start Calc\n", myIndex);
                if (!skipCurrentIter) {
                    calcNearestCenter(metric, cI);
                }
                LOG(V3_VERB, "                           myIndex: %i End Calc childs\n", myIndex);

                calculatingFinished = true;
                LOG(V3_VERB, "                           myIndex: %i work.empty() %i calculatingFinished %i leftDone %i rightDone %i \n", myIndex, work.empty(), calculatingFinished, leftDone, rightDone);
            });
        }
    }

    // LOG(V3_VERB, "                           myIndex: %i work.empty() %i calculatingFinished %i leftDone %i rightDone %i \n", myIndex, work.empty(), calculatingFinished, leftDone, rightDone);
    if (work.empty() && calculatingFinished && leftDone && rightDone) {
        LOG(V3_VERB, "                           myIndex: %i all work Finished!!!\n", myIndex);

        if (calculatingTask.valid()) calculatingTask.get();

        leftDone = false;
        rightDone = false;
        LOG(V3_VERB, "                           myIndex: %i all work Finished2!!!\n", myIndex);
        if (!skipCurrentIter) {
            auto producer = [&]() {
                calcCurrentClusterCenters();

                // LOG(V3_VERB, "clusterCenters: \n%s\n", dataToString(localClusterCenters).c_str());
                return clusterCentersToReduce(localSumMembers, localClusterCenters);
            };
            (reducer)->produce(producer);
            (reducer)->advance();
        }
        calculatingFinished = false;
        LOG(V3_VERB, "                           myIndex: %i all work Finished END!!!\n", myIndex);
    };
    if (hasReducer) {
        if (!skipCurrentIter) {
            if (iAmRoot && (reducer)->hasResult()) {
                LOG(V3_VERB, "                           myIndex: %i received Result from Transform\n", myIndex);
                setClusterCenters((reducer)->extractResult());
                clusterMembership.assign(pointsCount, -1);

                baseMsg.tag = MSG_BROADCAST_DATA;
                baseMsg.payload = clusterCentersToBroadcast(clusterCenters);
                baseMsg.payload.push_back(countCurrentWorkers);
                LOG(V3_VERB, "                           myIndex: %i sendRootNotification1\n", myIndex);
                MyMpi::isend(getJobTree().getRootNodeRank(), MSG_SEND_APPLICATION_MESSAGE, baseMsg);
                // only root...?

                // continue broadcasting

                // LOG(V3_VERB, "                           myIndex: %i clusterCenters: \n%s\n", getJobTree().getIndex(),
                //     dataToString(clusterCenters).c_str());
            }
            (reducer)->advance();
        } else {
            LOG(V3_VERB, "                           Skip Iter\n");
            skipCurrentIter = false;
            hasReducer = false;
            leftDone = false;
            rightDone = false;
            reducer.reset();
            clusterMembership.assign(pointsCount, -1);
            baseMsg.tag = MSG_BROADCAST_DATA;
            baseMsg.payload = clusterCentersToBroadcast(clusterCenters);
            baseMsg.payload.push_back(this->getVolume());
            LOG(V3_VERB, "                           myIndex: %i sendRootNotification2\n", myIndex);
            MyMpi::isend(getJobTree().getRootNodeRank(), MSG_SEND_APPLICATION_MESSAGE, baseMsg);
        }
    }
}

int KMeansJob::getIndex(int rank) {
    // LOG(V3_VERB, "                           myIndex: %i rank in: %i\n", myIndex, rank);
    if (getJobTree().hasLeftChild() && rank == getJobTree().getLeftChildNodeRank()) {
        // LOG(V3_VERB, "                           out L : %i\n", getJobTree().getLeftChildIndex());
        return getJobTree().getLeftChildIndex();
    }
    if (getJobTree().hasRightChild() && rank == getJobTree().getRightChildNodeRank()) {
        // LOG(V3_VERB, "                           out R : %i\n", getJobTree().getRightChildIndex());
        return getJobTree().getRightChildIndex();
    }
    if (!iAmRoot && rank == getJobTree().getParentNodeRank()) {
        // LOG(V3_VERB, "                           out P : %i\n", getJobTree().getParentIndex());
        return getJobTree().getParentIndex();
    }
    if (rank == getJobTree().getRank()) {
        // LOG(V3_VERB, "                           out Me : %i\n", getJobTree().getIndex());
        return getJobTree().getIndex();
    }
    return -1;
}

void KMeansJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
    int sourceIndex = getIndex(source);
    LOG(V3_VERB, "                           myIndex: %i source: %i mpiTag: %i\n", myIndex, sourceIndex, mpiTag);
    if (!loaded || getState() != JobState::ACTIVE) {
        LOG(V3_VERB, "                           myIndex: %i not Ready: %i mpiTag: %i\n", myIndex, sourceIndex, mpiTag);

        msg.returnedToSender = true;
        if (myIndex < 0) {
            myIndex = getJobTree().getIndex();
        }
        msg.payload.assign(1, myIndex);
        MyMpi::isend(source, mpiTag, std::move(msg));

        return;
    }
    if (msg.returnedToSender) {
        // I will do it
        std::vector<int> missingChilds;
        LOG(V3_VERB, "                           myIndex: %i returnFrom: %i mpiTag: %i\n", myIndex, sourceIndex, mpiTag);
        LOG(V3_VERB, "                           !leftDone %i !getJobTree().hasLeftChild() %i getJobTree().getLeftChildIndex() < countCurrentWorkers %i\n", !leftDone, !getJobTree().hasLeftChild(), getJobTree().getLeftChildIndex() < countCurrentWorkers);
        LOG(V3_VERB, "                           !leftDone %i\n", msg.payload[0]);
        if (!leftDone && (msg.payload[0] == getJobTree().getLeftChildIndex() || !getJobTree().hasLeftChild()) && getJobTree().getLeftChildIndex() < countCurrentWorkers) {
            // missing left child
            leftDone = true;
            missingChilds.push_back(getJobTree().getLeftChildIndex());
            LOG(V3_VERB, "                           myIndex: %i LsourceIndex: %i\n", myIndex, sourceIndex);
        }
        if (!rightDone && (msg.payload[0] == getJobTree().getRightChildIndex() || !getJobTree().hasRightChild()) && getJobTree().getRightChildIndex() < countCurrentWorkers) {
            // missing right child
            rightDone = true;
            missingChilds.push_back(getJobTree().getRightChildIndex());
            LOG(V3_VERB, "                           myIndex: %i RsourceIndex: %i\n", myIndex, sourceIndex);
        }
        for (auto child : missingChilds) {
            auto grandChilds = KMeansUtils::childIndexesOf(child, countCurrentWorkers);
            msg.payload = std::vector<int>(allReduceElementSize, 0);
            msg.tag = MSG_ALLREDUCE_CLAUSES;
            (reducer)->receive(source, MSG_JOB_TREE_REDUCTION, msg);
            work.push_back(child);
            for (auto child : grandChilds) {
                work.push_back(child);
            }
        }
        return;
    }

    if (mpiTag == MSG_JOB_TREE_REDUCTION) {
        LOG(V3_VERB, "                           myIndex: %i MSG_JOB_TREE_REDUCTION \n", myIndex);
        LOG(V3_VERB, "                           myIndex: %i sourceIndex: %i (myIndex * 2 + 1): %i \n", myIndex, sourceIndex, (myIndex * 2 + 1));
        LOG(V3_VERB, "                           myIndex: %i source: %i getJobTree().getLeftChildNodeRank(): %i \n", myIndex, source, getJobTree().getLeftChildNodeRank());
        if (!leftDone) leftDone = (source == getJobTree().getLeftChildNodeRank());
        if (!rightDone) rightDone = (source == getJobTree().getRightChildNodeRank());
        if (hasReducer) {
            LOG(V3_VERB, "                           myIndex: %i I have Reducer\n", myIndex);

            bool answear = (reducer)->receive(source, mpiTag, msg);
            LOG(V3_VERB, "                           myIndex: %i bool:%i\n", myIndex, answear);
        }
    }

    if (msg.tag == MSG_BROADCAST_DATA) {
        LOG(V3_VERB, "                           myIndex: %i MSG_BROADCAST_DATA \n", myIndex);
        LOG(V3_VERB, "                           myIndex: %i Workers: %i!\n", myIndex, countCurrentWorkers);
        setClusterCenters(msg.payload);
        if (myIndex < countCurrentWorkers) {
            initReducer(msg);
            clusterMembership.assign(pointsCount, -1);

            // continue broadcasting

            // LOG(V3_VERB, "                           myIndex: %i clusterCenters: \n%s\n", getJobTree().getIndex(),
            //     dataToString(clusterCenters).c_str());
            calculatingTask = ProcessWideThreadPool::get().addTask([&]() {
                LOG(V3_VERB, "                           myIndex: %i Start Calc\n", myIndex);
                calcNearestCenter(metric, myIndex);
                LOG(V3_VERB, "                           myIndex: %i End Calc basic\n", myIndex);
                calculatingFinished = true;
            });
        } else {
            LOG(V3_VERB, "                           myIndex: %i not in range\n", myIndex);

            msg.returnedToSender = true;
            msg.payload.assign(1, myIndex);
            if (myIndex < 0) {
                myIndex = getJobTree().getIndex();
            }
            MyMpi::isend(source, mpiTag, std::move(msg));

            return;
        }
    }
}
void KMeansJob::advanceCollective(JobMessage& msg, JobTree& jobTree) {
    // Broadcast to children

    if (jobTree.hasLeftChild() && jobTree.getLeftChildIndex() < countCurrentWorkers) {
        LOG(V3_VERB, "                           myIndex: %i sendTo %i type: %i\n", myIndex, jobTree.getLeftChildIndex(), MSG_SEND_APPLICATION_MESSAGE);
        MyMpi::isend(jobTree.getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    }
    if (jobTree.hasRightChild() && jobTree.getRightChildIndex() < countCurrentWorkers) {
        LOG(V3_VERB, "                           myIndex: %i sendTo %i type: %i\n", myIndex, jobTree.getRightChildIndex(), MSG_SEND_APPLICATION_MESSAGE);
        MyMpi::isend(jobTree.getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}
void KMeansJob::appl_dumpStats() {}
void KMeansJob::appl_memoryPanic() {}
void KMeansJob::loadInstance() {
    int* metadata = (int*)(data + getDescription().getMetadataSize());
    countClusters = metadata[0];
    dimension = metadata[1];
    pointsCount = metadata[2];
    LOG(V3_VERB, "                          countClusters: %i dimension %i pointsCount: %i\n", countClusters, dimension, pointsCount);

    /*
    kMeansData.reserve(pointsCount);
    float* points = (float*) (data + getDescription().getMetadataSize() + 3*sizeof(int));
    payload += 3;  // pointer start at first datapoint instead of metadata
    for (int point = 0; point < pointsCount; ++point) {
        Point p;
        p.reserve(dimension);
        for (int entry = 0; entry < dimension; ++entry) {
            p.push_back(*(points + entry));
        }
        kMeansData.push_back(p);
        points += dimension;
        if (terminate) return;
    }
    */
    allReduceElementSize = (dimension + 1) * countClusters;
    setMaxDemand();
}

void KMeansJob::setRandomStartCenters() {  // use RNG with seed
    clusterCenters.clear();
    clusterCenters.resize(countClusters);
    std::vector<int> selectedPoints;
    selectedPoints.reserve(countClusters);
    int randomNumber;
    std::random_device rd;
    //std::mt19937 gen(42);  // seed 42 for testing?
    std::mt19937 gen(rd());  
    std::uniform_int_distribution<> distr(0, pointsCount - 1);
    for (int i = 0; i < countClusters; ++i) {
        randomNumber = distr(gen);
        while (std::find(selectedPoints.begin(), selectedPoints.end(), randomNumber) != selectedPoints.end()) {
            randomNumber = distr(gen);
        }
        LOG(V2_INFO, "                          randomNumber: %i\n", randomNumber);
        selectedPoints.push_back(randomNumber);
        clusterCenters[i].reserve(dimension);
        for (int d = 0; d < dimension; d++) {
            clusterCenters[i].push_back(getKMeansData(randomNumber)[d]);
        }
    }
}

void KMeansJob::calcNearestCenter(std::function<float(const float* p1, const float* p2, const size_t dim)> metric, int intervalId) {
    int currentCluster;
    float currentDistance;
    float distanceToCluster;
    // while own or child slices todo
    int startIndex = static_cast<int>(static_cast<float>(pointsCount) * (static_cast<float>(intervalId) / static_cast<float>(countCurrentWorkers)));
    int endIndex = static_cast<int>(static_cast<float>(pointsCount) * (static_cast<float>(intervalId + 1) / static_cast<float>(countCurrentWorkers)));
    LOG(V3_VERB, "                           MI: %i intervalId: %i PC: %i cW: %i start:%i end:%i!!      iter:%i k:%i \n", myIndex, intervalId, pointsCount, countCurrentWorkers, startIndex, endIndex, iterationsDone, countClusters);
    for (int pointID = startIndex; pointID < endIndex; ++pointID) {
        if (terminate) return;
        // LOG(V1_WARN, "(pointID / endIndex) < 0.25: %i iAmRoot: %i countCurrentWorkers == 1: %i std::find(work.begin(), work.end(), 1) != work.end() && std::find(work.begin(), work.end(), 2) != work.end()): %i leftDone && rightDone:%i this->getVolume() > 1:%i\n", (pointID / endIndex) < 0.25, iAmRoot, countCurrentWorkers == 1, std::find(work.begin(), work.end(), 1) != work.end() && std::find(work.begin(), work.end(), 2) != work.end(), leftDone && rightDone,  this->getVolume() > 1);

        if ((pointID / endIndex) < 0.25 &&
            iAmRoot &&
            (countCurrentWorkers == 1 ||
             (std::find(work.begin(), work.end(), 1) != work.end() && std::find(work.begin(), work.end(), 2) != work.end())) &&
            (leftDone && rightDone) &&
            this->getVolume() > 1) {
            LOG(V2_INFO, "                           will skip Iter\n");
            skipCurrentIter = true;
            work.clear();
            workDone.clear();
            return;
        }
        currentCluster = -1;
        currentDistance = std::numeric_limits<float>::infinity();
        for (int clusterID = 0; clusterID < countClusters; ++clusterID) {
            distanceToCluster = metric(getKMeansData(pointID), clusterCenters[clusterID].data(), dimension);
            if (distanceToCluster < currentDistance) {
                currentCluster = clusterID;
                currentDistance = distanceToCluster;
            }
        }

        clusterMembership[pointID] = currentCluster;
    }
    LOG(V3_VERB, "                           MI: %i intervalId: %i PC: %i cW: %i start:%i end:%i COMPLETED iter:%i \n", myIndex, intervalId, pointsCount, countCurrentWorkers, startIndex, endIndex, iterationsDone);
}

void KMeansJob::calcCurrentClusterCenters() {
    oldClusterCenters = clusterCenters;

    countMembers();

    for (int cluster = 0; cluster < countClusters; ++cluster) {
        auto currentCenter = localClusterCenters[cluster].data();
        for (int d = 0; d < dimension; ++d) {
            currentCenter[d] = 0;
        }
    }
    for (auto workIndex : workDone) {
        int startIndex = static_cast<int>(static_cast<float>(pointsCount) * (static_cast<float>(workIndex) / static_cast<float>(countCurrentWorkers)));

        int endIndex = static_cast<int>(static_cast<float>(pointsCount) * (static_cast<float>(workIndex + 1) / static_cast<float>(countCurrentWorkers)));
        for (int pointID = startIndex; pointID < endIndex; ++pointID) {
            // if (clusterMembership[pointID] != -1) {
            int clusterId = clusterMembership[pointID];
            auto currentCenter = localClusterCenters[clusterId].data();
            float divisor = static_cast<float>(localSumMembers[clusterId]);
            auto currentDataPoint = getKMeansData(pointID);
            for (int d = 0; d < dimension; ++d) {
                currentCenter[d] += currentDataPoint[d] / divisor;
            }
        }
    }
    ++iterationsDone;
}

std::string KMeansJob::dataToString(std::vector<Point> data) {
    std::stringstream result;

    for (auto point : data) {
        for (auto entry : point) {
            result << entry << " ";
        }
        result << "\n";
    }
    return result.str();
}
std::string KMeansJob::dataToString(std::vector<int> data) {
    std::stringstream result;

    for (auto entry : data) {
        result << entry << " ";
    }
    result << "\n";
    return result.str();
}

void KMeansJob::countMembers() {
    localSumMembers.assign(countClusters, 0);
    for (int i = 0; i < pointsCount; ++i) {
        int clusterID = clusterMembership[i];
        if (clusterID != -1) {
            localSumMembers[clusterID] += 1;
        }
    }
    LOG(V3_VERB, "                           myIndex: %i sumMembers: %s\n",
        myIndex, dataToString(localSumMembers).c_str());
}

// float KMeansJob::calculateDifference(std::function<float(const float*, Point&)> metric) {
//     if (iterationsDone == 0) {
//         return std::numeric_limits<float>::infinity();
//     }
//     float sumOldvec = 0.0;
//     float sumDifference = 0.0;
//     Point v0(dimension, 0);
//     for (int k = 0; k < countClusters; ++k) {
//         sumOldvec += metric(v0.data(), clusterCenters[k]);
//
//         sumDifference += metric(clusterCenters[k].data(), oldClusterCenters[k]);
//     }
//     return sumDifference / sumOldvec;
// }

bool KMeansJob::centersChanged() {
    if (iterationsDone == 0) {
        return true;
    }
    for (int k = 0; k < countClusters; ++k) {
        for (int d = 0; d < dimension; ++d) {
            if (clusterCenters[k][d] != oldClusterCenters[k][d]) {
                return true;
            }
        }
    }
    return false;
}

bool KMeansJob::centersChanged(float factor) {
    if (iterationsDone == 0) {
        return true;
    }
    for (int k = 0; k < countClusters; ++k) {
        for (int d = 0; d < dimension; ++d) {
            float distance = fabs(clusterCenters[k][d] - oldClusterCenters[k][d]);
            float upperBoundDistance = factor * (fabs(clusterCenters[k][d] + oldClusterCenters[k][d]) / 2);
            if (distance > upperBoundDistance) {
                LOG(V3_VERB, "                           Dist: %f upperBoundDistance: %f\n", distance, upperBoundDistance);
                return true;
            }
        }
    }
    return false;
}

std::vector<float> KMeansJob::clusterCentersToSolution() {
    std::vector<float> result;
    result.reserve(countClusters * dimension + 2);
    result.push_back((float)countClusters);
    result.push_back((float)dimension);

    for (auto point : clusterCenters) {
        for (auto entry : point) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<int> KMeansJob::clusterCentersToBroadcast(const std::vector<Point>& reduceClusterCenters) {
    std::vector<int> result;
    result.resize(reduceClusterCenters.size() * dimension);
    int i = 0;
    for (auto point : reduceClusterCenters) {
        auto centerData = point.data();
        for (int entry = 0; entry < dimension; ++entry) {
            result[i++] = (*((int*)(centerData + entry)));
        }
    }
    // LOG(V3_VERB, "                           reduce in clusterCentersToBroadcast: \n%s\n",
    //     dataToString(result).c_str());
    return result;
}

std::vector<KMeansJob::Point> KMeansJob::broadcastToClusterCenters(const std::vector<int>& reduce) {
    std::vector<Point> localClusterCentersResult;
    const int elementsCount = allReduceElementSize - countClusters;
    const int* reduceData = reduce.data();

    reduceData += countClusters;

    localClusterCentersResult.resize(countClusters);
    for (int k = 0; k < countClusters; ++k) {
        localClusterCentersResult[k].resize(dimension);
        auto currentCenter = localClusterCentersResult[k].data();
        auto receivedCenter = reduceData + k * dimension;
        for (int d = 0; d < dimension; ++d) {
            currentCenter[d] = *((float*)(receivedCenter + d));
        }
    }

    return localClusterCentersResult;
}
void KMeansJob::setClusterCenters(const std::vector<int>& reduce) {
    const int elementsCount = allReduceElementSize - countClusters;
    const int* reduceData = reduce.data();
    for (int k = 0; k < countClusters; ++k) {
        auto currentCenter = clusterCenters[k].data();
        auto receivedCenter = reduceData + k * dimension;
        for (int d = 0; d < dimension; ++d) {
            currentCenter[d] = *((float*)(receivedCenter + d));
        }
    }

    countCurrentWorkers = *(reduceData + elementsCount);
    LOG(V3_VERB, "                           myIndex: %i countCurrentWorkers: %i\n", myIndex, countCurrentWorkers);
}

std::vector<int> KMeansJob::clusterCentersToReduce(const std::vector<int>& reduceSumMembers, const std::vector<Point>& reduceClusterCenters) {
    std::vector<int> result;
    int i = 0;
    std::vector<int> tempCenters = clusterCentersToBroadcast(reduceClusterCenters);

    result.resize(reduceSumMembers.size());

    for (auto entry : reduceSumMembers) {
        result[i++] = entry;
    }
    result.insert(result.end(), tempCenters.begin(), tempCenters.end());

    return result;
}

std::pair<std::vector<std::vector<float>>, std::vector<int>>
KMeansJob::reduceToclusterCenters(const std::vector<int>& reduce) {
    // auto [centers, counts] = reduceToclusterCenters(); //call example
    std::vector<int> localSumMembersResult;
    const int elementsCount = allReduceElementSize - countClusters;

    const int* reduceData = reduce.data();

    localSumMembersResult.assign(countClusters, 0);
    for (int i = 0; i < countClusters; ++i) {
        // LOG(V3_VERB, "i: %d\n", i);
        localSumMembersResult[i] = reduceData[i];
    }

    return std::pair(std::move(broadcastToClusterCenters(reduce)), std::move(localSumMembersResult));
}

std::vector<int> KMeansJob::aggregate(const std::list<std::vector<int>>& messages) {
    std::vector<std::vector<KMeansJob::Point>> centers;
    std::vector<int> counts;
    std::vector<int> tempSumMembers;
    std::vector<Point> tempClusterCenters;
    const int countMessages = messages.size();
    centers.resize(countMessages);
    counts.reserve(countMessages * countClusters);
    auto message = messages.begin();
    // LOG(V3_VERB, "                         myIndex: %i countMessages: %d\n", myIndex, countMessages);
    for (int i = 0; i < countMessages; ++i) {
        auto data = reduceToclusterCenters(*message);
        ++message;
        centers[i] = std::move(data.first);
        counts.insert(counts.end(), data.second.begin(), data.second.end());

        // LOG(V3_VERB, "                         myIndex: %i counts[%d] : \n%s\n", myIndex, i, dataToString(counts[i]).c_str());
        //  LOG(V3_VERB, "clusterCentersI: \n%s\n", dataToString(centers[i]).c_str());
    }

    tempSumMembers.resize(countClusters);
    for (int i = 0; i < countMessages; ++i) {
        for (int j = 0; j < countClusters; ++j) {
            tempSumMembers[j] = tempSumMembers[j] + counts[i * countClusters + j];

            // LOG(V3_VERB, "tempSumMembers[%d] + counts[%d][%d] : \n%d\n",j,i,j, tempSumMembers[j] + counts[i][j]);
            // LOG(V3_VERB, "tempSumMembers[%d] : \n%d\n",j, tempSumMembers[j]);
            // LOG(V3_VERB, "KRITcounts[%d] : \n%s\n",i, dataToString(tempSumMembers).c_str());
        }
    }
    tempClusterCenters.resize(countClusters);
    for (int i = 0; i < countClusters; ++i) {
        tempClusterCenters[i].assign(dimension, 0);
    }

    float ratio;
    float currentSum;

    for (int i = 0; i < countMessages; ++i) {
        std::vector<KMeansJob::Point>& currentMsg = centers[i];
        for (int j = 0; j < countClusters; ++j) {
            currentSum = static_cast<float>(tempSumMembers[j]);
            if (currentSum != 0) {
                KMeansJob::Point& currentCenter = tempClusterCenters[j];
                KMeansJob::Point& currentMsgCenter = currentMsg[j];
                ratio = static_cast<float>(counts[i * countClusters + j]) /
                        currentSum;
                for (int k = 0; k < dimension; ++k) {
                    currentCenter[k] += currentMsgCenter[k] * ratio;
                }
            }
        }
    }

    return clusterCentersToReduce(tempSumMembers, tempClusterCenters);  // localSumMembers, tempClusterCenters
}