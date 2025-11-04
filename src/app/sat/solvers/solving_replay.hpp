
#pragma once

#include "app/sat/data/clause.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "util/logger.hpp"
#include <fstream>
#include <ostream>
#include <string>

class SolvingReplay {

public:
    enum Mode {NONE, RECORD, REPLAY} _mode;

private:
    std::string _path;
    std::ofstream _out;
    std::ifstream _in;

    unsigned long _counter_terminate {0};
    unsigned long _counter_import_nonunit {0};
    unsigned long _counter_import_units {0};
    unsigned long _counter_return_from_solve {0};

    unsigned long _in_line {0};

    std::vector<int> _clause_data;

public:
    SolvingReplay(Mode mode, std::string path) : _mode(mode), _path(path) {
        if (_mode == RECORD) {
            _out = std::ofstream(_path);
            LOG(V2_INFO, "Opened replay output %s\n", _path.c_str());
        }
        if (_mode == REPLAY) {
            _in = std::ifstream(_path);
            LOG(V2_INFO, "Opened replay input %s\n", _path.c_str());
        }
    }
    Mode getMode() const {return _mode;}

    void recordTerminateCallback(bool terminate) {
        assert(_mode == RECORD);
        _counter_terminate++;
        _out << _counter_terminate << " t " << (terminate?1:0) << std::endl;
    }
    void recordImportCallback(bool success, Mallob::Clause& c, GenericClauseStore::ExportMode mode) {
        assert(_mode == RECORD);
        _counter_import_nonunit++;
        _out << _counter_import_nonunit << " I " << mode << " " << (success?1:0);
        if (success) {
            _out << " " << c.lbd << " " << c.size;
            for (int i = 0; i < c.size; i++) _out << " " << c.begin[i];
        }
        _out << std::endl;
    }
    void recordImportCallback(std::vector<int>& units) {
        assert(_mode == RECORD);
        _counter_import_units++;
        _out << _counter_import_units << " i " << units.size();
        for (int lit : units) _out << " " + std::to_string(lit);
        _out << std::endl;
    }
    void recordReturnFromSolve(int res) {
        assert(_mode == RECORD);
        _counter_return_from_solve++;
        _out << _counter_return_from_solve << " r " << res << std::endl;
    }

    bool replayTerminateCallback() {
        assert(_mode == REPLAY);
        _counter_terminate++;
        _in_line++;

        unsigned long counter; std::string type; int terminate;
        _in >> counter >> type >> terminate;
        checkConsistency(_counter_terminate, "t", counter, type.c_str());
        assert(terminate == 0 || terminate == 1);
        return terminate==1;
    }
    bool replayImportCallback(Mallob::Clause& c, GenericClauseStore::ExportMode mode) {
        assert(_mode == REPLAY);
        _counter_import_nonunit++;
        _in_line++;

        unsigned long counter; std::string type; int modeInt; int success;
        _in >> counter >> type >> modeInt >> success;
        checkConsistency(_counter_import_nonunit, "I", counter, type.c_str());
        assert(GenericClauseStore::ExportMode(modeInt) == mode);
        assert(success == 0 || success == 1);
        if (!success) return false;

        _clause_data.clear();
        int lbd; int size;
        _in >> lbd >> size;
        for (int i = 0; i < size; i++) {
            int lit;
            _in >> lit;
            _clause_data.push_back(lit);
        }
        c = Mallob::Clause(_clause_data.data(), size, lbd);
        return true;
    }
    std::vector<int> replayImportCallback() {
        assert(_mode == REPLAY);
        _counter_import_units++;
        _in_line++;

        unsigned long counter; std::string type; int size;
        _in >> counter >> type >> size;
        checkConsistency(_counter_import_units, "i", counter, type.c_str());
        assert(size >= 0);

        _clause_data.clear();
        for (int i = 0; i < size; i++) {
            int lit;
            _in >> lit;
            _clause_data.push_back(lit);
        }
        return _clause_data;
    }
    int replayReturnFromSolve(int res) {
        assert(_mode == REPLAY);
        _counter_return_from_solve++;
        _in_line++;

        unsigned long counter; std::string type; int readRes;
        _in >> counter >> type >> readRes;
        checkConsistency(_counter_return_from_solve, "r", counter, type.c_str());
        assert(res == readRes);
        return res;
    }

private:
    void checkConsistency(unsigned long expectedCount, const std::string& expectedType, unsigned long readCount, const std::string& readType) {
        if (expectedCount != readCount || expectedType != readType) {
            LOG(V0_CRIT, "[ERROR] %s line %lu : expected #%lu type %s, read #%lu type %s\n",
                _path.c_str(), _in_line, expectedCount, expectedType.c_str(), readCount, readType.c_str());
            abort();
        }
    }
};
