#include "kmeans_job.hpp"

#include <iostream>
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
KMeansJob::KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId, const int* newPayload)
    : Job(params, commSize, worldRank, jobId, JobDescription::Application::KMEANS) {
    setPayload(newPayload);
}

void KMeansJob::appl_start() {
    myRank = getJobTree().getRank();
    myIndex = getJobTree().getIndex();
    iAmRoot = getJobTree().isRoot();

    countCurrentWorkers = 8;

    LOG(V2_INFO, "                           COMMSIZE: %i myRank: %i myIndex: %i\n",
        countCurrentWorkers, myRank, myIndex);
    LOG(V2_INFO, "                           Children: %i\n",
        this->getJobTree().getNumChildren());
    payload = getDescription().getFormulaPayload(0);
    LOG(V2_INFO, "                           myIndex: %i getting ready1\n", myIndex);
    baseMsg = JobMessage(getId(),
                         getRevision(),
                         epoch,
                         MSG_WARMUP);
    LOG(V2_INFO, "                           myIndex: %i getting ready2\n", myIndex);

    loadTask = ProcessWideThreadPool::get().addTask([&]() {
        LOG(V2_INFO, "                           myIndex: %i getting ready3\n", myIndex);
        loadInstance();
        clusterMembership.assign(pointsCount, -1);
        LOG(V2_INFO, "                           myIndex: %i Ready!\n", myIndex);
        loaded = true;
        if (iAmRoot) {
            doInitWork();
        }
    });
}
void KMeansJob::initReducer() {
    auto folder =
        [&](std::list<std::vector<int>>& elems) {
            return aggregate(elems);
        };
    auto rootTransform = [&](std::vector<int> payload) {
        LOG(V2_INFO, "                           myIndex: %i start Roottransform\n", myIndex);
        auto data = reduceToclusterCenters(payload);

        clusterCenters = data.first;
        sumMembers = data.second;
        int sum = 0;
        for (auto i : sumMembers) {
            sum += i;
        }
        if (sum == pointsCount) {
            allCollected = true;

            LOG(V2_INFO, "                           AllCollected: Good\n");
        } else {
            LOG(V2_INFO, "                           AllCollected: Error\n");
        }
        auto transformed = clusterCentersToBroadcast(clusterCenters);
        transformed.push_back(this->getVolume());
        LOG(V2_INFO, "                           COMMSIZE: %i myIndex: %i \n",
            this->getVolume(), myIndex);
        LOG(V2_INFO, "                           Children: %i\n",
            this->getJobTree().getNumChildren());

        if ((1 / 1000 < calculateDifference(
                            [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); }))) {
            LOG(V2_INFO, "                           Another iter %i\n", epoch);
            sendRootNotification(MSG_JOB_TREE_BROADCAST);
            return transformed;

        } else {
            LOG(V2_INFO, "                           Got Result\n");
            internal_result.result = RESULT_SAT;
            internal_result.id = getId();
            internal_result.revision = getRevision();
            std::vector<int> transformSolution;

            internal_result.encodedType = JobResult::EncodedType::FLOAT;
            auto solution = clusterCentersToSolution();
            internal_result.setSolutionToSerialize((int*)(solution.data()), solution.size());
            finishedJob = true;
            return std::move(std::vector<int>(allReduceElementSize, 0));
        }
    };

    reducer.reset(new JobTreeAllReduction(getJobTree(),
                                          JobMessage(getId(),
                                                     getRevision(),
                                                     ++epoch,
                                                     MSG_ALLREDUCE_CLAUSES),
                                          std::vector<int>(allReduceElementSize, 0),
                                          folder));

    reducer->setTransformationOfElementAtRoot(rootTransform);
    hasReducer = true;
}

