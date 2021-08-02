
#include <ctype.h>
#include <stdio.h>
#include <iostream>
#include <assert.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "sat_reader.hpp"
#include "util/sys/terminator.hpp"

bool SatReader::read(JobDescription& desc) {

	FILE* pipe = nullptr;
	if ((_filename.size() > 3 && _filename.substr(_filename.size()-3, 3) == ".xz")
		|| (_filename.size() > 5 && _filename.substr(_filename.size()-5, 5) == ".lzma")) {
		// Decompress, read output
		auto command = "xz -c -d " + _filename;
		pipe = popen(command.c_str(), "r");
		if (pipe == nullptr) return false;
	}
	
	desc.beginInitialization(desc.getRevision());
	
	_sign = 1;
	_comment = false;
	_began_num = false;
	_assumption = false;
	_num = 0;
	_max_var = desc.getNumVars();

	if (pipe == nullptr) {
		// Read file with mmap
		int fd = open(_filename.c_str(), O_RDONLY);
		if (fd == -1) return false;
		char* f;

		off_t size;
    	struct stat s;
		int status = stat(_filename.c_str(), &s);
		if (status == -1) return false;
		size = s.st_size;
		desc.reserveSize(size / sizeof(int));

		f = (char *) mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
		for (long i = 0; i < size; i++) {
			process(f[i], desc);
		}
		munmap(f, size);
		close(fd);

	} else {
		// Read every character of the formula (in a buffered manner)
		char buffer[4096] = {'\0'};
		while (!Terminator::isTerminating() && fgets(buffer, sizeof(buffer), pipe) != nullptr) {
			size_t pos = 0;
			while (buffer[pos] != '\0') {
				int c = buffer[pos++];
				process(c, desc);
			}
		}
	}

	if (_began_num) { // write final zero (without newline)
		if (!_assumption) desc.addLiteral(0);
	}

	desc.setNumVars(_max_var);
	desc.endInitialization();

	if (pipe != nullptr) pclose(pipe);

	return true;
}
