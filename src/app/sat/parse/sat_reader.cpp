
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

#include "app/sat/data/formula_compressor.hpp"
#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "app/sat/proof/trusted_parser_process_adapter.hpp"
#include "sat_reader.hpp"
#include "data/job_description.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/tmpdir.hpp"
#include "data/app_configuration.hpp"
#include "util/option.hpp"

void handleUnsat(const Parameters& _params) {
	LOG_OMIT_PREFIX(V0_CRIT, "s UNSATISFIABLE\n");
	if (_params.proofOutputFile.isSet()) {
		// Output Mallob file with result code
		std::ofstream resultFile(".mallob_result");
		std::string resultCodeStr = std::to_string(20);
		if (resultFile.is_open()) resultFile.write(resultCodeStr.c_str(), resultCodeStr.size());
		// Create empty proof file
		std::ofstream ofs(_params.proofOutputFile());
	}
}

bool SatReader::parseWithTrustedParser(JobDescription& desc) {
	// Parse and sign in a separate subprocess

	if (!_tppa) {
		_tppa.reset(new TrustedParserProcessAdapter(_params.seed(), desc.getId()));
		_tppa->setup(_filename.c_str());
	}

	uint8_t* sig;
	std::vector<unsigned char> plain;
	std::vector<unsigned char>* out;
	if (_params.compressFormula()) out = &plain;
	else out = desc.getRevisionData(desc.getRevision()).get();

	bool ok = _tppa->parseAndSign(*out, sig);
	if (!ok) return false;

	std::string sigStr = Logger::dataToHexStr(sig, SIG_SIZE_BYTES);
	_max_var = _tppa->getNbVars();
	_num_read_clauses = _tppa->getNbClauses();
	desc.setFSize(_tppa->getFSize());
	LOG(V2_INFO, "IMPCHK parser -key-seed=%lu read %i vars, %i cls - sig %s\n",
		ImpCheck::getKeySeed(_params.seed()), _max_var, _num_read_clauses, sigStr.c_str());

	if (_params.compressFormula()) {
		auto vec = desc.getRevisionData(desc.getRevision()).get();
		auto outSizeBytesBefore = vec->size();
		FormulaCompressor::VectorFormulaOutput cOut(vec);
		FormulaCompressor::compress((const int*) out->data(), out->size() / sizeof(int), 0, 0, cOut, true);
		desc.setFSize((cOut.vec->size() - outSizeBytesBefore) / sizeof(int));
	}

	_input_finished = true;
	_input_invalid = false;
	return true;
}

bool SatReader::parseAndCompress(JobDescription& desc) {
	FormulaCompressor c;
	auto vec = desc.getRevisionData(desc.getRevision()).get();
	auto outSizeBytesBefore = vec->size();
	FormulaCompressor::VectorFormulaOutput out(vec);
	_input_invalid = !c.readAndCompress(_filename.c_str(), out);
	_input_finished = true;
	desc.setFSize((out.vec->size() - outSizeBytesBefore) / sizeof(int));
	_max_var = out.maxVar;
	_num_read_clauses = out.nbClauses;
	return !_input_invalid;
}

bool SatReader::parseInternally(JobDescription& desc) {

	_raw_content_mode = desc.getAppConfiguration().map.count("content-mode")
		&& desc.getAppConfiguration().map.at("content-mode") == "raw";

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

		if (_params.satPreprocessor.isSet()) {

			std::string newFilename = _params.logDirectory() + "/input_units_removed.cnf";
			remove(newFilename.c_str()); // remove if existing (ignore errors)
			//std::string cmd = "cadical " + _filename + " -c 0 -o " + newFilename;

			float time = Timer::elapsedSeconds();
			std::string cmd = _params.satPreprocessor() + " " + _filename + " > " + newFilename;
			int systemRetVal = system(cmd.c_str());
			time = Timer::elapsedSeconds() - time;

			int returnCode = WEXITSTATUS(systemRetVal);
			if (returnCode == 10) {
				LOG(V2_INFO, "external call to CaDiCaL found result SAT\n");
				LOG_OMIT_PREFIX(V0_CRIT, "s SATISFIABLE\n");
				return false;
			} else if (returnCode == 20) {
				LOG(V2_INFO, "external call to CaDiCaL found result UNSAT\n");
				handleUnsat(_params);
				return false;
			} else assert(returnCode == 0 || log_return_false("Unexpected return code %i\n", returnCode));

			_filename = newFilename;
			LOG(V2_INFO, "TIMING preprocessing %.3f\n", time);
		}

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

	} else if (_namedpipe != -1) {
		// Read formula over named pipe

		int iteration = 0;
		if (_raw_content_mode) {
			int buffer[1024] = {0};
			while ((iteration ^ 511) != 0 || !Terminator::isTerminating()) {
				int numRead = ::read(_namedpipe, buffer, sizeof(buffer));
				if (numRead <= 0) break;
				numRead /= sizeof(int);
				for (int i = 0; i < numRead; i++) {
					processInt(buffer[i], desc);
				}
				iteration++;
			}
		} else {
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
		}

	} else {
		// Read file over pipe
		if (_raw_content_mode) {
			int iteration = 0;
			int buffer[1024] = {0};
			while ((iteration ^ 511) != 0 || !Terminator::isTerminating()) {
				int numRead = ::read(fileno(_pipe), buffer, sizeof(buffer));
				if (numRead <= 0) break;
				numRead /= sizeof(int);
				for (int i = 0; i < numRead; i++) {
					int c = buffer[i];
					processInt(c, desc);
				}
				iteration++;
			}
		} else {
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
	}
	return true;
}

