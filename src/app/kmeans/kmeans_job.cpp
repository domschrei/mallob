#include "kmeans_job.hpp"

#include <iostream>
#include <random>
#include <thread>

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "comm/job_tree_basic_all_reduction.hpp"
#include "comm/msgtags.h"
#include "comm/mympi.hpp"
#include "kmeans_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"

KMeansJob::KMeansJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table)
    : Job(params, setup, table) {
}

void KMeansJob::appl_start() {
    _my_rank = getJobTree().getRank();
    _my_index = getJobTree().getIndex();
    _is_root = getJobTree().isRoot();

    _num_curr_workers = 1;
    if (_is_root) {
        LOG(V2_INFO, "%s : kmeans instance loaded\n", toStr());
    }
    LOG(V5_DEBG, "COMMSIZE: %i myRank: %i myIndex: %i\n",
        _num_curr_workers, _my_rank, _my_index);
    LOG(V5_DEBG, "Children: %i\n",
        this->getJobTree().getNumChildren());
    _data = getSerializedDescription(0)->data();
    _points_start = (float*)(_data + getDescription().getMetadataSize() + 3 * sizeof(int));
    LOG(V5_DEBG, "KMDBG myIndex: %i getting ready\n", _my_index);
    _base_msg = JobMessage(getId(), getContextId(),
                         getRevision(),
                         1,
                         MSG_BROADCAST_DATA);

    loadInstance();
    _cluster_membership.assign(_num_points, -1);

    _local_cluster_centers.resize(_num_clusters);
    _cluster_centers.resize(_num_clusters);
    for (int cluster = 0; cluster < _num_clusters; ++cluster) {
        _local_cluster_centers[cluster].assign(_dimension, 0);
        _cluster_centers[cluster].resize(_dimension);
    }
    LOG(V5_DEBG, "KMDBG myIndex: %i Ready!\n", _my_index);
    _loaded = true;
    if (_is_root) {
        doInitWork();
    }
}
void KMeansJob::initReducer(JobMessage& msg) {
    JobTree& tempJobTree = getJobTree();

    int lIndex = _my_index * 2 + 1;
    int rIndex = lIndex + 1;

    LOG(V5_DEBG, "KMDBG myIndex: %i !tempJobTree.hasLeftChild() %i lIndex in %i\n", _my_index, !tempJobTree.hasLeftChild(), (lIndex < _num_curr_workers));
    LOG(V5_DEBG, "KMDBG myIndex: %i !tempJobTree.hasRightChild() %i rIndex in %i\n", _my_index, !tempJobTree.hasRightChild(), (rIndex < _num_curr_workers));
    _work.clear();
    _work_done.clear();
    _work_done.push_back(_my_index);
    if (!tempJobTree.hasLeftChild()) {
        _left_done = true;
        if (lIndex < _num_curr_workers) {
            auto grandChilds = KMeansUtils::childIndexesOf(lIndex, _num_curr_workers);
            _work.push_back(lIndex);
            LOG(V5_DEBG, "KMDBG myIndex: %i push in %i\n", _my_index, lIndex);
            for (auto child : grandChilds) {
                _work.push_back(child);
                LOG(V5_DEBG, "KMDBG myIndex: %i push in %i\n", _my_index, child);
            }
        }
    }
    if (!tempJobTree.hasRightChild()) {
        _right_done = true;
        if ((rIndex < _num_curr_workers)) {
            auto grandChilds = KMeansUtils::childIndexesOf(rIndex, _num_curr_workers);
            _work.push_back(rIndex);
            LOG(V5_DEBG, "KMDBG myIndex: %i push in %i\n", _my_index, rIndex);
            for (auto child : grandChilds) {
                _work.push_back(child);
                LOG(V5_DEBG, "KMDBG myIndex: %i push in %i\n", _my_index, child);
            }
        }
    }
    // msg.payload = clusterCentersToBroadcast(clusterCenters);
    // msg.payload.push_back(countCurrentWorkers);

    _reducer.reset(new JobTreeBasicAllReduction(tempJobTree.getSnapshot(),
                                          JobMessage(getId(), getContextId(),
                                                     getRevision(),
                                                     1,
                                                     MSG_ALLREDUCE_CLAUSES),
                                          std::vector<int>(_all_red_elem_size, 0),
                                          folder));

    _reducer->setTransformationOfElementAtRoot(rootTransform);
    _reducer->disableBroadcast();
    LOG(V5_DEBG, "KMDBG myIndex: %i advanceCollective\n", _my_index);
    advanceCollective(msg, tempJobTree);
    if (tempJobTree.hasLeftChild() && !(lIndex < _num_curr_workers)) {
        _left_done = true;
        _base_msg.tag = MSG_ALLREDUCE_CLAUSES;
        _base_msg.payload = std::vector<int>(_all_red_elem_size, 0);

        LOG(V5_DEBG, "KMDBG myIndex: %i send fill left\n", _my_index);
        _reducer->receive(tempJobTree.getLeftChildNodeRank(), MSG_JOB_TREE_REDUCTION, _base_msg);
    }
    if (tempJobTree.hasRightChild() && !(rIndex < _num_curr_workers)) {
        _right_done = true;
        _base_msg.tag = MSG_ALLREDUCE_CLAUSES;
        _base_msg.payload = std::vector<int>(_all_red_elem_size, 0);
        LOG(V5_DEBG, "KMDBG myIndex: %i send fill right\n", _my_index);
        _reducer->receive(tempJobTree.getRightChildNodeRank(), MSG_JOB_TREE_REDUCTION, _base_msg);
    }

    _has_reducer = true;
}

