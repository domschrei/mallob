
#ifndef MALLOB_SAT_READER_H
#define MALLOB_SAT_READER_H

#include <cstdint>
#include <memory>
#include <stdio.h>
#include <bits/std_abs.h>
#include <stdlib.h>
#include <string>
#include <algorithm>

#include "app/sat/proof/trusted_parser_process_adapter.hpp"
#include "data/job_description.hpp"

class Parameters;

class SatReader {

private:
    const Parameters& _params;
    std::vector<std::string> _files;
    std::string _filename;
    bool _raw_content_mode;
    FILE* _pipe {nullptr};
	int _namedpipe {-1};
    bool _force_incremental_parser;

    std::shared_ptr<TrustedParserProcessAdapter> _tppa;

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
    SatReader(const Parameters& params, const std::string& file, bool forceIncrementalParser = false) : 
        _params(params), _files({file}), _force_incremental_parser(forceIncrementalParser) {}
    SatReader(const Parameters& params, const std::vector<std::string>& files, bool forceIncrementalParser = false) : 
        _params(params), _files(files), _force_incremental_parser(forceIncrementalParser) {}
    void setTrustedParser(std::shared_ptr<TrustedParserProcessAdapter> tppa) {_tppa = tppa;}
    std::shared_ptr<TrustedParserProcessAdapter> getTrustedParser() {return _tppa;}
    bool read(JobDescription& desc);
    bool parseInternally(JobDescription& desc);
    bool parseWithTrustedParser(JobDescription& desc);
    bool parseAndCompress(JobDescription& desc);

    inline void processInt(int x, JobDescription& desc) {
        
        //std::cout << x << std::endl;

        if (_input_finished) {
            // Already WAS valid input: additional numbers will make it invalid!
            _input_invalid = true;
            return;
        }

        if (x == INT32_MAX && _traversing_clauses) {
            // switch to assumptions
            _traversing_clauses = false;
            _traversing_assumptions = true;
            desc.addData(x);
            return;
        }

        if (_traversing_assumptions && x == 0) {
            // End of assumptions: done.
            _input_finished = true;
            desc.addData(x);
            return;
        }

        desc.addData(x);
        if (_traversing_clauses && x == 0) _num_read_clauses++;

        _max_var = std::max(_max_var, std::abs(x));
        _empty_clause = _traversing_assumptions || (x == 0);
    }

    inline void process(char c, JobDescription& desc) {

        if (_comment && c != '\n') return;

        signed char uc = *((signed char*) &c);
        switch (uc) {
        case EOF:
            if (_assumption) desc.addData(0);
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
                    desc.addData(0);
                    if (_last_added_lit_was_zero) _contains_empty_clause = true;
                    _last_added_lit_was_zero = true;
                    _num_read_clauses++;
                }
                _began_num = false;
            }
            break;
        case 'p':
        case 'c':
            _comment = true;
            break;
        case 'a':
            if (!_assumption) desc.addData(INT32_MAX); // separator to begin assumptions
            _assumption = true;
            break;
        case ' ':
            if (_began_num) {
                _max_var = std::max(_max_var, _num);
                if (!_assumption) {
                    int lit = _sign * _num;
                    desc.addData(lit);
                    if (lit == 0) {
                        if (_last_added_lit_was_zero) _contains_empty_clause = true;
                        _num_read_clauses++;
                    }
                    _last_added_lit_was_zero = lit == 0;
                } else if (_num != 0) {
                    desc.addData(_sign * _num);
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

    int getNbVars() const {
        return _max_var;
    }
    int getNbClauses() const {
        return _num_read_clauses;
    }
};

#endif
