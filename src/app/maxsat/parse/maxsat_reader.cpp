
#include <climits>
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

#include "parserinterface.hpp"
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

	auto& config = desc.getAppConfiguration();
	config.updateFixedSizeEntry("__NC", 0);
	config.updateFixedSizeEntry("__NV", 0);
	config.updateFixedSizeEntry("__NO", 0);
	config.updateFixedSizeEntry("__XL", 0);
	config.updateFixedSizeEntry("__XU", 0);
	if (_params.onTheFlyChecking()) {
		std::string placeholder(32, 'x');
		desc.setAppConfigurationEntry("__SIG", placeholder.c_str());
	}
	desc.beginInitialization(desc.getRevision());

	std::unique_ptr<maxPreprocessor::ParserInterface> parser;
	if (_params.maxPre()) {
		parser.reset(new maxPreprocessor::ParserInterface());
		std::ifstream ifs {_filename};
		float time = Timer::elapsedSeconds();
		int res = parser->read_file_init_interface(ifs);
		const float timeParse = Timer::elapsedSeconds() - time;
		if (res != 0) return false;
		time = Timer::elapsedSeconds();
		parser->preprocess(_params.maxPreTechniques(), 0, _params.maxPreTimeout());
		const float timePreprocess = Timer::elapsedSeconds() - time;
		std::vector<int> formula;
		parser->getInstance(formula, _objective, _max_var, _num_read_clauses);
		LOG(V3_VERB, "MAXSAT MaxPRE stat lits:%i vars:%i cls:%i obj:%lu\n", formula.size(), _max_var, _num_read_clauses, _objective.size());
		LOG(V3_VERB, "MAXSAT MaxPRE time parse:%.3f preprocess:%.3f\n", timeParse, timePreprocess);

		// Parcel job description
		for (int lit : formula) desc.addPermanentData(lit);
		desc.addPermanentData(0);
		for (auto& [weight, lit] : _objective) {
			// Need to write each 64-bit weight as two 32-bit integers ...
			const int* weightAsTwoInts = (int*) &weight;
			desc.addPermanentData(weightAsTwoInts[0]);
			desc.addPermanentData(weightAsTwoInts[1]);
			desc.addPermanentData(lit);
		}
		desc.addPermanentData((int) _objective.size());
	} else {
		if (!parseInternally(desc)) return false;
	}

	unsigned long lb = parser ? parser->get_lb() : 0;
	unsigned long ub = parser ? parser->get_ub() : ULONG_MAX;
	desc.addPermanentData(((int*) &lb)[0]);
	desc.addPermanentData(((int*) &lb)[1]);
	desc.addPermanentData(((int*) &ub)[0]);
	desc.addPermanentData(((int*) &ub)[1]);

	assert(getNbClauses() > 0);

	config.updateFixedSizeEntry("__NC", _num_read_clauses);
	config.updateFixedSizeEntry("__NV", _max_var);
	config.updateFixedSizeEntry("__NO", (int)_objective.size());
	config.updateFixedSizeEntry("__XL", -1);
	config.updateFixedSizeEntry("__XU", -1);

	desc.endInitialization();

	if (_pipe != nullptr) pclose(_pipe);
	if (_namedpipe != -1) close(_namedpipe);

	if (_contains_empty_clause) {
		return false;
	}

	return _params.maxPre() || isValidInput();
}