void KMeansJob::sendRootNotification(int tag) {
    initSend = false;
    auto tree = getJobTree();
    baseMsg = JobMessage(getId(),
                         getRevision(),
                         epoch,
                         tag);
    baseMsg.payload = clusterCentersToBroadcast(clusterCenters);
    baseMsg.payload.push_back(countCurrentWorkers);
    MyMpi::isend(getJobTree().getRootNodeRank(), tag, std::move(baseMsg));
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
void KMeansJob::appl_suspend() {}
void KMeansJob::appl_resume() {}
JobResult&& KMeansJob::appl_getResult() {
    return std::move(internal_result);
}
void KMeansJob::appl_terminate() {
    if (loaded && loadTask.valid()) loadTask.get();
    if (initSend && initMsgTask.valid()) initMsgTask.get();
}
void KMeansJob::appl_communicate() {
    if (!loaded) return;

    if (iAmRoot && initSend) {
        sendRootNotification(MSG_JOB_TREE_BROADCAST);
        LOG(V2_INFO, "                           Send Init ONCE!!!\n");
    }
    if (calculatingFinished) {
        LOG(V2_INFO, "                           myIndex: %i Calc Finished!!!\n", myIndex);

        if (calculatingTask.valid()) calculatingTask.get();

        auto producer = [&]() {
            return clusterCentersToReduce(localSumMembers, localClusterCenters);
        };
        (reducer)->produce(producer);
        clusterMembership.assign(pointsCount, -1);
        calculatingFinished = false;
    };
    if (hasReducer) {
        (reducer)->advance();
        if ((reducer)->hasResult()) {
            LOG(V2_INFO, "                           myIndex: %i received clusterCenters\n", myIndex);
            clusterCenters = broadcastToClusterCenters((reducer)->extractResult(), true);
        }
    }
}
void KMeansJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
    LOG(V2_INFO, "                           myIndex: %i source: %i mpiTag: %i\n", myIndex, getJobComm().getWorldToInternalMap()[source], mpiTag);
    if (!loaded) {
        LOG(V2_INFO, "                           myIndex: %i not Ready: %i mpiTag: %i\n", myIndex, getJobComm().getWorldToInternalMap()[source], mpiTag);
        if (!msg.returnedToSender) {
            msg.returnedToSender = true;
            MyMpi::isend(source, mpiTag, std::move(msg));
        }
        return;
    }
    if (msg.returnedToSender) {
        // I will do it
        LOG(V2_INFO, "                           myIndex: %i returnto: %i mpiTag: %i\n", myIndex, getJobComm().getWorldToInternalMap()[source], mpiTag);
        msg.returnedToSender = false;
        MyMpi::isend(source, mpiTag, std::move(msg));
        return;
    }
    if (hasReducer) {
        LOG(V2_INFO, "                           myIndex: %i I have Reducer\n", myIndex);
        if (mpiTag == MSG_JOB_TREE_BROADCAST) {
            initReducer();
        } else {
            bool answear = (reducer)->receive(source, mpiTag, msg);
            LOG(V2_INFO, "                           myIndex: %i bool:%i\n", myIndex, answear);
        }
    }

    LOG(V2_INFO, "                           myIndex: %i MESSAGE! %i \n", myIndex, mpiTag);
    if (mpiTag == MSG_JOB_TREE_BROADCAST) {
        LOG(V2_INFO, "                           myIndex: %i Broadcast in!\n", myIndex);
        LOG(V2_INFO, "                           myIndex: %i Workers: %i!\n", myIndex, countCurrentWorkers);
        if (myIndex < countCurrentWorkers) {
            if (hasReducer) {
                (reducer)->advance();
                if ((reducer)->hasResult()) {
                    LOG(V2_INFO, "                           myIndex: %i received clusterCenters\n", myIndex);
                    clusterCenters = broadcastToClusterCenters((reducer)->extractResult(), true);
                }
            }
            if (!receivedInitSend) {
                advanceCollective(msg, MSG_JOB_TREE_BROADCAST);

                clusterCenters = broadcastToClusterCenters(msg.payload, true);
                receivedInitSend = true;
            }
            // continue broadcasting

            // LOG(V2_INFO, "                           myIndex: %i clusterCenters: \n%s\n", getJobTree().getIndex(),
            //     dataToString(clusterCenters).c_str());
            calculatingTask = ProcessWideThreadPool::get().addTask([&]() {
                LOG(V2_INFO, "                           myIndex: %i Start Calc\n", myIndex);
                calcNearestCenter(metric, myIndex);
                calcCurrentClusterCenters();
                LOG(V2_INFO, "                           myIndex: %i End Calc\n", myIndex);
                calculatingFinished = true;
            });
        }
    }
}
void KMeansJob::advanceCollective(JobMessage& msg, int broadcastTag) {
    // Broadcast to children
    if (msg.tag == broadcastTag) {
        if (getJobTree().hasLeftChild() && getJobTree().getLeftChildIndex() < countCurrentWorkers)
            MyMpi::isend(getJobTree().getLeftChildNodeRank(), broadcastTag, msg);
        if (getJobTree().hasRightChild() && getJobTree().getRightChildIndex() < countCurrentWorkers)
            MyMpi::isend(getJobTree().getRightChildNodeRank(), broadcastTag, msg);
    }
}
void KMeansJob::appl_dumpStats() {}
void KMeansJob::appl_memoryPanic() {}
void KMeansJob::loadInstance() {
    countClusters = payload[0];
    dimension = payload[1];
    pointsCount = payload[2];
    kMeansData.reserve(pointsCount);
    payload += 3;  // pointer start at first datapoint instead of metadata
    for (int point = 0; point < pointsCount; ++point) {
        Point p;
        p.reserve(dimension);
        for (int entry = 0; entry < dimension; ++entry) {
            p.push_back(*((float*)(payload + entry)));
        }
        kMeansData.push_back(p);
        payload = payload + dimension;
    }
    allReduceElementSize = (dimension + 1) * countClusters;
}

