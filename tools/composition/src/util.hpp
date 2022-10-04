#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cinttypes>
#include <vector>
#include <string>


typedef char** filename_array;


//error codes/result codes
typedef int16_t result_code_t;
#define PRI_RESULT_CODE PRId16

#define SUCCESS                  0
#define FAILED_ALLOCATION        1
#define FAILED_WRITE             2
#define FAILED_READ              3
#define NO_FILE                  4
#define UNABLE_TO_OPEN           5
#define BAD_USAGE                6
#define UNKNOWN_CLAUSE           7
#define EMPTY_CLAUSE_NOT_FOUND   8
#define PARSE_ERROR              9
#define END_OF_FILE             10
#define CLAUSE_CYCLE            11
#define WRONG_FILE_CLAUSE       12
#define FORK_ERROR              13
#define FILE_CLOSE_ERROR        14



typedef short bool_t;
#define true 1
#define false 0



//Identifier for a clause
typedef int64_t clause_id_t;
#define PRI_CLAUSE_ID PRId64
#define SCN_CLAUSE_ID SCNd64

//Get the 0-based index of the file containing clause
static inline int32_t get_file_index(clause_id_t clause, int32_t num_original_clauses, int32_t num_files){
    int32_t file_num = (clause - num_original_clauses) % num_files;
    return (file_num == 0 ? num_files : file_num) - 1;
}



//Information about a clause
struct clause_t {
    //identifer for this clause
    clause_id_t clause_id;
    //vector of literals forming this clause
    std::vector<int32_t> literals;
    //vector of clauses used in proof
    std::vector<clause_id_t> proof_clauses;

    explicit clause_t(std::vector<int32_t>::size_type literal_vector_size,
                      std::vector<clause_id_t>::size_type proof_clause_vector_size) :
        literals(literal_vector_size),
        proof_clauses(proof_clause_vector_size) { }

    explicit clause_t() :
        //numbers based on problems we've looked at to approximate the sizes
        //should be few resizes
        literals(1000),
        proof_clauses(5000) { }
};
typedef struct clause_t clause_t;

//Reset the clause to put a new clause into it
//Necessary to reset because we aren't reallocating for each new clause we read
static inline void reset_clause(clause_t *clause){
    clause->clause_id = 0;
    clause->literals.clear();
    clause->proof_clauses.clear();
}

static inline clause_id_t get_clause_id(clause_t *clause){
    return clause->clause_id;
}

static inline void set_clause_id(clause_t *clause, clause_id_t id){
    clause->clause_id = id;
}

//Add a literal to the clause, returns a result code
static inline result_code_t add_clause_literal(clause_t *clause, int32_t lit){
    clause->literals.push_back(lit);
    return SUCCESS;
}

//Add a proof clause ID to the clause, returns a result code
static inline result_code_t add_clause_proof_clause(clause_t *clause, clause_id_t clause_id){
    clause->proof_clauses.push_back(clause_id);
    return SUCCESS;
}

//whether the clause has any literals
static inline bool_t is_clause_empty(clause_t *clause){
    return clause->literals.size() == 0;
}

//Print a clause---for debugging purposes only
void print_clause(clause_t *clause);

//Output a clause to a file
//Returns a code for success or the type of failure
result_code_t output_added_clause(clause_t *clause, FILE *outfile,
                                  bool_t frat_format, bool_t is_binary);

//Output a clause to a file in binary, with the file contents in reverse byte order
result_code_t output_lrat_binary_backward(clause_t *clause, FILE *outfile);

//Write a delete line into outfile:  id  d clauses 0
//Returns a code for success or the type of failure
result_code_t output_delete_clauses(clause_id_t clause_id, std::vector<clause_id_t>& clauses,
                                    FILE *outfile, bool is_binary);

//Write a binary delete line into outfile in reverse byte order
result_code_t output_backward_delete_clauses(clause_id_t id, std::vector<clause_id_t>& clauses,
                                             FILE *outfile);



//Hold a file for reading and maintaining a read line which has yet to be output
struct file_t {
    //next clause to output
    //MUST read another clause immediately after outputting to maintain this correctly
    clause_t clause;
    //ID for the last clause from this file which was output to the file
    clause_id_t last_clause_output;
    //whether the clause is an actual clause or garbage
    bool_t has_another_clause;
    //file from which we are reading
    FILE *file;
    //name of the file for better error messages
    char *filename;

    explicit file_t() :
        //no extra information to know clause lengths, so call basic constructor
        clause() { }
};
typedef struct file_t file_t;

static inline clause_t *get_file_clause(file_t *f){
    return &f->clause;
}

static inline FILE *get_file_file(file_t *f){
    return f->file;
}

static inline char *get_file_filename(file_t *f){
    return f->filename;
}

//Remove the last clause from a file after it has been output to prepare for reading the next line
void remove_output_clause(file_t *file);

//Set whether the file has another clause read into it
static inline void set_file_has_another_clause(file_t *f, bool_t yesno){
    f->has_another_clause = yesno;
}

//Get whether the file has another clause read into it to process
static inline bool_t get_file_has_another_clause(file_t *f){
    return f->has_another_clause;
}



//Hold all the files in the current problem, along with other information
struct combine_problem_t;
typedef struct combine_problem_t combine_problem_t;

//Create a storage for the problem information
//Note filenames_arr is expected to have length num_files
//Note problem_loc is a pointer to a space for holding a pointer---we need to do it
//   this way to create one outside the file defining it because the type is abstract
//Returns an error code and puts the built structure in the problem_loc
result_code_t initialize_problem(std::vector<std::string> filenames_arr,
                                 combine_problem_t **problem);

//Get the total number of files
int32_t get_num_files(combine_problem_t *problem);

//Get a particular file---assumes index is valid for the number of files contained in files
file_t *get_file(combine_problem_t *problem, int32_t index);

//Get the known number of original clauses
int32_t get_num_original_clauses(combine_problem_t *problem);

//Add an original clause ID to the problem
void add_original_clause_count(combine_problem_t *problem, int64_t num_clauses);

//Determine whether we can output the current clause, meaning all its
//   dependencies have already been output
bool_t valid_to_output(clause_t *clause, combine_problem_t *problem);

//Free the storage
void free_problem(combine_problem_t *problem);


#endif
