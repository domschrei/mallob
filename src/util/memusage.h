
#ifndef MALLOB_MEMORY_USAGE_H
#define MALLOB_MEMORY_USAGE_H

#include <unistd.h>
#include <ios>
#include <iostream>
#include <fstream>
#include <string>

// https://stackoverflow.com/a/671389
//
// process_mem_usage(double &, double &) - takes two doubles by reference,
// attempts to read the system-dependent data for a process' virtual memory
// size and resident set size, and return the results in KB.
//
// On failure, returns 0.0, 0.0
void process_mem_usage(int& cpu, double& vm_usage, double& resident_set);

#endif