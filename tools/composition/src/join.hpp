#ifndef JOIN_H
#define JOIN_H

#include "util.hpp"


/*
 *Combine all the added clauses from the input files into a single LRAT file
 *Returns a code for success or the type of failure
 *  outfilename:  file into which to write the combined proof
 *  infilenames:  names of the files containing proofs from the instances in instance_num-order
 *  num_infiles:  number of names in infilenames
 *  is_binary:    whether the input files are in binary format
 *  num_original_clauses:  number of original clauses in the problem
 *  read_lrat:  true if the input files are in LRAT format, false for FRAT
 *  is_loose:  treat syntax errors as the end of the file instead of as an error
 */
result_code_t combine_input_files(char *outfilename, std::vector<std::string> infilenames,
                                  bool_t is_binary, int32_t num_original_clauses,
                                  bool_t read_lrat, bool_t is_loose);


/*
 *Build the proof of deriving the empty clause, putting it in outfile
 */
result_code_t build_proof(combine_problem_t *problem, FILE* outfile, bool_t read_binary,
                          bool_t read_lrat, bool_t is_loose);


#endif
