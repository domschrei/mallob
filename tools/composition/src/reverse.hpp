#ifndef REVERSE_H
#define REVERSE_H

#include "util.hpp"

//Reverse the proof lines from filename into revfilename
//Returns a result code/error code
result_code_t reverse_file(char *filename, char *revfilename);

#endif
