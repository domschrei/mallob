#ifndef REVERSE_H
#define REVERSE_H

#include "util.hpp"

//Reverse the proof lines from filename into revfilename
//Returns a result code/error code
result_code_t reverse_file(char *filename, char *revfilename, bool_t is_binary);


//Write the bytes of filename out to revfilename in reverse order
//e.g. "olleh" in filename becomes "hello" in revfilename
result_code_t byte_reverse_file(char *filename, char *revfilename);

#endif
