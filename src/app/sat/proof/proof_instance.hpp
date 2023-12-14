
#pragma once

#include <algorithm>
#include <fstream>

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/proof/lrat_line.hpp"
#include "app/sat/proof/reverse_binary_lrat_parser.hpp"
#include "app/sat/proof/serialized_lrat_line.hpp"
#include "external_id_priority_queue.hpp"
#include "util/logger.hpp"
#include "util/sys/thread_pool.hpp"
#include "app/sat/proof/lrat_utils.hpp"
#include "merging/proof_merge_connector.hpp"

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
    Logger _log;

    const int _instance_id;
    const int _num_instances;
    int _original_num_clauses;
    bool _winning_instance;

    ReverseBinaryLratParser _parser;
    SerializedLratLine _current_line;
    bool _current_line_aligned = false;
    ExternalIdPriorityQueue _frontier;
    ExternalIdPriorityQueue _backlog;

    std::vector<LratClauseId> _local_epoch_starts;
    std::vector<LratClauseId> _local_epoch_offsets;
    std::vector<LratClauseId> _global_epoch_starts;
    int _current_epoch;

    std::string _output_filename;
    std::ofstream _output;
    lrat_utils::WriteBuffer _output_buf;

    std::future<void> _work_future;
    bool _work_done = true;
    std::vector<LratClauseId> _outgoing_clause_ids;
    bool _finished = false;

    unsigned long _num_parsed_clauses = 0;
    unsigned long _num_traced_clauses = 0;
    unsigned long _num_output_lines = 0;

    bool _interleave_merging {false};
    ProofMergeConnector* _merge_connector;

    bool _debugging {false};

public:
    ProofInstance(int instanceId, int numInstances, int originalNumClauses,
        const std::string& proofFilename, int finalEpoch, 
        int winningInstance, const std::vector<LratClauseId>& globalEpochStarts, 
        std::vector<LratClauseId>&& localEpochStarts, 
        std::vector<LratClauseId>&& localEpochOffsets, const std::string& extMemDiskDir, 
        const std::string& outputFilenameOrEmpty, bool debugging) :
            _log(Logger::getMainInstance().copy("Proof", ".proof")),
            _instance_id(instanceId), _num_instances(numInstances), 
            _original_num_clauses(originalNumClauses),
            _winning_instance(winningInstance == instanceId),
            _parser(proofFilename), 
            _frontier(extMemDiskDir + "/disk." + std::to_string(instanceId) + ".frontier", finalEpoch),
            _backlog(extMemDiskDir + "/disk." + std::to_string(instanceId) + ".backlog", finalEpoch),
            _local_epoch_starts(localEpochStarts), 
            _local_epoch_offsets(localEpochOffsets), _global_epoch_starts(globalEpochStarts),
            _current_epoch(finalEpoch), 
            _output_filename(outputFilenameOrEmpty),
            _output([&](){
                if (outputFilenameOrEmpty.empty()) return std::ofstream(); 
                else return std::ofstream(outputFilenameOrEmpty, std::ofstream::binary);
            }()),
            _output_buf(_output),
            _interleave_merging(outputFilenameOrEmpty.empty()),
            _debugging(debugging) {}

    ~ProofInstance() {
        if (_work_future.valid()) _work_future.get();
    }

    void setProofMergeConnector(ProofMergeConnector* conn) {
        _merge_connector = conn;
    }

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
        LOGGER(_log, V5_DEBG, "%i ~> %i IDs\n", _instance_id, _outgoing_clause_ids.size());
        return std::move(_outgoing_clause_ids);
    }

    bool finished() const {return _work_done && _finished;}

    std::string getOutputFilename() const {return _output_filename;}

    std::vector<unsigned long> getStats() const {
        std::vector<unsigned long> stats;
        stats.push_back(_parser.getNumReadBytes());
        stats.push_back(_num_parsed_clauses);
        stats.push_back(_num_traced_clauses);
        return stats;
    }

