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

//Read either an add line or a delete line in LRAT format from infile
//Add lines populate clause and set wrote_add_clause to true
//Delete lines populate delete_clause and set wrote_add_clause to false
result_code_t parse_any_line(FILE *infile, clause_t *clause,
                             std::vector<clause_id_t>& delete_clause,
                             bool_t *wrote_add_clause);

//Read either an add line or a delete line in binary LRAT format from infile
//Add lines populate clause and set wrote_add_clause to true
//Delete lines populate delete_clause and set wrote_add_clause to false
result_code_t parse_any_binary_line(FILE *infile, clause_t *clause,
                                    std::vector<clause_id_t>& delete_clause,
                                    bool_t *wrote_add_clause);

//Read the "p cnf <vars> <clauses>" header of a DIMACS file and enter
//   the number of original clauses into num_original_clauses_loc
//Returns a result code
result_code_t parse_cnf_file(char *filename, int32_t *num_original_clauses_loc);

//Read a signed literal int64 in the variable-length encoding of the binary DRAT format
//   https://github.com/marijnheule/drat-trim#binary-drat-format
result_code_t read_clause_id(FILE *file, clause_id_t *id);

//Read a signed literal in the variable-length encoding of the binary DRAT format
//   https://github.com/marijnheule/drat-trim#binary-drat-format
result_code_t read_lit(FILE *file, int32_t *lit);

#endif