void KMeansJob::setRandomStartCenters() {
    clusterCenters.clear();
    clusterCenters.resize(countClusters);
    for (int i = 0; i < countClusters; ++i) {
        clusterCenters[i] = kMeansData[static_cast<int>((static_cast<float>(i) / static_cast<float>(countClusters)) * (pointsCount - 1))];
    }
}

void KMeansJob::calcNearestCenter(std::function<float(Point, Point)> metric, int intervalId) {
    struct centerDistance {
        int cluster;
        float distance;
    } currentNearestCenter;
    float distanceToCluster;
    // while own or child slices todo
    int startIndex = static_cast<int>(static_cast<float>(pointsCount) * (static_cast<float>(intervalId) / static_cast<float>(countCurrentWorkers)));
    int endIndex = static_cast<int>(static_cast<float>(pointsCount) * (static_cast<float>(intervalId + 1) / static_cast<float>(countCurrentWorkers)));
    LOG(V2_INFO, "                           MR: %i PC: %i cW: %i start:%i end:%i!!\n", intervalId, pointsCount, countCurrentWorkers, startIndex, endIndex);
    for (int pointID = startIndex; pointID < endIndex; ++pointID) {
        currentNearestCenter.cluster = -1;
        currentNearestCenter.distance = std::numeric_limits<float>::infinity();
        for (int clusterID = 0; clusterID < countClusters; ++clusterID) {
            distanceToCluster = metric(kMeansData[pointID], clusterCenters[clusterID]);
            if (distanceToCluster < currentNearestCenter.distance) {
                currentNearestCenter.cluster = clusterID;
                currentNearestCenter.distance = distanceToCluster;
            }
        }

        clusterMembership[pointID] = currentNearestCenter.cluster;
    }
}

