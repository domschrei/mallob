
#pragma once

#include "../lrat_utils.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/background_worker.hpp"

class ProofWriter {

public:
    struct LratOutputLine {
        enum Type {ADD, DELETE} type;
        std::vector<uint8_t> data;
        LratOutputLine() {}
        LratOutputLine(Type type, std::vector<uint8_t>&& data) : type(type), data(std::move(data)) {}
    };

private:
    const std::string _filename;
    const bool _binary;
    std::ofstream _ofs;
    SPSCBlockingRingbuffer<LratOutputLine> _buffer;
    BackgroundWorker _worker;

    unsigned long _num_pushed_lines {0};
    unsigned long _num_written_lines {0};
    bool _done {false};

public:
    ProofWriter(const std::string& filename, bool binary) : _filename(filename), _binary(binary),
        _ofs([&](){
            if (_binary) {
                return std::ofstream(_filename, std::ios::binary);
            } else {
                return std::ofstream(_filename);
            }
        }()), _buffer(131072) {
        
        runWriter();
    } 

    void pushAdditionBlocking(SerializedLratLine& line) {
        LratOutputLine out(LratOutputLine::ADD, std::move(line.data()));
        _buffer.pushBlocking(out);
        _num_pushed_lines++;
    }

    void pushDeletionBlocking(LratClauseId id, const std::vector<LratClauseId>& hintsToDelete) {
        std::vector<uint8_t> data;
        data.resize((1+hintsToDelete.size()) * sizeof(LratClauseId));
        memcpy(data.data(), &id, sizeof(LratClauseId));
        for (size_t i = 0; i < hintsToDelete.size(); i++) {
            memcpy(data.data()+(i+1)*sizeof(LratClauseId), hintsToDelete.data()+i, sizeof(LratClauseId));
        }
        LratOutputLine out(LratOutputLine::DELETE, std::move(data));
        _buffer.pushBlocking(out);
        _num_pushed_lines++;
    }

    void markExhausted() {
        LOG(V2_INFO, "Proof writer received full proof (%lu lines)\n", _num_pushed_lines);
        _buffer.markExhausted();
    }

    bool isDone() const {
        return _done;
    }

    ~ProofWriter() {
        _worker.stop();
        LOG(V2_INFO, "Proof writer wrote %lu/%lu lines\n", _num_written_lines, _num_pushed_lines);
    }

private:
    void runWriter() {
        _worker.run([&]() {

            LratOutputLine line;
            SerializedLratLine sline;
            {
                lrat_utils::WriteBuffer out(_ofs);

                while (_worker.continueRunning() && _buffer.poll(line)) {
                    
                    if (line.type == LratOutputLine::ADD) {
                        // write ADD line

                        sline.data().swap(line.data);
                        if (_binary) {
                            lrat_utils::writeLine(out, sline, lrat_utils::REVERSED);
                        } else {
                            std::string output = sline.toStr();
                            _ofs.write(output.c_str(), output.size());
                        }

                    } else {
                        // write DELETE line

                        LratClauseId id = *((LratClauseId*) line.data.data());
                        LratClauseId* hints = (LratClauseId*) (line.data.data() + sizeof(LratClauseId));
                        int numHints = (line.data.size() / sizeof(LratClauseId)) - 1;

                        if (_binary) {
                            lrat_utils::writeDeletionLine(out, id, hints, numHints, lrat_utils::REVERSED);
                        } else {
                            std::string delLine = std::to_string(id) + " d";
                            for (int i = 0; i < numHints; i++) {
                                delLine += " " + std::to_string(hints[i]);
                            } 
                            delLine += " 0\n";
                            _ofs.write(delLine.c_str(), delLine.size());
                        }
                    }

                    _num_written_lines++;
                }
            }

            _ofs.flush();
            _done = true;
        });
    }
};
