#ifndef PRUNE_H
#define PRUNE_H

//Remove unneeded clauses from the file and insert deletes when possible
//The input file infilename must be in reverse order with the empty clause at the end
//The output file outfilename will also be in reverse order
//Returns a result code
result_code_t prune_proof(char *infilename, char *outfilename, int32_t num_instances,
                          int32_t num_original_clauses);

#endif
