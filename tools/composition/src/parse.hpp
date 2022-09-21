#ifndef PARSE_H
#define PARSE_H

#include "util.hpp"

//Read and parse a single add line in a proof file, moving past any other lines
//Returns a result code
//Based on parsing in lrat-check from drat-trim
result_code_t parse_line(FILE *file, char *filename, clause_t *clause,
                         bool_t read_lrat, bool_t is_loose);

//Read and parse a single add line in a binary proof file, moving past other lines
//Returns a result code
//Based on parsing in lrat-check from drat-trim
result_code_t parse_binary_line(FILE *file, char *filename, clause_t *clause,
                                bool_t read_lrat, bool_t is_loose);

//Read a single add line from a proof file from instance file_index, moving past non-add lines
//read_binary is true if the input files are in binary format
//Returns a result code
result_code_t read_input_file_line(int32_t file_index, combine_problem_t *problem, bool_t read_binary,
                                   bool_t read_lrat, bool_t is_loose);

//Read the "p cnf <vars> <clauses>" header of a DIMACS file and enter
//   the number of original clauses into num_original_clauses_loc
//Returns a result code
result_code_t parse_cnf_file(char *filename, int32_t *num_original_clauses_loc);

//Write an added clause into outfile
//Returns a code for success or the type of failure
result_code_t output_added_clause(clause_t *clause, FILE *outfile, bool_t frat_format);

//Write a delete line into outfile:  id  d clauses 0
//Returns a code for success or the type of failure
result_code_t output_delete_clauses(clause_id_t clause_id, std::vector<clause_id_t>& clauses, FILE *outfile);

#endif
