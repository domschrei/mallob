
/*
 * SatUtils.cpp
 *
 *  Created on: Mar 9, 2015
 *      Author: balyo
 */

#include <ctype.h>
#include <stdio.h>

#include "sat_reader.hpp"

std::shared_ptr<std::vector<int>> SatReader::read() {
	FILE* f;
	bool piped;
	if ((_filename.size() > 3 && _filename.substr(_filename.size()-3, 3) == ".xz")
		|| (_filename.size() > 5 && _filename.substr(_filename.size()-5, 5) == ".lzma")) {
		// Decompress, read output
		auto command = "xz -c -d " + _filename;
		f = popen(command.c_str(), "r");
		piped = true;
	} else {
		// Directly read file
    	f = fopen(_filename.c_str(), "r");
		piped = false;
	}
	if (f == NULL) {
		return NULL;
	}
	int c = 0;
	bool neg = false;
	std::shared_ptr<std::vector<int>> cls = std::make_shared<std::vector<int>>();
	while (c != EOF) {
		c = fgetc(f);

		// comment or problem definition line
		if (c == 'c' || c == 'p') {
			// skip this line
			while(c != '\n') {
				c = fgetc(f);
			}
			continue;
		}
		// whitespace
		if (isspace(c)) {
			continue;
		}
		// negative
		if (c == '-') {
			neg = true;
			continue;
		}

		// number
		if (isdigit(c)) {
			int num = c - '0';
			c = fgetc(f);
			while (isdigit(c)) {
				num = num*10 + (c-'0');
				c = fgetc(f);
			}
			_num_vars = std::max(_num_vars, num);
			if (neg) {
				num *= -1;
			}
			neg = false;

			cls->push_back(num);
		}
	}

	if (piped) pclose(f);
	else fclose(f);

	return cls;
}





