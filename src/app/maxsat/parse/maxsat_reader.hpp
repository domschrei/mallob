
#ifndef MALLOB_MAXSAT_READER_H
#define MALLOB_MAXSAT_READER_H

#include <stdio.h>
#include <bits/std_abs.h>
#include <stdlib.h>
#include <string>
#include <algorithm>

#include "data/job_description.hpp"

class Parameters;

class MaxSatReader {

private:
    const Parameters& _params;
    std::string _filename;
    FILE* _pipe {nullptr};
	int _namedpipe {-1};

    // Content mode: ASCII
    int _sign = 1;
	bool _comment = false;
	bool _began_num = false;
    bool _assumption = false;
	size_t _num = 0;
	int _max_var = 0;
    int _num_read_clauses = 0;
    bool _last_added_lit_was_zero {true};
    bool _contains_empty_clause {false};
    bool _hard_clause = false;

    std::vector<std::pair<uint64_t, int>> _objective;
    std::pair<uint64_t, int> _current_soft_unit {0, 0};

    bool _input_invalid {false};
    bool _input_finished {false};

public:
    MaxSatReader(const Parameters& params, const std::string& filename) : 
        _params(params), _filename(filename) {}
    bool read(JobDescription& desc);
    bool parseInternally(JobDescription& desc);
    bool parseWithTrustedParser(JobDescription& desc);

    inline void process(char c, JobDescription& desc) {

        if (_comment && c != '\n') return;

        signed char uc = *((signed char*) &c);
        switch (uc) {
        case EOF:
            _input_finished = true;
        case '\n':
        case '\r':
            _comment = false;
            if (_began_num) {
                if (_num != 0) {
                    _input_invalid = true;
                    return;
                }
                if (!_assumption) {
                    if (_hard_clause) {
                        desc.addPermanentData(0);
                        _num_read_clauses++;
                    } else {
                        // soft clause
                        _objective.push_back(_current_soft_unit);
                        _current_soft_unit = {0, 0};
                    }
                    if (_last_added_lit_was_zero) _contains_empty_clause = true;
                    _last_added_lit_was_zero = true;
                }
                _began_num = false;
            }
            _hard_clause = false;
            _assumption = false;
            break;
        case 'p':
        case 'c':
            _comment = true;
            break;
        case 'a':
            _assumption = true;
            break;
        case 'h':
            _hard_clause = true;
            break;
        case ' ':
            if (_began_num) {
                if (!_assumption) {
                    int lit = _sign * (int)_num;
                    if (_hard_clause) {
                        _max_var = std::max(_max_var, std::abs(lit));
                        desc.addPermanentData(lit);
                    } else if (_num != 0) {
                        // soft unit clause
                        if (_current_soft_unit.first == 0) {
                            assert(_sign == 1);
                            _current_soft_unit.first = _num;
                        } else if (_current_soft_unit.second == 0) {
                            _max_var = std::max(_max_var, std::abs(lit));
                            _current_soft_unit.second = lit;
                        }
                    }
                    if (_num == 0) {
                        if (_last_added_lit_was_zero) _contains_empty_clause = true;
                        _num_read_clauses++;
                    }
                    _last_added_lit_was_zero = lit == 0;
                } else if (_num != 0) {
                    desc.addTransientData(_sign * (int)_num);
                }
                _num = 0;
                _began_num = false;
            }
            _sign = 1;
            break;
        case '-':
            _sign = -1;
            _began_num = true;
            break;
        default:
            // Add digit to current number
            _num = _num*10 + (c-'0');
            _began_num = true;
            break;
        }
    }

    void finalize(JobDescription& desc) {
        desc.addPermanentData(0);
        for (auto& softUnit : _objective) {
            // Need to write each weight, which could be 64-bit, as two 32-bit integers ...
            const int* weightAsTwoInts = (int*) &softUnit.first;
            desc.addPermanentData(weightAsTwoInts[0]);
            desc.addPermanentData(weightAsTwoInts[1]);
            desc.addPermanentData(softUnit.second);
        }
        desc.addPermanentData((int) _objective.size());
    }

    bool isValidInput() const {
        return _input_finished && !_input_invalid;
    }

    int getNbVars() const {
        return _max_var;
    }
    int getNbClauses() const {
        return _num_read_clauses;
    }
};

#endif
