
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <fstream>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "app/sat/proof/trusted_parser_process_adapter.hpp"
#include "maxsat_reader.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/tmpdir.hpp"
#include "data/app_configuration.hpp"
#include "util/option.hpp"

bool MaxSatReader::parseInternally(JobDescription& desc) {

	bool rawContentMode = desc.getAppConfiguration().map.count("content-mode")
		&& desc.getAppConfiguration().map.at("content-mode") == "raw";
	assert(!rawContentMode);

	_pipe = nullptr;
	_namedpipe = -1;
	if ((_filename.size() > 3 && _filename.substr(_filename.size()-3, 3) == ".xz")
		|| (_filename.size() > 5 && _filename.substr(_filename.size()-5, 5) == ".lzma")) {
		// Decompress, read output
		auto command = "xz -c -d " + _filename;
		_pipe = popen(command.c_str(), "r");
		if (_pipe == nullptr) return false;
	} else if (_filename.size() > 5 && _filename.substr(_filename.size()-5, 5) == ".pipe") {
		// Named pipe!
		_namedpipe = open(_filename.c_str(), O_RDONLY);
	}

	if (_pipe == nullptr && _namedpipe == -1) {

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

		char* f = (char*) mmapped;
		for (long i = 0; i < size; i++) {
			process(f[i], desc);
		}
		process(EOF, desc);

		munmap(mmapped, size);
		close(fd);

	} else if (_namedpipe != -1) {
		// Read formula over named pipe

		int iteration = 0;
		const int bufsize = 4096;
		char buffer[bufsize] = {'\0'};
		while ((iteration ^ 511) != 0 || !Terminator::isTerminating()) {
			int numRead = ::read(_namedpipe, buffer, bufsize);
			if (numRead <= 0) break;
			for (int i = 0; i < numRead; i++) {
				int c = buffer[i];
				process(c, desc);
			}
			iteration++;
		}
		process(EOF, desc);

	} else {
		// Read file over pipe
		char buffer[4096] = {'\0'};
		while (!Terminator::isTerminating() && fgets(buffer, sizeof(buffer), _pipe) != nullptr) {
			size_t pos = 0;
			while (buffer[pos] != '\0') {
				int c = buffer[pos++];
				process(c, desc);
			}
		}
		process(EOF, desc);
	}

	finalize(desc);
	return true;
}

bool MaxSatReader::read(JobDescription& desc) {

	const std::string NC_DEFAULT_VAL = "BMMMKKK111";
	desc.setAppConfigurationEntry("__NC", NC_DEFAULT_VAL);
	desc.setAppConfigurationEntry("__NV", NC_DEFAULT_VAL);
	desc.setAppConfigurationEntry("__NO", NC_DEFAULT_VAL);
	if (_params.onTheFlyChecking()) {
		std::string placeholder(32, 'x');
		desc.setAppConfigurationEntry("__SIG", placeholder.c_str());
	}
	desc.beginInitialization(desc.getRevision());

	if (!parseInternally(desc)) return false;
	assert(getNbClauses() > 0);

	// Store # variables and # clauses in app config
	std::vector<std::pair<int, std::string>> fields {
		{_num_read_clauses, "__NC"},
		{_max_var, "__NV"},
		{_objective.size(), "__NO"}
	};
	for (auto [nbRead, dest] : fields) {
		std::string nbStr = std::to_string(nbRead);
		assert(nbStr.size() < NC_DEFAULT_VAL.size());
		while (nbStr.size() < NC_DEFAULT_VAL.size())
			nbStr += ".";
		desc.setAppConfigurationEntry(dest, nbStr);
	}

	desc.endInitialization();

	if (_pipe != nullptr) pclose(_pipe);
	if (_namedpipe != -1) close(_namedpipe);

	if (_contains_empty_clause) {
		return false;
	}

	return isValidInput();
}