void KMeansJob::calcCurrentClusterCenters() {
    oldClusterCenters = clusterCenters;

    countMembers();

    localClusterCenters.clear();
    localClusterCenters.resize(countClusters);
    for (int cluster = 0; cluster < countClusters; ++cluster) {
        localClusterCenters[cluster].assign(dimension, 0);
    }
    for (int pointID = 0; pointID < pointsCount; ++pointID) {
        if (clusterMembership[pointID] != -1) {
            for (int d = 0; d < dimension; ++d) {
                if (static_cast<float>(localSumMembers[clusterMembership[pointID]]) == 0) {
                    LOG(V2_INFO, "                           myIndex: %i this shouldnt happen..\n", myIndex);
                    LOG(V2_INFO, "                           myIndex: %i localSumMembers: %s\n", myIndex, dataToString(localSumMembers).c_str());
                    LOG(V2_INFO, "                           myIndex: %i clusterMembership[pointID]: %i\n", myIndex, clusterMembership[pointID]);
                    LOG(V2_INFO, "                           myIndex: %i d: %i\n", myIndex, d);
                    LOG(V2_INFO, "                           myIndex: %i localClusterCenters[clusterMembership[pointID]][d]: %i\n", myIndex, localClusterCenters[clusterMembership[pointID]][d]);
                    LOG(V2_INFO, "                           myIndex: %i kMeansData[pointID][d]: %i\n", myIndex, kMeansData[pointID][d]);
                    LOG(V2_INFO, "                           myIndex: %i localSumMembers[clusterMembership[pointID]]: %i\n", myIndex, localSumMembers[clusterMembership[pointID]]);
                    LOG(V2_INFO, "                           myIndex: %i kMeansData[pointID][d] / static_cast<float>(localSumMembers[clusterMembership[pointID]]): %f\n", myIndex, kMeansData[pointID][d] / static_cast<float>(localSumMembers[clusterMembership[pointID]]));
                }
                localClusterCenters[clusterMembership[pointID]][d] +=
                    kMeansData[pointID][d] / static_cast<float>(localSumMembers[clusterMembership[pointID]]);
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
    for (int clusterID : clusterMembership) {
        if (clusterID != -1) {
            localSumMembers[clusterID] += 1;
        }
    }
    LOG(V2_INFO, "                           myIndex: %i sumMembers: %s\n",
        myIndex, dataToString(localSumMembers).c_str());
}

float KMeansJob::calculateDifference(std::function<float(Point, Point)> metric) {
    if (iterationsDone == 0) {
        return std::numeric_limits<float>::infinity();
    }
    float sumOldvec = 0.0;
    float sumDifference = 0.0;
    Point v0(dimension, 0);
    for (int k = 0; k < countClusters; ++k) {
        sumOldvec += metric(v0, clusterCenters[k]);

        sumDifference += metric(clusterCenters[k], oldClusterCenters[k]);
    }
    return sumDifference / sumOldvec;
}

std::vector<float> KMeansJob::clusterCentersToSolution() {
    std::vector<float> result;
    result.push_back((float)countClusters);
    result.push_back((float)dimension);

    for (auto point : clusterCenters) {
        for (auto entry : point) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<int> KMeansJob::clusterCentersToBroadcast(std::vector<Point> reduceClusterCenters) {
    std::vector<int> result;
    for (auto point : reduceClusterCenters) {
        auto centerData = point.data();
        for (int entry = 0; entry < dimension; ++entry) {
            result.push_back(*((int*)(centerData + entry)));
        }
    }
    // LOG(V2_INFO, "                           reduce in clusterCentersToBroadcast: \n%s\n",
    //     dataToString(result).c_str());
    return result;
}

std::vector<KMeansJob::Point> KMeansJob::broadcastToClusterCenters(std::vector<int> reduce, bool withNumWorkers) {
    std::vector<Point> localClusterCentersResult;
    const int elementsCount = allReduceElementSize - countClusters;
    int* reduceData = reduce.data();

    if (!withNumWorkers) reduceData += countClusters;

    localClusterCentersResult.clear();
    localClusterCentersResult.resize(countClusters);
    for (int i = 0; i < countClusters; ++i) {
        localClusterCentersResult[i].assign(dimension, 0);
    }
    for (int i = 0; i < elementsCount; ++i) {
        localClusterCentersResult[i / dimension][i % dimension] = *((float*)(reduceData + i));
    }
    if (withNumWorkers) {
        countCurrentWorkers = *(reduceData + elementsCount);
        LOG(V2_INFO, "                           myIndex: %i countCurrentWorkers: %i\n", myIndex, countCurrentWorkers);
    }

    return localClusterCentersResult;
}

std::vector<int> KMeansJob::clusterCentersToReduce(std::vector<int> reduceSumMembers, std::vector<Point> reduceClusterCenters) {
    std::vector<int> result;
    std::vector<int> tempCenters;

    tempCenters = clusterCentersToBroadcast(reduceClusterCenters);

    for (auto entry : reduceSumMembers) {
        result.push_back(entry);
    }
    result.insert(result.end(), tempCenters.begin(), tempCenters.end());

    return std::move(result);
}

std::pair<std::vector<std::vector<float>>, std::vector<int>>
KMeansJob::reduceToclusterCenters(std::vector<int> reduce) {
    // auto [centers, counts] = reduceToclusterCenters(); //call example
    std::vector<int> localSumMembersResult;
    std::vector<Point> localClusterCentersResult;
    const int elementsCount = allReduceElementSize - countClusters;

    int* reduceData = reduce.data();

    localSumMembersResult.assign(countClusters, 0);
    for (int i = 0; i < countClusters; ++i) {
        // LOG(V2_INFO, "i: %d\n", i);
        localSumMembersResult[i] = reduceData[i];
    }

    localClusterCentersResult = broadcastToClusterCenters(reduce);

    return std::pair(std::move(localClusterCentersResult), std::move(localSumMembersResult));
}

std::vector<int> KMeansJob::aggregate(std::list<std::vector<int>> messages) {
    std::vector<std::vector<KMeansJob::Point>> centers;
    std::vector<std::vector<int>> counts;
    std::vector<int> tempSumMembers;
    std::vector<Point> tempClusterCenters;
    const int countMessages = messages.size();
    centers.resize(countMessages);
    counts.resize(countMessages);
    LOG(V2_INFO, "                         myIndex: %i countMessages: %d\n", myIndex, countMessages);
    for (int i = 0; i < countMessages; ++i) {
        auto data = reduceToclusterCenters(messages.front());
        messages.pop_front();
        centers[i] = data.first;
        counts[i] = data.second;

        LOG(V2_INFO, "                         myIndex: %i counts[%d] : \n%s\n", myIndex, i, dataToString(counts[i]).c_str());
    }
    tempSumMembers.assign(countClusters, 0);
    for (int i = 0; i < countMessages; ++i) {
        for (int j = 0; j < countClusters; ++j) {
            tempSumMembers[j] = tempSumMembers[j] + counts[i][j];

            // LOG(V2_INFO, "tempSumMembers[%d] + counts[%d][%d] : \n%d\n",j,i,j, tempSumMembers[j] + counts[i][j]);
            // LOG(V2_INFO, "tempSumMembers[%d] : \n%d\n",j, tempSumMembers[j]);
            // LOG(V2_INFO, "KRITcounts[%d] : \n%s\n",i, dataToString(tempSumMembers).c_str());
        }
    }
    tempClusterCenters.resize(countClusters);
    for (int i = 0; i < countClusters; ++i) {
        tempClusterCenters[i].assign(dimension, 0);
    }
    for (int i = 0; i < countMessages; ++i) {
        for (int j = 0; j < countClusters; ++j) {
            for (int k = 0; k < dimension; ++k) {
                tempClusterCenters[j][k] += centers[i][j][k] *
                                            (static_cast<float>(counts[i][j]) /
                                             static_cast<float>(tempSumMembers[j]));
            }
        }
    }

    return clusterCentersToReduce(tempSumMembers, tempClusterCenters);  // localSumMembers, tempClusterCenters
}
