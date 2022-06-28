
#include <ctype.h>
#include <stdio.h>
#include <iostream>
#include "util/assert.hpp"
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "sat_reader.hpp"
#include "util/sys/terminator.hpp"

bool SatReader::read(JobDescription& desc) {

	_raw_content_mode = desc.getAppConfiguration().map.count("content-mode")
		&& desc.getAppConfiguration().map.at("content-mode") == "RAW";

	FILE* pipe = nullptr;
	int namedpipe = -1;
	if ((_filename.size() > 3 && _filename.substr(_filename.size()-3, 3) == ".xz")
		|| (_filename.size() > 5 && _filename.substr(_filename.size()-5, 5) == ".lzma")) {
		// Decompress, read output
		auto command = "xz -c -d " + _filename;
		pipe = popen(command.c_str(), "r");
		if (pipe == nullptr) return false;
	} else if (_filename.size() > 5 && _filename.substr(_filename.size()-5, 5) == ".pipe") {
		// Named pipe!
		namedpipe = open(_filename.c_str(), O_RDONLY);
	}
		
	if (pipe == nullptr && namedpipe == -1) {
		// Read file with mmap
		int fd = open(_filename.c_str(), O_RDONLY);
		if (fd == -1) return false;

		off_t size;
    	struct stat s;
		int status = stat(_filename.c_str(), &s);
		if (status == -1) return false;
		size = s.st_size;
		desc.reserveSize(size / sizeof(int));
		void* mmapped = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);

		if (_raw_content_mode) {
			int* f = (int*) mmapped;
			for (long i = 0; i < size; i++) {
				processInt(f[i], desc);
			}
		} else {
			char* f = (char*) mmapped;
			for (long i = 0; i < size; i++) {
				process(f[i], desc);
			}
			process(EOF, desc);
		}
		munmap(mmapped, size);
		close(fd);

	} else if (namedpipe != -1) {
		// Read formula over named pipe
		int iteration = 0;
		if (_raw_content_mode) {
			int buffer[1024] = {0};
			while (iteration ^ 511 != 0 || !Terminator::isTerminating()) {
				int numRead = ::read(namedpipe, buffer, sizeof(buffer));
				if (numRead <= 0) break;
				numRead /= sizeof(int);
				for (int i = 0; i < numRead; i++) {
					processInt(buffer[i], desc);
				}
				iteration++;
			}
		} else {
			char buffer[4096] = {'\0'};
			while (iteration ^ 511 != 0 || !Terminator::isTerminating()) {
				int numRead = ::read(namedpipe, buffer, sizeof(buffer));
				if (numRead <= 0) break;
				for (int i = 0; i < numRead; i++) {
					int c = buffer[i];
					process(c, desc);
				}
				iteration++;
			}
			process(EOF, desc);
		}

	} else {
		// Read file over pipe
		if (_raw_content_mode) {
			int iteration = 0;
			int buffer[1024] = {0};
			while (iteration ^ 511 != 0 || !Terminator::isTerminating()) {
				int numRead = ::read(fileno(pipe), buffer, sizeof(buffer));
				if (numRead <= 0) break;
				for (int i = 0; i < numRead; i++) {
					int c = buffer[i];
					processInt(c, desc);
				}
				iteration++;
			}
		} else {
			char buffer[4096] = {'\0'};
			while (!Terminator::isTerminating() && fgets(buffer, sizeof(buffer), pipe) != nullptr) {
				size_t pos = 0;
				while (buffer[pos] != '\0') {
					int c = buffer[pos++];
					process(c, desc);
				}
			}
			process(EOF, desc);
		}
	}

	if (pipe != nullptr) pclose(pipe);
	if (namedpipe != -1) close(namedpipe);

	return isValidInput();
}
