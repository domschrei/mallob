
#pragma once

#include <algorithm>

#include "app/sat/proof/reverse_lrat_parser.hpp"
#include "util/external_priority_queue.hpp"
#include "util/sys/thread_pool.hpp"

/*
The contract of an instance of this class looks as follows:
* Construct the instance
* Call advance(ptr) where ptr points to an empty vector
* While finished() returns false:
  * Query ready() until true
  * Call extractNextOutgoingClauseIds() and aggregate the result
    with the according results from all participating processes
    into a single (sorted) vector of unique clause IDs
  * Call advance(ptr) with ptr pointing to the aggregated vector
*/
class ProofInstance {

private:
    const int _instance_id;
    const int _num_instances;
    int _original_num_clauses;
    bool _winning_instance;

    ReverseLratParser _parser;
    LratLine _current_line;
    bool _current_line_aligned = false;
    ExternalPriorityQueue<LratClauseId> _frontier;
    ExternalPriorityQueue<LratClauseId> _backlog;

    std::vector<LratClauseId> _local_epoch_starts;
    std::vector<LratClauseId> _local_epoch_offsets;
    std::vector<LratClauseId> _global_epoch_starts;
    int _current_epoch;

    std::string _output_filename;
    std::ofstream _output;

    std::future<void> _work_future;
    bool _work_done = true;
    std::vector<LratClauseId> _outgoing_clause_ids;
    bool _finished = false;

    unsigned long _num_traced_clauses = 0;

public:
    ProofInstance(int instanceId, int numInstances, int originalNumClauses,
        const std::string& proofFilename, int finalEpoch, 
        int winningInstance, const std::vector<LratClauseId>& globalEpochStarts, 
        std::vector<LratClauseId>&& localEpochStarts, 
        std::vector<LratClauseId>&& localEpochOffsets, const std::string& outputFilename) :
            _instance_id(instanceId), _num_instances(numInstances), 
            _original_num_clauses(originalNumClauses),
            _winning_instance(winningInstance == instanceId),
            _parser(proofFilename), _local_epoch_starts(localEpochStarts), 
            _local_epoch_offsets(localEpochOffsets), _global_epoch_starts(globalEpochStarts),
            _current_epoch(finalEpoch),
            _output_filename(outputFilename), _output(outputFilename) {}

    void advance(const LratClauseId* clauseIdsData, size_t clauseIdsSize) {
        _work_done = false;
        _work_future = ProcessWideThreadPool::get().addTask([this, clauseIdsData, clauseIdsSize]() {
            handleIncomingClauseIds(clauseIdsData, clauseIdsSize);
            readEpoch();
            if (!_finished) prepareNextOutgoingClauseIds();
            _work_done = true;
        });
    }

    bool ready() const {return _work_done;}

    std::vector<LratClauseId>&& extractNextOutgoingClauseIds() {
        assert(ready());
        _work_future.get();
        std::string clsStr;
        for (auto id : _outgoing_clause_ids) clsStr += std::to_string(id) + " ";
        LOG(V2_INFO, "Proof instance %i exporting %i IDs { %s}\n", _instance_id, _outgoing_clause_ids.size(), clsStr.c_str());
        return std::move(_outgoing_clause_ids);
    }

    bool finished() const {return _work_done && _finished;}

    std::string getOutputFilename() const {return _output_filename;}

private:

    void handleIncomingClauseIds(const LratClauseId* clauseIdsData, size_t clauseIdsSize) {
        
        // Import self-produced clauses
        int numSelfClauses = 0;
        for (size_t i = 0; i < clauseIdsSize; i++) {
            LratClauseId id = clauseIdsData[i];
            if (isSelfProducedClause(id)) {
                _frontier.push(id);
                numSelfClauses++;
            }
        }
        LOG(V2_INFO, "Proof instance %i accepted %i self clauses to trace\n", 
            _instance_id, numSelfClauses);
    }

