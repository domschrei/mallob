
#include <stdio.h>

#include "join.hpp"
#include "parse.hpp"


//Read and output all lines from a file which can be output now
//Sets empty_clause_output and some_clause_output as appropriate
result_code_t run_one_file(combine_problem_t *p, int32_t file_index,
                           FILE *outfile, bool_t read_binary,
                           bool_t *empty_clause_output,
                           bool_t *some_clause_output,
                           bool_t read_lrat, bool_t is_loose){
    file_t *f = get_file(p, file_index);
    //for efficiency, continue with this file as long as we can output lines
    while (!*empty_clause_output &&
           get_file_has_another_clause(f) &&
           valid_to_output(get_file_clause(f), p)){

        //output the current clause
        output_added_clause(get_file_clause(f), outfile, false, read_binary);
        *empty_clause_output = is_clause_empty(get_file_clause(f)); //no literals
        *some_clause_output = true;

        //Remove the last clause from the file to prepare for reading the next one
        remove_output_clause(f);

        //read the next line
        result_code_t result = read_input_file_line(file_index, p, read_binary,
                                                    read_lrat, is_loose);
        if (result == END_OF_FILE){
            set_file_has_another_clause(f, false);
        }
        if (result != SUCCESS){
            return result; //error
        }

    }
    return SUCCESS;
}


//Run through all files for a round
//Set empty_clause_output as appropriate
result_code_t run_all_files(combine_problem_t *p, FILE *outfile, bool_t read_binary,
                            bool_t *empty_clause_output, bool_t read_lrat, bool_t is_loose){
    int32_t file_index = 0;
    int32_t count_EOF = 0; //count how many files are out of clauses in the round
    int32_t num_files = get_num_files(p);
    bool_t some_clause_output = false; //check something happened this round
    //walk through all the files for the round
    while (!*empty_clause_output && file_index < num_files){
        file_t *f = get_file(p, file_index);
        result_code_t result = run_one_file(p, file_index, outfile, read_binary,
                                            empty_clause_output, &some_clause_output,
                                            read_lrat, is_loose);
        if (result != SUCCESS && result != END_OF_FILE){
            return result;
        }
        //check if this file is done
        if (!get_file_has_another_clause(f)){
            count_EOF++;
        }

        file_index++;
    }

    //empty clause doesn't exist if all files done without finding it
    if (!*empty_clause_output && count_EOF == num_files){
        return EMPTY_CLAUSE_NOT_FOUND;
    }
    //no clause output in a round means we're stuck
    else if (!*empty_clause_output && !some_clause_output){
        return CLAUSE_CYCLE;
    }
    return SUCCESS;
}


result_code_t build_proof(combine_problem_t *p, FILE *outfile, bool_t read_binary,
                          bool_t read_lrat, bool_t is_loose){
    int32_t num_files = get_num_files(p);
    result_code_t result;
    //read an initial line into each file
    for (int32_t file_index = 0; file_index < num_files; file_index++){
        result = read_input_file_line(file_index, p, read_binary,
                                      read_lrat, is_loose);
        if (result == END_OF_FILE){
            //ignore it here; we'll see it later
        }
        else if (result != SUCCESS){
            return result; //error
        }
    }

    //go through the files until all the clauses up to the empty clause are entered
    //walk through them in rounds so we can detect the empty clause missing and cycles by counting
    result = SUCCESS;
    bool_t empty_clause_output = false;
    while (!empty_clause_output && result == SUCCESS){
        //walk through all the files for the round
        result = run_all_files(p, outfile, read_binary, &empty_clause_output,
                               read_lrat, is_loose);
    }
    if (empty_clause_output){
        return SUCCESS;
    }
    return result;
}


result_code_t combine_input_files(char *outfilename, std::vector<std::string> infilenames,
                                  bool_t is_binary, int32_t num_original_clauses,
                                  bool_t read_lrat, bool_t is_loose){
    int32_t num_infiles = infilenames.size();

    //create the problem store
    combine_problem_t *p = NULL;
    result_code_t result = initialize_problem(infilenames, &p);
    switch (result){
    case FAILED_ALLOCATION:
        printf("Error:  Unable to allocate enough memory in initializing files\n");
        return result;
        break;
    case NO_FILE:
        //message should be printed for specific file already
        return result;
        break;
    case UNABLE_TO_OPEN:
        //message should be printed for specific file already
        return result;
        break;
    case SUCCESS:
        //nothing to print because it worked
        break;
    default:
        printf("Internal error:  Error code %" PRI_RESULT_CODE " should ", result);
        printf("not be returned from initializing the input files\n");
        return result;
    }
    add_original_clause_count(p, num_original_clauses);


    //open the output file
    FILE *outfile = fopen(outfilename, "w");
    if (outfile == NULL){
        printf("Error:  Unable to open output file %s\n", outfilename);
        free_problem(p);
        return UNABLE_TO_OPEN;
    }


    //build the new file
    result = build_proof(p, outfile, is_binary, read_lrat, is_loose);

    //print a message for the error that occurred if no message was printed before
    switch (result){
    case FAILED_ALLOCATION:
        printf("Error:  Unable to allocate enough memory in composing proof\n");
        break;
    case FAILED_WRITE:
        printf("Error:  Unable to write to file\n");
        break;
    case EMPTY_CLAUSE_NOT_FOUND:
        printf("Error:  Incomplete problem:  Could not find empty clause\n");
        break;
    case CLAUSE_CYCLE:
        printf("Error:  Unable to output clause from any file (");
        printf("looking at clauses");
        for (int32_t i = 0; i < num_infiles; i++){
            file_t *file = get_file(p, i);
            if (get_clause_id(get_file_clause(file)) != 0){
                printf(" %" PRI_CLAUSE_ID, get_clause_id(get_file_clause(file)));
            }
        }
        printf(")\n");
        break;
    case SUCCESS:
        //nothing to print because it worked
        break;
    case FAILED_READ:
        //message should be printed for specific file already
        break;
    case PARSE_ERROR:
        //message should be printed for specific error already
        break;
    case WRONG_FILE_CLAUSE:
        //message should be printed for specific clause and file already
        break;
    default:
        //anything else should not be returned
        printf("Internal error:  Error code %" PRI_RESULT_CODE " should not ", result);
        printf("be returned from building the proof\n");
    }

    if (fclose(outfile) == EOF){
        //error trying to close the file
        printf("Error:  Unable to close file %s\n", outfilename);
    }

    free_problem(p);

    return result;
}