void KMeansJob::sendRootNotification() {
    _init_send = false;
    _base_msg.tag = MSG_BROADCAST_DATA;
    _base_msg.payload = clusterCentersToBroadcast(_cluster_centers);
    _base_msg.payload.push_back(_num_curr_workers);
    LOG(V5_DEBG, "KMDBG myIndex: %i sendRootNotification0\n", _my_index);
    getJobTree().sendToRoot(_base_msg);
}
void KMeansJob::doInitWork() {
    _init_msg_task = ProcessWideThreadPool::get().addTask([&]() {
        setRandomStartCenters();
        _init_send = true;
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
    LOG(V5_DEBG, "KMDBG myIndex: %i i got SUSPENDED :( iter: %i\n", _my_index, _iterations_done);
    _base_msg.tag = MSG_ALLREDUCE_CLAUSES;
    _base_msg.returnedToSender = true;
    if (_my_index < 0) {
        _my_index = getJobTree().getIndex();
    }
    _base_msg.payload.assign(1, _my_index);
    getJobTree().sendToParent(_base_msg);
}
void KMeansJob::appl_resume() {
    LOG(V5_DEBG, "KMDBG myIndex: %i i got RESUMED :D iter: %i\n", _my_index, _iterations_done);

    reset();
}

void KMeansJob::reset() {
    _iterations_done = 0;
    _max_demand = -1;
    _max_demand_calculated = false;
    _finished_job = false;
    _is_root = false;
    _loaded = false;
    _init_send = false;
    _calculating_finished = false;
    _has_reducer = false;
    _left_done = false;
    _right_done = false;
    _skip_current_iter = false;
    _my_index = -2;
    _work.clear();
    _work_done.clear();
    _terminate = true;
    if (_init_msg_task.valid()) _init_msg_task.get();
    if (_calculating_task.valid()) _calculating_task.get();
    _reducer.reset();

    _terminate = false;
    appl_start();
}

JobResult&& KMeansJob::appl_getResult() {
    return std::move(_internal_result);
}
void KMeansJob::appl_terminate() {
    LOG(V5_DEBG, "KMDBG start terminate\n");
    _terminate = true;
    _reducer.reset();
}

KMeansJob::~KMeansJob() {
    if (_init_msg_task.valid()) _init_msg_task.get();
    if (_calculating_task.valid()) _calculating_task.get();
    LOG(V5_DEBG, "KMDBG end terminate\n");
}

void KMeansJob::appl_communicate() {
    if (!_loaded || _finished_job || _terminate) return;

    if (_is_root && _init_send) {
        sendRootNotification();
        LOG(V5_DEBG, "KMDBG Send Init ONCE!!!\n");
    }
    if (!_work.empty()) {
        // LOG(V5_DEBG, "KMDBG myIndex: %i !work.empty() TRUE\n", myIndex);
        if (_calculating_finished) {
            LOG(V5_DEBG, "KMDBG myIndex: %i calculatingFinished TRUE\n", _my_index);
            _calculating_finished = false;
            _calculating_task.get();
            const int currentIndex = _work[_work.size() - 1];
            _work.pop_back();
            _work_done.push_back(currentIndex);
            _calculating_task = ProcessWideThreadPool::get().addTask([&, cI = currentIndex]() {
                if (!_left_done) _left_done = (currentIndex == (_my_index * 2 + 1));
                if (!_right_done) _right_done = (currentIndex == (_my_index * 2 + 2));
                LOG(V5_DEBG, "KMDBG myIndex: %i Start Calc\n", _my_index);
                if (!_skip_current_iter) {
                    calcNearestCenter(metric, cI);
                }
                LOG(V5_DEBG, "KMDBG myIndex: %i End Calc childs\n", _my_index);

                _calculating_finished = true;
                LOG(V5_DEBG, "KMDBG myIndex: %i work.empty() %i calculatingFinished %i leftDone %i rightDone %i \n", _my_index, _work.empty(), _calculating_finished, _left_done, _right_done);
            });
        }
    }

    // LOG(V5_DEBG, "KMDBG myIndex: %i work.empty() %i calculatingFinished %i leftDone %i rightDone %i \n", myIndex, work.empty(), calculatingFinished, leftDone, rightDone);
    if (_work.empty() && _calculating_finished && _left_done && _right_done) {
        LOG(V5_DEBG, "KMDBG myIndex: %i all work Finished!!!\n", _my_index);

        if (_calculating_task.valid()) _calculating_task.get();

        _left_done = false;
        _right_done = false;
        LOG(V5_DEBG, "KMDBG myIndex: %i all work Finished2!!!\n", _my_index);
        if (!_skip_current_iter) {
            auto producer = [&]() {
                calcCurrentClusterCenters();

                // LOG(V5_DEBG, "clusterCenters: \n%s\n", dataToString(localClusterCenters).c_str());
                return clusterCentersToReduce(_local_sum_members, _local_cluster_centers);
            };
            (_reducer)->produce(producer);
            (_reducer)->advance();
        }
        _calculating_finished = false;
        LOG(V5_DEBG, "KMDBG myIndex: %i all work Finished END!!!\n", _my_index);
    };
    if (_has_reducer) {
        if (!_skip_current_iter) {
            if (_is_root && (_reducer)->hasResult()) {
                LOG(V5_DEBG, "KMDBG myIndex: %i received Result from Transform\n", _my_index);
                setClusterCenters((_reducer)->extractResult());
                _cluster_membership.assign(_num_points, -1);

                _base_msg.tag = MSG_BROADCAST_DATA;
                _base_msg.payload = clusterCentersToBroadcast(_cluster_centers);
                _base_msg.payload.push_back(_num_curr_workers);
                LOG(V5_DEBG, "KMDBG myIndex: %i sendRootNotification1\n", _my_index);
                getJobTree().sendToRoot(_base_msg);
                // only root...?

                // continue broadcasting

                // LOG(V5_DEBG, "KMDBG myIndex: %i clusterCenters: \n%s\n", getJobTree().getIndex(),
                //     dataToString(clusterCenters).c_str());
            }
            (_reducer)->advance();
        } else {
            LOG(V5_DEBG, "KMDBG Skip Iter\n");
            _skip_current_iter = false;
            _has_reducer = false;
            _left_done = false;
            _right_done = false;
            _reducer.reset();
            _cluster_membership.assign(_num_points, -1);
            _base_msg.tag = MSG_BROADCAST_DATA;
            _base_msg.payload = clusterCentersToBroadcast(_cluster_centers);
            _base_msg.payload.push_back(this->getVolume());
            LOG(V5_DEBG, "KMDBG myIndex: %i sendRootNotification2\n", _my_index);
            getJobTree().sendToRoot(_base_msg);
        }
    }
}

int KMeansJob::getIndex(int rank) {
    // LOG(V5_DEBG, "KMDBG myIndex: %i rank in: %i\n", myIndex, rank);
    if (getJobTree().hasLeftChild() && rank == getJobTree().getLeftChildNodeRank()) {
        // LOG(V5_DEBG, "KMDBG out L : %i\n", getJobTree().getLeftChildIndex());
        return getJobTree().getLeftChildIndex();
    }
    if (getJobTree().hasRightChild() && rank == getJobTree().getRightChildNodeRank()) {
        // LOG(V5_DEBG, "KMDBG out R : %i\n", getJobTree().getRightChildIndex());
        return getJobTree().getRightChildIndex();
    }
    if (!_is_root && rank == getJobTree().getParentNodeRank()) {
        // LOG(V5_DEBG, "KMDBG out P : %i\n", getJobTree().getParentIndex());
        return getJobTree().getParentIndex();
    }
    if (rank == getJobTree().getRank()) {
        // LOG(V5_DEBG, "KMDBG out Me : %i\n", getJobTree().getIndex());
        return getJobTree().getIndex();
    }
    return -1;
}

void KMeansJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
    int sourceIndex = getIndex(source);
    LOG(V5_DEBG, "KMDBG myIndex: %i source: %i mpiTag: %i payloadSize: %lu\n", _my_index, sourceIndex, mpiTag, msg.payload.size());
    if (!_loaded) {
        LOG(V5_DEBG, "KMDBG myIndex: %i not Ready: %i mpiTag: %i\n", _my_index, sourceIndex, mpiTag);
        if (_my_index < 0) {
            _my_index = getJobTree().getIndex();
        }
        msg.payload.assign(1, _my_index);
        msg.returnToSender(source, mpiTag);
        return;
    }
    if (msg.returnedToSender) {
        // I will do it
        std::vector<int> missingChilds;
        LOG(V5_DEBG, "KMDBG myIndex: %i returnFrom: %i mpiTag: %i\n", _my_index, sourceIndex, mpiTag);
        LOG(V5_DEBG, "KMDBG !leftDone %i !getJobTree().hasLeftChild() %i getJobTree().getLeftChildIndex() < countCurrentWorkers %i\n", !_left_done, !getJobTree().hasLeftChild(), getJobTree().getLeftChildIndex() < _num_curr_workers);
        LOG(V5_DEBG, "KMDBG !leftDone %i\n", msg.payload[0]);
        if (!_left_done && (msg.payload[0] == getJobTree().getLeftChildIndex() || !getJobTree().hasLeftChild()) && getJobTree().getLeftChildIndex() < _num_curr_workers) {
            // missing left child
            _left_done = true;
            missingChilds.push_back(getJobTree().getLeftChildIndex());
            LOG(V5_DEBG, "KMDBG myIndex: %i LsourceIndex: %i\n", _my_index, sourceIndex);
        }
        if (!_right_done && (msg.payload[0] == getJobTree().getRightChildIndex() || !getJobTree().hasRightChild()) && getJobTree().getRightChildIndex() < _num_curr_workers) {
            // missing right child
            _right_done = true;
            missingChilds.push_back(getJobTree().getRightChildIndex());
            LOG(V5_DEBG, "KMDBG myIndex: %i RsourceIndex: %i\n", _my_index, sourceIndex);
        }
        for (auto child : missingChilds) {
            auto grandChilds = KMeansUtils::childIndexesOf(child, _num_curr_workers);
            msg.payload = std::vector<int>(_all_red_elem_size, 0);
            msg.tag = MSG_ALLREDUCE_CLAUSES;
            (_reducer)->receive(source, MSG_JOB_TREE_REDUCTION, msg);
            _work.push_back(child);
            for (auto child : grandChilds) {
                _work.push_back(child);
            }
        }
        return;
    }

    if (mpiTag == MSG_JOB_TREE_REDUCTION) {
        LOG(V5_DEBG, "KMDBG myIndex: %i MSG_JOB_TREE_REDUCTION \n", _my_index);
        LOG(V5_DEBG, "KMDBG myIndex: %i sourceIndex: %i (myIndex * 2 + 1): %i \n", _my_index, sourceIndex, (_my_index * 2 + 1));
        LOG(V5_DEBG, "KMDBG myIndex: %i source: %i getJobTree().getLeftChildNodeRank(): %i \n", _my_index, source, getJobTree().getLeftChildNodeRank());
        if (!_left_done) _left_done = (source == getJobTree().getLeftChildNodeRank());
        if (!_right_done) _right_done = (source == getJobTree().getRightChildNodeRank());
        if (_has_reducer) {
            LOG(V5_DEBG, "KMDBG myIndex: %i I have Reducer\n", _my_index);

            bool answear = (_reducer)->receive(source, mpiTag, msg);
            LOG(V5_DEBG, "KMDBG myIndex: %i bool:%i\n", _my_index, answear);
        }
    }

    if (msg.tag == MSG_BROADCAST_DATA) {
        LOG(V5_DEBG, "KMDBG myIndex: %i MSG_BROADCAST_DATA \n", _my_index);
        LOG(V5_DEBG, "KMDBG myIndex: %i Workers: %i!\n", _my_index, _num_curr_workers);
        setClusterCenters(msg.payload);
        if (_my_index < _num_curr_workers) {
            initReducer(msg);
            _cluster_membership.assign(_num_points, -1);

            // continue broadcasting

            // LOG(V5_DEBG, "KMDBG myIndex: %i clusterCenters: \n%s\n", getJobTree().getIndex(),
            //     dataToString(clusterCenters).c_str());
            _calculating_task = ProcessWideThreadPool::get().addTask([&]() {
                LOG(V5_DEBG, "KMDBG myIndex: %i Start Calc\n", _my_index);
                calcNearestCenter(metric, _my_index);
                LOG(V5_DEBG, "KMDBG myIndex: %i End Calc basic\n", _my_index);
                _calculating_finished = true;
            });
        } else {
            LOG(V5_DEBG, "KMDBG myIndex: %i not in range\n", _my_index);
            msg.payload.assign(1, _my_index);
            if (_my_index < 0) {
                _my_index = getJobTree().getIndex();
            }
            msg.returnToSender(source, mpiTag);
            return;
        }
    }
}
void KMeansJob::advanceCollective(JobMessage& msg, JobTree& jobTree) {
    // Broadcast to children

    if (jobTree.hasLeftChild() && jobTree.getLeftChildIndex() < _num_curr_workers) {
        LOG(V5_DEBG, "KMDBG myIndex: %i sendTo %i type: %i\n", _my_index, jobTree.getLeftChildIndex(), MSG_SEND_APPLICATION_MESSAGE);
        getJobTree().sendToLeftChild(msg);
    }
    if (jobTree.hasRightChild() && jobTree.getRightChildIndex() < _num_curr_workers) {
        LOG(V5_DEBG, "KMDBG myIndex: %i sendTo %i type: %i\n", _my_index, jobTree.getRightChildIndex(), MSG_SEND_APPLICATION_MESSAGE);
        getJobTree().sendToRightChild(msg);
    }
}
void KMeansJob::appl_dumpStats() {}
void KMeansJob::appl_memoryPanic() {}
void KMeansJob::loadInstance() {
    int* metadata = (int*)(_data + getDescription().getMetadataSize());
    _num_clusters = metadata[0];
    _dimension = metadata[1];
    _num_points = metadata[2];
    LOG(V5_DEBG, "                          countClusters: %i dimension %i pointsCount: %i\n", _num_clusters, _dimension, _num_points);
    _all_red_elem_size = (_dimension + 1) * _num_clusters;
    setMaxDemand();
}

void KMeansJob::setRandomStartCenters() {  // use RNG with seed
    _cluster_centers.clear();
    _cluster_centers.resize(_num_clusters);
    std::vector<int> selectedPoints;
    selectedPoints.reserve(_num_clusters);
    int randomNumber;
    std::random_device rd;
    //std::mt19937 gen(42);  // seed 42 for testing
    std::mt19937 gen(rd());  
    std::uniform_int_distribution<> distr(0, _num_points - 1);
    for (int i = 0; i < _num_clusters; ++i) {
        randomNumber = distr(gen);
        while (std::find(selectedPoints.begin(), selectedPoints.end(), randomNumber) != selectedPoints.end()) {
            randomNumber = distr(gen);
        }
        LOG(V5_DEBG, "                          randomNumber: %i\n", randomNumber);
        selectedPoints.push_back(randomNumber);
        _cluster_centers[i].reserve(_dimension);
        for (int d = 0; d < _dimension; d++) {
            _cluster_centers[i].push_back(getKMeansData(randomNumber)[d]);
        }
    }
}

void KMeansJob::calcNearestCenter(std::function<float(const float* p1, const float* p2, const size_t dim)> metric, int intervalId) {
    int currentCluster;
    float currentDistance;
    float distanceToCluster;
    // while own or child slices todo
    int startIndex = static_cast<int>(static_cast<float>(_num_points) * (static_cast<float>(intervalId) / static_cast<float>(_num_curr_workers)));
    int endIndex = static_cast<int>(static_cast<float>(_num_points) * (static_cast<float>(intervalId + 1) / static_cast<float>(_num_curr_workers)));
    LOG(V5_DEBG, "KMDBG MI: %i intervalId: %i PC: %i cW: %i start:%i end:%i!!      iter:%i k:%i \n", _my_index, intervalId, _num_points, _num_curr_workers, startIndex, endIndex, _iterations_done, _num_clusters);
    for (int pointID = startIndex; pointID < endIndex; ++pointID) {
        if (_terminate) return;
        // LOG(V1_WARN, "(pointID / endIndex) < 0.25: %i iAmRoot: %i countCurrentWorkers == 1: %i std::find(work.begin(), work.end(), 1) != work.end() && std::find(work.begin(), work.end(), 2) != work.end()): %i leftDone && rightDone:%i this->getVolume() > 1:%i\n", (pointID / endIndex) < 0.25, iAmRoot, countCurrentWorkers == 1, std::find(work.begin(), work.end(), 1) != work.end() && std::find(work.begin(), work.end(), 2) != work.end(), leftDone && rightDone,  this->getVolume() > 1);

        if (((float)pointID / endIndex) < 0.25 &&
            _is_root &&
            (_num_curr_workers == 1 ||
             (std::find(_work.begin(), _work.end(), 1) != _work.end() && std::find(_work.begin(), _work.end(), 2) != _work.end())) &&
            (_left_done && _right_done) &&
            this->getVolume() > 1) {
            LOG(V3_VERB, "%s : will skip Iter\n", toStr());
            _skip_current_iter = true;
            _work.clear();
            _work_done.clear();
            return;
        }
        currentCluster = -1;
        currentDistance = std::numeric_limits<float>::infinity();
        for (int clusterID = 0; clusterID < _num_clusters; ++clusterID) {
            distanceToCluster = metric(getKMeansData(pointID), _cluster_centers[clusterID].data(), _dimension);
            if (distanceToCluster < currentDistance) {
                currentCluster = clusterID;
                currentDistance = distanceToCluster;
            }
        }

        _cluster_membership[pointID] = currentCluster;
    }
    LOG(V5_DEBG, "KMDBG MI: %i intervalId: %i PC: %i cW: %i start:%i end:%i COMPLETED iter:%i \n", _my_index, intervalId, _num_points, _num_curr_workers, startIndex, endIndex, _iterations_done);
}

void KMeansJob::calcCurrentClusterCenters() {
    _old_cluster_centers = _cluster_centers;

    countMembers();

    for (int cluster = 0; cluster < _num_clusters; ++cluster) {
        auto currentCenter = _local_cluster_centers[cluster].data();
        for (int d = 0; d < _dimension; ++d) {
            currentCenter[d] = 0;
        }
    }
    for (auto workIndex : _work_done) {
        int startIndex = static_cast<int>(static_cast<float>(_num_points) * (static_cast<float>(workIndex) / static_cast<float>(_num_curr_workers)));

        int endIndex = static_cast<int>(static_cast<float>(_num_points) * (static_cast<float>(workIndex + 1) / static_cast<float>(_num_curr_workers)));
        for (int pointID = startIndex; pointID < endIndex; ++pointID) {
            // if (clusterMembership[pointID] != -1) {
            int clusterId = _cluster_membership[pointID];
            auto currentCenter = _local_cluster_centers[clusterId].data();
            float divisor = static_cast<float>(_local_sum_members[clusterId]);
            auto currentDataPoint = getKMeansData(pointID);
            for (int d = 0; d < _dimension; ++d) {
                currentCenter[d] += currentDataPoint[d] / divisor;
            }
        }
    }
    ++_iterations_done;
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
    _local_sum_members.assign(_num_clusters, 0);
    for (int i = 0; i < _num_points; ++i) {
        int clusterID = _cluster_membership[i];
        if (clusterID != -1) {
            _local_sum_members[clusterID] += 1;
        }
    }
    LOG(V5_DEBG, "KMDBG myIndex: %i sumMembers: %s\n",
        _my_index, dataToString(_local_sum_members).c_str());
}

bool KMeansJob::centersChanged() {
    if (_iterations_done == 0) {
        return true;
    }
    for (int k = 0; k < _num_clusters; ++k) {
        for (int d = 0; d < _dimension; ++d) {
            if (_cluster_centers[k][d] != _old_cluster_centers[k][d]) {
                return true;
            }
        }
    }
    return false;
}

bool KMeansJob::centersChanged(float factor) {
    if (_iterations_done == 0) {
        return true;
    }
    for (int k = 0; k < _num_clusters; ++k) {
        for (int d = 0; d < _dimension; ++d) {
            float distance = fabs(_cluster_centers[k][d] - _old_cluster_centers[k][d]);
            float upperBoundDistance = factor * (fabs(_cluster_centers[k][d] + _old_cluster_centers[k][d]) / 2);
            if (distance > upperBoundDistance) {
                LOG(V5_DEBG, "KMDBG Dist: %f upperBoundDistance: %f\n", distance, upperBoundDistance);
                return true;
            }
        }
    }
    return false;
}

std::vector<float> KMeansJob::clusterCentersToSolution() {
    std::vector<float> result;
    result.reserve(_num_clusters * _dimension + 2);
    result.push_back((float)_num_clusters);
    result.push_back((float)_dimension);

    for (auto point : _cluster_centers) {
        for (auto entry : point) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<int> KMeansJob::clusterCentersToBroadcast(const std::vector<Point>& reduceClusterCenters) {
    std::vector<int> result;
    result.resize(reduceClusterCenters.size() * _dimension);
    int i = 0;
    for (auto point : reduceClusterCenters) {
        auto centerData = point.data();
        for (int entry = 0; entry < _dimension; ++entry) {
            result[i++] = (*((int*)(centerData + entry)));
        }
    }
    // LOG(V5_DEBG, "KMDBG reduce in clusterCentersToBroadcast: \n%s\n",
    //     dataToString(result).c_str());
    return result;
}

std::vector<KMeansJob::Point> KMeansJob::broadcastToClusterCenters(const std::vector<int>& reduce) {
    std::vector<Point> localClusterCentersResult;
    const int elementsCount = _all_red_elem_size - _num_clusters;
    const int* reduceData = reduce.data();

    reduceData += _num_clusters;

    localClusterCentersResult.resize(_num_clusters);
    for (int k = 0; k < _num_clusters; ++k) {
        localClusterCentersResult[k].resize(_dimension);
        auto currentCenter = localClusterCentersResult[k].data();
        auto receivedCenter = reduceData + k * _dimension;
        for (int d = 0; d < _dimension; ++d) {
            currentCenter[d] = *((float*)(receivedCenter + d));
        }
    }

    return localClusterCentersResult;
}
void KMeansJob::setClusterCenters(const std::vector<int>& reduce) {
    const int elementsCount = _all_red_elem_size - _num_clusters;
    for (int k = 0; k < _num_clusters; ++k) {
        assert(k < _cluster_centers.size());
        auto& currentCenter = _cluster_centers[k];
        for (int d = 0; d < _dimension; ++d) {
            assert(k*_dimension + d < reduce.size());
            int val = reduce[k*_dimension + d];
            currentCenter[d] = *((float*)(&val));
        }
    }
    _num_curr_workers = reduce[elementsCount];
    LOG(V5_DEBG, "KMDBG myIndex: %i countCurrentWorkers: %i\n", _my_index, _num_curr_workers);
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
    const int elementsCount = _all_red_elem_size - _num_clusters;

    const int* reduceData = reduce.data();

    localSumMembersResult.assign(_num_clusters, 0);
    for (int i = 0; i < _num_clusters; ++i) {
        // LOG(V5_DEBG, "i: %d\n", i);
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
    counts.reserve(countMessages * _num_clusters);
    auto message = messages.begin();
    // LOG(V5_DEBG, "                         myIndex: %i countMessages: %d\n", myIndex, countMessages);
    for (int i = 0; i < countMessages; ++i) {
        auto data = reduceToclusterCenters(*message);
        ++message;
        centers[i] = std::move(data.first);
        counts.insert(counts.end(), data.second.begin(), data.second.end());

        // LOG(V5_DEBG, "                         myIndex: %i counts[%d] : \n%s\n", myIndex, i, dataToString(counts[i]).c_str());
        //  LOG(V5_DEBG, "clusterCentersI: \n%s\n", dataToString(centers[i]).c_str());
    }

    tempSumMembers.resize(_num_clusters);
    for (int i = 0; i < countMessages; ++i) {
        for (int j = 0; j < _num_clusters; ++j) {
            tempSumMembers[j] = tempSumMembers[j] + counts[i * _num_clusters + j];

            // LOG(V5_DEBG, "tempSumMembers[%d] + counts[%d][%d] : \n%d\n",j,i,j, tempSumMembers[j] + counts[i][j]);
            // LOG(V5_DEBG, "tempSumMembers[%d] : \n%d\n",j, tempSumMembers[j]);
            // LOG(V5_DEBG, "KRITcounts[%d] : \n%s\n",i, dataToString(tempSumMembers).c_str());
        }
    }
    tempClusterCenters.resize(_num_clusters);
    for (int i = 0; i < _num_clusters; ++i) {
        tempClusterCenters[i].assign(_dimension, 0);
    }

    float ratio;
    float currentSum;

    for (int i = 0; i < countMessages; ++i) {
        std::vector<KMeansJob::Point>& currentMsg = centers[i];
        for (int j = 0; j < _num_clusters; ++j) {
            currentSum = static_cast<float>(tempSumMembers[j]);
            if (currentSum != 0) {
                KMeansJob::Point& currentCenter = tempClusterCenters[j];
                KMeansJob::Point& currentMsgCenter = currentMsg[j];
                ratio = static_cast<float>(counts[i * _num_clusters + j]) /
                        currentSum;
                for (int k = 0; k < _dimension; ++k) {
                    currentCenter[k] += currentMsgCenter[k] * ratio;
                }
            }
        }
    }

    return clusterCentersToReduce(tempSumMembers, tempClusterCenters);  // localSumMembers, tempClusterCenters
}