    void readEpoch() {

        LOG(V2_INFO, "Proof instance %i reading epoch %i\n", _instance_id, _current_epoch);
        int numReadLines = 0;

        if (!_current_line.valid() && _parser.hasNext()) {
            _current_line = _parser.next();
            _current_line_aligned = false;
        } 
        
        LratClauseId id = std::numeric_limits<unsigned long>::max();
        while (_current_line.valid()) {

            assert(!_current_line.hints.empty());
            for (auto& hint : _current_line.hints) assert(hint < 1000000000000000000UL);
            auto unalignedId = _current_line.id;
            if (!_current_line_aligned) {
                alignSelfProducedClauseIds(_current_line, /*assertSelfProduced=*/true);
                _current_line_aligned = true;
            }
            auto nextId = _current_line.id;
            assert(nextId <= id || log_return_false("[ERROR] Instance %i: Read clause ID %lu, expected <= %lu\n", 
                _instance_id, nextId, id));
            id = nextId;

            int epoch = getClauseEpoch(id);
            if (epoch != _current_epoch) {
                // check if it is from a future epoch (which would be an error)
                assert(epoch < _current_epoch || log_return_false(
                    "[ERROR] Instance %i: clause ID=%lu (originally %lu) from epoch %i found; expected epoch %i or smaller\n", 
                    _instance_id, id, unalignedId, epoch, _current_epoch));
                // stop reading because a former epoch has been reached
                LOG(V2_INFO, "Proof instance %i stopping reading epoch %i at clause with ID %lu (originally %lu) from epoch %i\n", 
                    _instance_id, _current_epoch, id, unalignedId, epoch);
                break; 
            }

            if (_frontier.top() == id || 
                    (_current_line.literals.empty() && _winning_instance)) {
                // Clause derivation is necessary for the combined proof
                _num_traced_clauses++;
                if (_current_line.literals.empty()) {
                    LOG(V2_INFO, "Instance %i: found \"winning\" empty clause\n", _instance_id);
                }

                // Output the line
                auto lineStr = _current_line.toStr();
                _output.write(lineStr.c_str(), lineStr.size());

                // Traverse clause hints
                for (auto hintId : _current_line.hints) {
                    assert(hintId < 1000000000000000000UL);
                    if (isSelfProducedClause(hintId)) {
                        _frontier.push(hintId);
                    } else if (!isOriginalClause(hintId)) {
                        int hintEpoch = getClauseEpoch(hintId);
                        if (hintEpoch >= epoch) {
                            LOG(V0_CRIT, "[ERROR] Found ext. hint %ld from epoch %i for clause %ld from epoch %i!\n", 
                                hintId, hintEpoch, id, epoch);
                            LOG(V0_CRIT, "[ERROR] Concerned line: %s\n", _current_line.toStr().c_str());
                            _output.flush();
                            abort();
                        }
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
            if (_parser.hasNext()) {
                _current_line = _parser.next();
                _current_line_aligned = false;
            }
            else _current_line.id = -1;
            numReadLines++;
        }

        LOG(V2_INFO, "Proof instance %i: %i lines this epoch; last read ID: %lu; %lu traced so far; %lu in backlog\n", 
            _instance_id, numReadLines, id, _num_traced_clauses, _backlog.size());

        if (_current_epoch == 0) {
            // End of the procedure reached!
            
            // -- the proof file must have been read completely
            assert(!_current_line.valid());
            assert(!_parser.hasNext());

            // -- there may not be any underived clauses left
            assert(_frontier.empty());
            assert(_backlog.empty());

            _finished = true;
            LOG(V2_INFO, "Proof instance %i finished!\n", _instance_id);
        } else {
            _current_epoch--;
        }
    }

    void prepareNextOutgoingClauseIds() {

        _outgoing_clause_ids.clear();

        long id = std::numeric_limits<long>::max();
        while (!_backlog.empty()) {
            long nextId = _backlog.top();
            assert(nextId <= id);
            id = nextId;
            
            int epoch = getClauseEpoch(id);
            if (epoch != _current_epoch) {
                // check if it is from a future epoch (which would be an error)
                assert(epoch < _current_epoch || 
                    log_return_false("[ERROR] Instance %i: clause ID=%lu from epoch %i found; expected epoch %i or smaller\n", 
                    _instance_id, id, epoch, _current_epoch));
                // stop reading because a former epoch has been reached
                break;
            }

            // Found an external clause ID from the prior epoch
            _outgoing_clause_ids.push_back(id);

            while (_backlog.top() == id) _backlog.pop();
        }
    }

    void alignSelfProducedClauseIds(LratLine& line, bool assertSelfProduced) {
        alignClauseId(line.id, assertSelfProduced);
        for (auto& hint : line.hints) alignClauseId(hint, /*assertSelfProduced=*/false);
    }
    void alignClauseId(LratClauseId& id, bool assertSelfProduced) {
        if (assertSelfProduced) assert(isSelfProducedClause(id));
        if (isSelfProducedClause(id)) {
            int epoch = getUnalignedClauseEpoch(id);
            id += _local_epoch_offsets[epoch];
            assert(isSelfProducedClause(id));
            assert(getClauseEpoch(id) == epoch);
        }
    }

    bool isOriginalClause(LratClauseId clauseId) {
        return clauseId <= _original_num_clauses;
    }
    bool isSelfProducedClause(LratClauseId clauseId) {
        if (isOriginalClause(clauseId)) return false;
        return _instance_id == (clauseId-_original_num_clauses-1) % _num_instances;
    }
    
    int getUnalignedClauseEpoch(LratClauseId clauseId) {
        return metadata::getEpoch(clauseId, _local_epoch_starts);
    }
    int getClauseEpoch(LratClauseId clauseId) {
        return metadata::getEpoch(clauseId, _global_epoch_starts);
    }
};
