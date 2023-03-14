
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

public:
    enum ContentMode {ASCII, RAW};

private:
    Parameters& _params;
    std::string _filename;
    ContentMode _content_mode;

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

    bool _valid_input = false;

public:
    SatReader(Parameters& params, const std::string& filename, ContentMode contentMode) : 
        _params(params), _filename(filename), _content_mode(contentMode) {}
    bool read(JobDescription& desc);

    inline void processInt(int x, JobDescription& desc) {
        
        //std::cout << x << std::endl;

        if (_valid_input) {
            // Already WAS valid input: additional numbers will make it invalid!
            _valid_input = false;
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
                _valid_input = true;
            }
            return;
        }
        
        if (_traversing_clauses) {
            desc.addLiteral(x);
            if (x == 0) _num_read_clauses++;
        }
        else desc.addAssumption(x);

        _max_var = std::max(_max_var, std::abs(x));
        _empty_clause = _traversing_assumptions || (x == 0);
    }

    inline void process(char c, JobDescription& desc) {

        if (_comment && c != '\n') return;

        switch (c) {
        case '\n':
        case '\r':
        case EOF:
            _comment = false;
            if (_began_num) {
                assert(_num == 0);
                if (!_assumption) {
                    desc.addLiteral(0);
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
                    desc.addLiteral(lit);
                    if (lit == 0) {
                        if (_last_added_lit_was_zero) _contains_empty_clause = true;
                        _num_read_clauses++;
                    }
                    _last_added_lit_was_zero = lit == 0;
                } else if (_num != 0) {
                    desc.addAssumption(_sign * _num);
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
        return _content_mode != RAW || _valid_input;
    }
};

#endif
