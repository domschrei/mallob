
#ifndef DOMPASCH_MALLOB_DISTRIBUTED_BFS_HPP
#define DOMPASCH_MALLOB_DISTRIBUTED_BFS_HPP

#include <vector>
#include <map>

#include "data/serializable.hpp"
#include "data/job_transfer.hpp"
#include "comm/mympi.hpp"
#include "util/logger.hpp"
#include "util/hashing.hpp"

struct BFSMessage : public Serializable {
    int depth = 0;
    int maxDepth;
    int answer = -1;
    JobRequest request;

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> packed(3*sizeof(int));
        int i = 0, n;
        n = sizeof(int); memcpy(packed.data()+i, &depth, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &maxDepth, n); i += n;
        n = sizeof(int); memcpy(packed.data()+i, &answer, n); i += n;
        auto reqPacked = request.serialize();
        packed.insert(packed.end(), reqPacked.begin(), reqPacked.end());
        return packed;
    }

    BFSMessage& deserialize(const std::vector<uint8_t> &packed) override {
        int i = 0, n;
        n = sizeof(int); memcpy(&depth, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&maxDepth, packed.data()+i, n); i += n;
        n = sizeof(int); memcpy(&answer, packed.data()+i, n); i += n;
        std::vector<uint8_t> reqPacked;
        reqPacked.insert(reqPacked.end(), packed.begin()+i, packed.end());
        request.deserialize(reqPacked);
        return *this;
    }
};

struct BFS {
    int parentRank = -1;
    size_t numAnswers = 0;
    int answer = -1;
    int depth;
    bool completed = false;
};

struct JobRequestHasher {
    size_t operator()(const JobRequest& req) const {
        size_t h = 1;
        hash_combine(h, req.revision);
        hash_combine(h, req.jobId);
        hash_combine(h, req.numHops);
        hash_combine(h, req.requestedNodeIndex);
        hash_combine(h, req.requestingNodeRank);
        hash_combine(h, req.rootRank);
        hash_combine(h, req.timeOfBirth);
        return h;
    }
};

class DistributedBFS {

private:
    std::vector<int>& _my_successors;
    std::function<int(const JobRequest&)> _visit_callback;
    std::function<void(const JobRequest&, int)> _result_callback;
    robin_hood::unordered_map<JobRequest, BFS, JobRequestHasher> _open_searches;

public:
    DistributedBFS(std::vector<int>& successors, 
            std::function<int(const JobRequest&)> visitCallback, 
            std::function<void(const JobRequest&, int)> resultCallback) :
        _my_successors(successors), _visit_callback(visitCallback), 
        _result_callback(resultCallback) {}

    void startSearch(JobRequest& req, int maxDepth) {
        if (maxDepth == 0) return;
        _open_searches[req].parentRank = MyMpi::rank(MPI_COMM_WORLD);
        _open_searches[req].numAnswers = 1; // myself
        _open_searches[req].depth = 0;
        BFSMessage msg;
        msg.maxDepth = maxDepth;
        msg.request = req;
        auto packed = msg.serialize();
        for (int rank : _my_successors) {
            LOG_ADD_DEST(V5_DEBG, "BFS (d=%i) query", rank, msg.depth);
            MyMpi::isend(rank, MSG_REQUEST_IDLE_NODE_BFS, std::move(packed));
        }
    }

    void handle(MessageHandle& handle) {
        
        assert(handle.source >= 0);
        assert(handle.tag == MSG_REQUEST_IDLE_NODE_BFS || handle.tag == MSG_ANSWER_IDLE_NODE_BFS);
        BFSMessage msg = Serializable::get<BFSMessage>(handle.getRecvData());
        
        bool doAnswer = false;
        if (handle.tag == MSG_REQUEST_IDLE_NODE_BFS) {  
            msg.depth++;

            // Do I already participate in a BFS for this request? 
            if (_open_searches.count(msg.request)) {
                auto& search = _open_searches[msg.request];
                if (search.depth <= msg.depth) {
                    // This message comes from an equally deep or deeper parent
                    // than the previous message: Just send back negative answer
                    msg.answer = -1;
                    LOG_ADD_DEST(V5_DEBG, "BFS (d=%i) answer %i", handle.source, msg.depth, msg.answer);
                    MyMpi::isend(handle.source, MSG_ANSWER_IDLE_NODE_BFS, msg);
                    return;
                }
            }

            // Visit
            int myAnswer = _visit_callback(msg.request);
            auto& search = _open_searches[msg.request];
            search.completed = false;
            search.parentRank = handle.source;
            search.numAnswers = 1;
            search.answer = myAnswer;
            search.depth = msg.depth;
            
            if (myAnswer >= 0 || msg.depth == msg.maxDepth) {
                // Max. depth reached or answer found: Switch to answering BFS
                search.completed = true;
                msg.answer = myAnswer;
                doAnswer = true;
            } else {
                // Explore successor nodes
                for (int rank : _my_successors) {
                    LOG_ADD_DEST(V5_DEBG, "BFS (d=%i) query %s", rank, msg.depth, msg.request.toStr().c_str());
                    MyMpi::isend(rank, MSG_REQUEST_IDLE_NODE_BFS, msg);
                }
            }
        }

        if (handle.tag == MSG_ANSWER_IDLE_NODE_BFS) {
            msg.depth--;
            
            // Past, obsolete BFS? Discard.
            if (!_open_searches.count(msg.request) || _open_searches[msg.request].depth != msg.depth
                || _open_searches[msg.request].completed) {
                LOG_ADD_SRC(V5_DEBG, "Obsolete BFS %s", handle.source, msg.request.toStr().c_str());
                return;
            }

            // Unvisit
            assert(_open_searches.count(msg.request));
            auto& search = _open_searches[msg.request];
            if (search.answer == -1) search.answer = msg.answer;
            search.numAnswers++;

            if (search.answer >= 0 || search.numAnswers == _my_successors.size()+1) {
                // Finished - ready to send up
                // (do not wait for more messages if there is already some positive answer)
                search.completed = true;
                msg.answer = search.answer;
                doAnswer = true;
            }
        }

        // Send an answer to the parent
        if (doAnswer) {
            assert(_open_searches.count(msg.request));
            int parent = _open_searches[msg.request].parentRank;
            assert(parent >= 0);
            if (parent == MyMpi::rank(MPI_COMM_WORLD)) {
                // Answers arrived at "root" of the BFS
                _result_callback(msg.request, _open_searches[msg.request].answer);
            } else {
                // Go back to predecessor node
                LOG_ADD_DEST(V5_DEBG, "BFS (d=%i) answer %i", parent, msg.depth, msg.answer);
                MyMpi::isend(parent, MSG_ANSWER_IDLE_NODE_BFS, msg);
            }
        }
    }

    void collectGarbage(int balancingEpoch) {
        auto it = _open_searches.begin();
        while (it != _open_searches.end()) {
            auto& [req, bfs] = *it;
            if (req.balancingEpoch < balancingEpoch) {
                it = _open_searches.erase(it);
            } else ++it;
        }
    }
};

#endif