bool SatReader::read(JobDescription& desc) {

	std::optional<std::future<void>> optFuture;
	std::vector<int> litsToParse;
	if (_files.empty()) {
		// No explicit file provided, so nothing to do.
		if (!_params.onTheFlyChecking()) return true;

		// With real-time checking, we *still* need to explicitly parse,
		// so we route the added formula payload over a pipe.
		_filename = TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallob." + std::to_string(Proc::getPid())
			+ ".tsinput." + std::to_string(desc.getId());
		if (!_tppa) {
			// Create a new pipe file and parser adapter for this job
			mkfifo(_filename.c_str(), 0666);
			_tppa.reset(new TrustedParserProcessAdapter(_params.seed(), desc.getId()));
			_tppa->setup(_filename.c_str());
		}
		// Write formula to the pipe in a side thread
		litsToParse = std::move(desc.getPreloadedLiterals());
		desc.getPreloadedLiterals().clear();
		optFuture = ProcessWideThreadPool::get().addTask([&]() {
			// Output formula increment to the pipe file
			std::ofstream ofs(_filename);
			for (int lit : litsToParse) {
				if (lit == INT32_MIN) break;
				if (lit == INT32_MAX) {
					ofs << "a ";
					continue;
				}
				ofs << lit;
				if (lit == 0) ofs << "\n";
			}
		});
	}
	if (_filename.empty()) {
		assert(_files.size() == 1);
		_filename = _files.front();
	}

	const std::string NC_DEFAULT_VAL = "BMMMKKK111";
	desc.setAppConfigurationEntry("__NC", NC_DEFAULT_VAL);
	desc.setAppConfigurationEntry("__NV", NC_DEFAULT_VAL);
	desc.beginInitialization(desc.getRevision());

	if (_params.onTheFlyChecking()) {
		auto ok = parseWithTrustedParser(desc);
		if (optFuture.has_value()) optFuture->get();
		if (!ok) return false;
	} else if (_params.compressFormula()) {
		if (!parseAndCompress(desc)) return false;
	} else {
		if (!parseInternally(desc)) return false;
	}

	// Store # variables and # clauses in app config
	std::vector<std::pair<int, std::string>> fields {
		{_num_read_clauses, "__NC"},
		{_max_var, "__NV"}
	};
	for (auto [nbRead, dest] : fields) {
		std::string nbStr = std::to_string(nbRead);
		assert(nbStr.size() < NC_DEFAULT_VAL.size());
		while (nbStr.size() < NC_DEFAULT_VAL.size())
			nbStr += ".";
		desc.setAppConfigurationEntry(dest, nbStr);
	}

	if (_params.satPreprocessor.isSet()) {
		std::ofstream ofs(TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallob.preprocessed-header.pipe", std::ofstream::app);
		std::string out = "p cnf " + std::to_string(_max_var) + " " + std::to_string(_num_read_clauses) + "\n";
		if (ofs.is_open()) ofs.write(out.c_str(), out.size());
	}

	desc.endInitialization();

	if (_pipe != nullptr) pclose(_pipe);
	if (_namedpipe != -1) close(_namedpipe);

	if (_contains_empty_clause) {
		handleUnsat(_params);
		return false;
	}

	return isValidInput();
}
