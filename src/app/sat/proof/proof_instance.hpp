
#pragma once

#include <algorithm>

#include "app/sat/proof/reverse_lrat_parser.hpp"
#include "util/external_priority_queue.hpp"
#include "util/sys/thread_pool.hpp"

class ProofInstance {

private:
    const int _instance_id;
    const int _num_instances;
    bool _winning_instance;

    ReverseLratParser _parser;
    ReverseLratParser::LratLine _current_line;
    ExternalPriorityQueue<LratClauseId> _frontier;
    ExternalPriorityQueue<LratClauseId> _backlog;

    std::vector<LratClauseId> _local_epoch_starts;
    std::vector<LratClauseId> _local_epoch_offsets;
    std::vector<LratClauseId> _global_epoch_starts;
    int _current_epoch;

    std::ofstream _output;

    std::future<void> _work_future;
    bool _work_done = true;
    std::vector<LratClauseId> _outgoing_clause_ids;
    bool _finished = false;

public:
    ProofInstance(int instanceId, int numInstances, 
        const std::string& proofFilename, int finalEpoch, 
        int winningInstance, const std::vector<LratClauseId>& localEpochStarts, 
        const std::vector<LratClauseId>& localEpochOffsets,
        const std::string& outputFilename) :
            _instance_id(instanceId), _num_instances(numInstances),
            _winning_instance(winningInstance == instanceId),
            _parser(proofFilename), _local_epoch_starts(localEpochStarts), 
            _local_epoch_offsets(localEpochOffsets), _current_epoch(finalEpoch),
            _output(outputFilename) {
        
        _global_epoch_starts.resize(_local_epoch_starts.size());
        for (size_t i = 0; i < _global_epoch_starts.size(); ++i) {
            _global_epoch_starts[i] = _local_epoch_starts[i] + _local_epoch_offsets[i];
        }
    }

    void advance(const std::shared_ptr<std::vector<LratClauseId>>& incomingClauseIds) {
        _work_done = false;
        _work_future = ProcessWideThreadPool::get().addTask([this, incomingClauseIds]() {
            handleIncomingClauseIds(*incomingClauseIds);
            readEpoch();
            if (!_finished) prepareNextOutgoingClauseIds();
            _work_done = true;
        });
    }

    bool ready() const {return _work_done;}

    std::vector<LratClauseId>&& extractNextOutgoingClauseIds() {
        assert(ready());
        _work_future.get();

        // Proceed with the next (earlier) epoch
        assert(_current_epoch > 0);
        _current_epoch--;

        return std::move(_outgoing_clause_ids);
    }

    bool finished() const {return _work_done && _finished;}

private:

    void handleIncomingClauseIds(const std::vector<LratClauseId>& clauseIds) {
        
        // Import self-produced clauses
        for (auto id : clauseIds) {
            if (isSelfProducedClause(id)) _frontier.push(id);
        }
    }

    void readEpoch() {
        if (!_current_line.valid() && _parser.hasNext()) 
            _current_line = _parser.next();
        
        while (_current_line.valid()) {

            alignSelfProducedClauseIds(_current_line);
            auto id = _current_line.id;

            int epoch = getClauseEpoch(id);
            if (epoch != _current_epoch) {
                // check if it is from a future epoch (which would be an error)
                assert(epoch < _current_epoch);
                // stop reading because a former epoch has been reached
                break; 
            }

            if (_frontier.top() == id || 
                    (_current_line.literals.empty() && _winning_instance)) {
                // Clause derivation is necessary for the combined proof

                // Output the line
                auto lineStr = _current_line.toStr();
                _output.write(lineStr.c_str(), lineStr.size());

                // Traverse clause hints
                for (auto hintId : _current_line.hints) {
                    if (isSelfProducedClause(hintId)) {
                        _frontier.push(hintId);
                    } else if (!isOriginalClause(hintId)) {
                        _backlog.push(hintId);
                    }
                }

                // Remove all instances of this ID from the frontier
                while (_frontier.top() == id) _frontier.pop();
            } else {
                // Next necessary clause ID must be smaller -
                // Ignore this line
                assert(_frontier.top() < id);
            }

            // Get next proof line
            if (_parser.hasNext()) _current_line = _parser.next();
            else _current_line.id = -1;
        }

        if (_current_epoch == 0) {
            // End of the procedure reached!
            
            // -- the proof file must have been read completely
            assert(!_current_line.valid());
            assert(!_parser.hasNext());

            // -- there may not be any underived clauses left
            assert(_frontier.empty());
            assert(_backlog.empty());

            _finished = true;
        }
    }

    void prepareNextOutgoingClauseIds() {

        _outgoing_clause_ids.clear();

        while (!_backlog.empty()) {
            long id = _backlog.top();
            
            int epoch = getClauseEpoch(id);
            if (epoch != _current_epoch-1) {
                // check if it is from a future epoch (which would be an error)
                assert(epoch < _current_epoch-1);
                // stop reading because a former epoch has been reached
                break;
            }

            // Found an external clause ID from the prior epoch
            _outgoing_clause_ids.push_back(id);

            while (_backlog.top() == id) _backlog.pop();
        }
    }

    void alignSelfProducedClauseIds(ReverseLratParser::LratLine& line) {
        alignClauseId(line.id);
        for (auto& hint : line.hints) alignClauseId(hint);
    }
    void alignClauseId(LratClauseId& id) {
        if (isSelfProducedClause(id)) {
            int epoch = getClauseEpoch(id);
            id += _local_epoch_offsets[epoch];
        }
    }

    bool isOriginalClause(LratClauseId clauseId) {
        return clauseId < _global_epoch_starts[0];
    }
    bool isSelfProducedClause(LratClauseId clauseId) {
        return !isOriginalClause(clauseId) && clauseId % _num_instances == _instance_id;
    }
    LratClauseId convertSelfProducedClauseId(LratClauseId clauseId) {
        int epoch = _current_epoch;
        while (clauseId < _local_epoch_starts[epoch]) --epoch;
        return clauseId + _local_epoch_offsets[epoch];
    }
    
    int getClauseEpoch(LratClauseId clauseId) {
        auto it = std::lower_bound(_global_epoch_starts.begin(), _global_epoch_starts.end(), clauseId);
        assert(it != _global_epoch_starts.end());
        auto epoch = std::distance(std::begin(_global_epoch_starts), it);
        return epoch;
    }
};
