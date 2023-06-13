
#ifndef MALLOB_SAT_READER_H
#define MALLOB_SAT_READER_H

#include <string>
#include <vector>
#include <memory>
#include <stdio.h> 
#include "util/assert.hpp"

#include "data/job_description.hpp"
#include "util/params.hpp"

#include <iostream>

class SatReader {

private:
    const Parameters& _params;
    std::string _filename;
    bool _raw_content_mode;

    // Content mode: ASCII
    int _sign = 1;
	bool _comment = false;
	bool _began_num = false;
    bool _assumption = false;
	int _num = 0;
	int _max_var = 0;
    int _num_read_clauses = 0;
    bool _last_added_lit_was_zero {true};
    bool _contains_empty_clause {false};

    // Content mode: RAW
    bool _traversing_clauses = true;
    bool _traversing_assumptions = false;
    bool _empty_clause = true;

    bool _input_invalid {false};
    bool _input_finished {false};

public:
    SatReader(const Parameters& params, const std::string& filename) : 
        _params(params), _filename(filename) {}
    bool read(JobDescription& desc);

    inline void processInt(int x, JobDescription& desc) {
        
        //std::cout << x << std::endl;

        if (_input_finished) {
            // Already WAS valid input: additional numbers will make it invalid!
            _input_invalid = true;
            return;
        }

        if (_empty_clause && x == 0) {
            // Received an "empty clause" (zero without a preceding non-zero literal)
            if (_traversing_clauses) {
                // switch to assumptions
                _traversing_clauses = false;
                _traversing_assumptions = true;
            } else if (_traversing_assumptions) {
                // End of assumptions: done.
                _input_finished = true;
            }
            return;
        }
        
        if (_traversing_clauses) {
            desc.addPermanentData(x);
            if (x == 0) _num_read_clauses++;
        }
        else desc.addTransientData(x);

        _max_var = std::max(_max_var, std::abs(x));
        _empty_clause = _traversing_assumptions || (x == 0);
    }

    inline void process(char c, JobDescription& desc) {

        // When processing QBF, the prefix is encoded as sequence of
        // integers, each either > 0 (existential) or < 0 (universal).
        // The quantifier prefix is separated from the CNF formula by a
        // 0, like a clause in SAT.

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
                    desc.addPermanentData(0);
                    if (_last_added_lit_was_zero) _contains_empty_clause = true;
                    _last_added_lit_was_zero = true;
                    _num_read_clauses++;
                }
                _began_num = false;
            }
            _assumption = false;
            break;
        case 'p':
        case 'c':
            _comment = true;
            break;
        case 'a':
            _assumption = true;
            break;
        case ' ':
            if (_began_num) {
                _max_var = std::max(_max_var, _num);
                if (!_assumption) {
                    int lit = _sign * _num;
                    desc.addPermanentData(lit);
                    if (lit == 0) {
                        if (_last_added_lit_was_zero) _contains_empty_clause = true;
                        _num_read_clauses++;
                    }
                    _last_added_lit_was_zero = lit == 0;
                } else if (_num != 0) {
                    desc.addTransientData(_sign * _num);
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

    bool isValidInput() const {
        return _input_finished && !_input_invalid;
    }
};

#endif