private:

    void handleIncomingClauseIds(const LratClauseId* clauseIdsData, size_t clauseIdsSize) {
        
        // Import self-produced clauses
        int numSelfClauses = 0;
        for (size_t i = 0; i < clauseIdsSize; i++) {
            LratClauseId id = clauseIdsData[i];
            if (isSelfProducedClause(id)) {
                _frontier.push(id, getClauseEpoch(id));
                numSelfClauses++;
            }
        }
        LOGGER(_log, V5_DEBG, "%i <~ %i self IDs\n", _instance_id, numSelfClauses);
    }

    void readEpoch() {

        LOGGER(_log, V5_DEBG, "%i reading e.%i\n", _instance_id, _current_epoch);
        int numReadLines = 0;

        std::ofstream dbgOfs;
        if (_debugging) {
            dbgOfs = std::ofstream(
                _log.getLogFilename()
                + "." + std::to_string(_instance_id)
                + "." + std::to_string(_current_epoch)
            );
        }

        if (!_current_line.valid() && _parser.getNextLine(_current_line)) {
            _current_line_aligned = false;
            numReadLines++;
        }
        
        LratClauseId id = std::numeric_limits<unsigned long>::max();
        auto formerId = id;
        int numSkippedLines = 0;
        while (_current_line.valid()) {

            auto& alignedId = _current_line.getId();
            auto unalignedId = alignedId;
            auto [hints, numHints] = _current_line.getUnsignedHints();
            auto [lits, numLits] = _current_line.getLiterals();

            if (!_current_line_aligned) {
                if (_debugging) outputLratLine("INU", _current_line, dbgOfs);
                alignSelfProducedClauseIds(alignedId, hints, numHints, /*assertSelfProduced=*/true);
                _current_line_aligned = true;
            }
            auto nextId = alignedId;
            assert(nextId <= id || log_return_false("[ERROR] Instance %i: Read clause ID %lu, expected <= %lu\n", 
                _instance_id, nextId, id));
            formerId = id;
            id = nextId;
            if (_debugging) outputLratLine("IN ", _current_line, dbgOfs);

            int epoch = getClauseEpoch(id);
            // skip the line if it is a stray one from a future epoch
            if (epoch > _current_epoch) {
                // TODO fail if the current epoch isn't the final one
                numSkippedLines++;
                if (_parser.getNextLine(_current_line)) {
                    _current_line_aligned = false;
                    numReadLines++;
                }
                continue;
            }
            // stop reading if a former epoch has been reached
            if (epoch < _current_epoch) {
                LOGGER(_log, V5_DEBG, "%i stopping e.%i @ ID %lu (orig. %lu) from e.%i\n",
                    _instance_id, _current_epoch, id, unalignedId, epoch);
                break; 
            }

            if ((!_frontier.empty() && _frontier.top() == id) || 
                    (numLits == 0 && _winning_instance)) {
                // Clause derivation is necessary for the combined proof
                _num_traced_clauses++;
                if (numLits == 0) {
                    LOGGER(_log, V3_VERB, "%i found \"winning\" empty clause\n", _instance_id);
                }

                // Traverse clause hints
                for (size_t i = 0; i < numHints; i++) {
                    auto hintId = hints[i];
                    int hintEpoch = getClauseEpoch(hintId);
                    if (isSelfProducedClause(hintId)) {
                        if (_debugging) outputLratId("FRT", hintId, dbgOfs);
                        _frontier.push(hintId, hintEpoch);
                    } else if (!isOriginalClause(hintId)) {
                        if (hintEpoch >= epoch) {
                            LOGGER(_log, V0_CRIT, "[ERROR] Proof %i found ext. hint %ld from epoch %i for clause %ld from epoch %i!\n", 
                                _instance_id, hintId, hintEpoch, id, epoch);
                            LOGGER(_log, V0_CRIT, "[ERROR] Concerned line: %s\n", _current_line.toStr().c_str());
                            _output.flush();
                            abort();
                        }
                        if (_debugging) outputLratId("BLG", hintId, dbgOfs);
                        _backlog.push(hintId, hintEpoch);
                    }
                }

                // Output the line
                if (_debugging) outputLratLine("OUT", _current_line, dbgOfs);
                if (_interleave_merging) {
                    _merge_connector->pushBlocking(_current_line);
                } else {
                    lrat_utils::writeLine(_output_buf, _current_line);
                }
                _num_output_lines++;

                // Remove all instances of this ID from the frontier
                while (!_frontier.empty() && _frontier.top() == id) 
                    _frontier.pop();
            } else {
                // Next necessary clause ID must be smaller -
                // Ignore this line
                assert(_frontier.empty() || _frontier.top() < id || log_return_false(
                    "[ERROR] Proof %i: clause ID=%lu found in frontier; expected ID smaller than %lu\n", 
                    _instance_id, _frontier.top(), id));
            }

            // Get next proof line
            if (_parser.getNextLine(_current_line)) {
                _current_line_aligned = false;
                numReadLines++;
            }
        }

        // print a warning if some lines needed to be skipped
        if (numSkippedLines > 0) {
            LOGGER(_log, V1_WARN, "[WARN] Proof %i: skipped %i lines from future epoch > %i\n", 
                _instance_id, numSkippedLines, _current_epoch);
            numSkippedLines = 0;
        }

        LOGGER(_log, V4_VVER, "%i e.%i read:%i last:%s traced:%lu blg:%lu frt:%lu\n",
            _instance_id, _current_epoch, numReadLines, 
            numReadLines==0 ? "-" : std::to_string(formerId).c_str(), 
            _num_traced_clauses, _backlog.size(), _frontier.size());

        _num_parsed_clauses += numReadLines;

        if (_current_epoch == 0) {
            // End of the procedure reached!
            
            // -- the proof file must have been read completely
            assert(!_current_line.valid());
            assert(!_parser.getNextLine(_current_line));

            // -- there may not be any underived clauses left
            assert(_frontier.empty());
            assert(_backlog.empty());

            if (_interleave_merging) {

                // Insert final "stop" line which contains the statistics
                LratLine statsLine;
                statsLine.id = 0;
                auto stats = getStats();
                for (auto stat : stats) {
                    statsLine.hints.push_back(stat);
                    statsLine.signsOfHints.push_back(true);
                }
                SerializedLratLine serializedStatsLine(statsLine);
                _merge_connector->pushBlocking(serializedStatsLine);

                _merge_connector->markExhausted();
            }

            _finished = true;
            LOGGER(_log, V4_VVER, "%i finished pruning\n", _instance_id);

        } else {
            // Insert sentinel element / "stub" line to signal end of epoch
            if (_interleave_merging) {
                int nextIdEpoch = _current_epoch-1;
                if (nextIdEpoch+1 >= _global_epoch_starts.size()) {
                    LOGGER(_log, V1_WARN, "[WARN] Skipping sentinel for \"dead\" epoch %i\n", nextIdEpoch+1);
                } else {
                    auto sentinelId = _global_epoch_starts[nextIdEpoch+1]-1;
                    SerializedLratLine sentinelLine(sentinelId);
                    assert(sentinelLine.isStub());
                    _merge_connector->pushBlocking(sentinelLine);
                    _num_output_lines++;
                    LOGGER(_log, V5_DEBG, "%i e.%i sentinel:%lu outlines:%lu\n", 
                        _instance_id, _current_epoch, sentinelId, _num_output_lines);
                }
            }
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
                    log_return_false("[ERROR] Proof %i: clause ID=%lu from epoch %i found; expected epoch %i or smaller\n", 
                    _instance_id, id, epoch, _current_epoch));
                // stop reading because a former epoch has been reached
                break;
            }

            // Found an external clause ID from the prior epoch
            _outgoing_clause_ids.push_back(id);

            while (!_backlog.empty() && _backlog.top() == id) 
                _backlog.pop();
        }
    }

    void alignSelfProducedClauseIds(LratClauseId& id, LratClauseId* hints, int numHints, bool assertSelfProduced) {
        alignClauseId(id, assertSelfProduced);
        for (size_t i = 0; i < numHints; i++) {
            alignClauseId(*(hints+i), /*assertSelfProduced=*/false);
        }
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
        return _instance_id == (clauseId-_original_num_clauses) % _num_instances;
    }
    
    int getUnalignedClauseEpoch(LratClauseId clauseId) {
        return ClauseMetadata::getEpoch(clauseId, _local_epoch_starts);
    }
    int getClauseEpoch(LratClauseId clauseId) {
        return ClauseMetadata::getEpoch(clauseId, _global_epoch_starts);
    }

    void outputLratLine(const std::string& op, const SerializedLratLine& line, std::ofstream& ofs) {
        ofs << op << " " << line.toStr();
        ofs.flush();
    }
    void outputLratId(const std::string& op, LratClauseId id, std::ofstream& ofs) {
        ofs << op << " " << id << std::endl;
        ofs.flush();
    }
};
