
#ifndef MALLOB_SAT_READER_H
#define MALLOB_SAT_READER_H

#include <string>
#include <vector>
#include <memory>
#include <assert.h>

#include "data/job_description.hpp"

class SatReader {

private:
    std::string _filename;

    int _sign = 1;
	bool _comment = false;
	bool _began_num = false;
    bool _assumption = false;
	int _num = 0;
	int _max_var = 0;

public:
    SatReader(std::string filename) : _filename(filename) {}
    bool read(JobDescription& desc);
    inline void process(char c, JobDescription& desc) {

        if (_comment && c != '\n') return;

        switch (c) {
        case '\n':
            _comment = false;
            if (_began_num) {
                assert(_num == 0);
                if (!_assumption) desc.addLiteral(0);
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
                    desc.addLiteral(_sign * _num);
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
};

#endif