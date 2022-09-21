
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "parse.hpp"


/********************************************************************
 *                        TEXT PROOF PARSING                        *
 ********************************************************************/

//Read an original clause, assuming the initial 'o' has already been read
result_code_t parse_original_line(FILE *file, char *filename, bool_t is_loose){
    clause_id_t id;
    int32_t tmp;
    int32_t lit;
    tmp = fscanf(file, "%" SCN_CLAUSE_ID, &id);
    if (tmp == EOF){
        return END_OF_FILE;
    }
    else if (tmp == 1){
        //read all literals
        tmp = fscanf(file, " %" SCNd32, &lit);
        while (tmp != EOF && tmp == 1 && lit != 0){
            tmp = fscanf(file, " %" SCNd32, &lit);
        }
        if (tmp == 0){
            if (is_loose){
                return END_OF_FILE;
            }
            printf("Parse error in file %s:  ", filename);
            printf("Could not parse literals for original clause %" PRI_CLAUSE_ID "\n", id);
            return PARSE_ERROR;
        }
        else if (tmp == EOF){ //assume Mallob killed it
            return END_OF_FILE;
        }
    }
    return SUCCESS;
}


//Read an add clause
//FRAT:  assume the initial 'a' has already been read, read_lrat = false
//LRAT:  clause ID is in id, read_lrat = true
result_code_t parse_add_line(FILE *file, char *filename, clause_t *clause,
                             clause_id_t id, bool_t read_lrat, bool_t is_loose){
    int32_t lit;
    int32_t tmp;

    if (!read_lrat){
        tmp = fscanf(file, "%" SCN_CLAUSE_ID, &id);
        if (tmp == EOF){ //assume Mallob killed it
            return END_OF_FILE;
        }
    }
    set_clause_id(clause, id);

    //read all literals
    int32_t this_clause_lit_count = 0;
    tmp = fscanf(file, " %" SCNd32, &lit);
    while (tmp != EOF && tmp == 1 && lit != 0){
        add_clause_literal(clause, lit);
        this_clause_lit_count++;
        tmp = fscanf(file, " %" SCNd32, &lit);
    }
    if (tmp == 0){
        if (is_loose){
            return END_OF_FILE;
        }
        printf("Parse error in file %s:  ", filename);
        printf("Could not parse add clause literals for clause %" PRI_CLAUSE_ID "\n", id);
        return PARSE_ERROR;
    }
    else if (tmp == EOF){
        //assume incomplete add clauses are due to Mallob killing the process
        //therefore it is not an error, just another instance finishing first
        return END_OF_FILE;
    }

    if (!read_lrat){
        //remove 'l' if this is an FRAT proof
        fscanf(file, " l"); //don't care about result
    }

    //read all proof clauses
    int32_t proof_clause_count = 0;
    clause_id_t proof_clause_id;
    tmp = fscanf(file, " %" SCN_CLAUSE_ID, &proof_clause_id);
    while (tmp != EOF && tmp == 1 && proof_clause_id != 0){
        add_clause_proof_clause(clause, proof_clause_id);
        proof_clause_count++;
        tmp = fscanf(file, " %" SCN_CLAUSE_ID, &proof_clause_id);
    }
    if (tmp == 0){
        if (is_loose){
            return END_OF_FILE;
        }
        printf("Parse error in file %s:  ", filename);
        printf("Incomplete add clause proof for clause %" PRI_CLAUSE_ID "\n", id);
        return PARSE_ERROR;
    }
    else if (tmp == EOF){
        //empty clause should always succeed in writing its proof
        if (this_clause_lit_count == 0){
            printf("Parse error in file %s:  ", filename);
            printf("File ended before proof for empty clause %" PRI_CLAUSE_ID " was finished\n", id);
            return PARSE_ERROR;
        }
        //as above, anything else incomplete is due to Mallob killing the process
        //therefore it is not an error, just another instance finishing first
        return END_OF_FILE;
    }

    return SUCCESS;
}


//Read another type of clause---f, d, r, or t---assuming the initial char has been read
result_code_t read_other_clause(FILE *file, char *filename, char c, bool_t is_loose){
    clause_id_t id;
    int32_t tmp;
    tmp = fscanf(file, " %" SCN_CLAUSE_ID, &id);
    while (tmp != 0 && tmp != EOF && id != 0){
        tmp = fscanf(file, " %" SCN_CLAUSE_ID, &id);
    }
    if (tmp == 0){
        if (is_loose){
            return END_OF_FILE;
        }
        printf("Parse error in file %s:  Incomplete other clause ", filename);
        printf("starting with '%c'\n", c);
        return PARSE_ERROR;
    }
    else if (tmp == EOF){
        //assume anything incomplete is due to another instance
        //   finishing first and Mallob killing this one
        return END_OF_FILE;
    }
    //otherwise end of the clause and nothing to do
    return SUCCESS;
}


result_code_t parse_line(FILE *file, char *filename, clause_t *clause,
                         bool_t read_lrat, bool_t is_loose){
    char c;
    result_code_t result = SUCCESS;

    bool_t add_clause_read = false;

    while (!add_clause_read){
        if (read_lrat){
            clause_id_t id;
            int32_t tmp = fscanf(file, "%" SCN_CLAUSE_ID, &id);
            if (tmp == EOF){
                result = END_OF_FILE;
            }
            else if (tmp == 0){
                c = getc(file);
                if (c == 'c'){ //move past a comment
                    while (c != EOF && c != '\n'){
                        c = getc(file);
                    }
                    if (c == EOF){
                        result = END_OF_FILE;
                    }
                }
                else{
                    if (is_loose){
                        result = END_OF_FILE;
                    }
                    else{
                        printf("Error:  Cannot parse next entry in LRAT file %s ", filename);
                        printf("not starting with a clause ID\n");
                        result = PARSE_ERROR;
                    }
                }
            }
            else{
                //since we ignore delete clauses anyway, we can read one here to make it catch
                clause_id_t ignore;
                tmp = fscanf(file, " d %" SCN_CLAUSE_ID, &ignore);
                if (tmp == EOF){
                    //assume this being incomplete is due to another instance finishing
                    //Mallob killed this, so this isn't an error, just an end
                    result = END_OF_FILE;
                }
                else if (tmp == 1){ //delete line
                    result = read_other_clause(file, filename, 'd', is_loose);
                }
                else{ //add line
                    result = parse_add_line(file, filename, clause, id, read_lrat, is_loose);
                    add_clause_read = true;
                }
            }
        }
        else{
            c = getc(file);
            switch (c){
            case EOF:
                result = END_OF_FILE;
                break;
            case 'c':  //move past a comment
                while (c != EOF && c != '\n'){
                    c = getc(file);
                }
                if (c == EOF){
                    result = END_OF_FILE;
                }
                break;
            case 'o':  //check for an original clause
                result = parse_original_line(file, filename, is_loose);
                break;
            case 'a':  //check for an add clause and build the clause
                result = parse_add_line(file, filename, clause, 0, read_lrat, is_loose);
                add_clause_read = true;
                break;
            case 'f': //check for finalize clause
                //nothing of any importance can come after a finalize clause
                result = END_OF_FILE;
                break;
            case 'd': case 'r': case 't': //move past other clauses we don't care about
                result = read_other_clause(file, filename, c, is_loose);
                break;
            case '\n': case ' ': case '\t': case '\r':  //ignore whitespace
                //nothing to do here---just go around again without getting caught by the default
                break;
            default:  //Anything not caught above is an error
                if (is_loose){
                    result = END_OF_FILE;
                }
                else{
                    printf("Error:  Cannot parse next entry in file %s starting with '%c'\n", filename, c);
                    result = PARSE_ERROR;
                }
            }
        }

        if (result != SUCCESS){
            return result;
        }
    }

    return SUCCESS;
}





/********************************************************************
 *                       BINARY PROOF PARSING                       *
 ********************************************************************/

//Read a signed literal in the variable-length encoding of the binary DRAT format
//   https://github.com/marijnheule/drat-trim#binary-drat-format
result_code_t read_lit(FILE *file, int32_t *lit){
    int32_t unadjusted = 0;
    int32_t coefficient = 1;
    int32_t tmp = getc(file);
    //while the most-signgficant bit is 1, meaning the number continues
    while (tmp != EOF && (tmp & 0b10000000)){
        unadjusted += coefficient * (tmp & 0b01111111); //remove first bit
        //ready for next step
        coefficient *= 128; //2^7 because we essentially have 7-bit bytes
        tmp = getc(file);
    }
    if (tmp == EOF){
        //assume Mallob killed this instance while writing
        return END_OF_FILE;
    }
    //add the last byte
    unadjusted += coefficient * tmp; //first bit is 0, so can leave it

    if (unadjusted % 2){ //odds map to negatives
        *lit = -(unadjusted - 1) / 2;
    }
    else{
        *lit = unadjusted / 2;
    }

    return SUCCESS;
}


//Read an unsigned literal int64 in the variable-length encoding of the binary DRAT
//   format, but without the signed part of the encoding
//   https://github.com/marijnheule/drat-trim#binary-drat-format
result_code_t read_unsigned_clause_id(FILE *file, clause_id_t *id){
    int64_t unadjusted = 0;
    int64_t coefficient = 1;
    int32_t tmp = getc(file);
    //while the most-signgficant bit is 1, meaning the number continues
    while (tmp != EOF && (tmp & 0b10000000)){
        unadjusted += coefficient * (tmp & 0b01111111); //remove first bit
        //ready for next step
        coefficient *= 128; //2^7 because we essentially have 7-bit bytes
        tmp = getc(file);
    }
    if (tmp == EOF){
        //assume Mallob killed this instance while writing
        return END_OF_FILE;
    }
    //add the last byte
    unadjusted += coefficient * tmp; //first bit is 0, so can leave it
    *id = unadjusted;

    return SUCCESS;
}


//Read a signed literal int64 in the variable-length encoding of the binary DRAT format
//   https://github.com/marijnheule/drat-trim#binary-drat-format
result_code_t read_clause_id(FILE *file, clause_id_t *id){
    int64_t unadjusted = 0;
    int64_t coefficient = 1;
    int32_t tmp = getc(file);
    //while the most-signgficant bit is 1, meaning the number continues
    while (tmp != EOF && (tmp & 0b10000000)){
        unadjusted += coefficient * (tmp & 0b01111111); //remove first bit
        //ready for next step
        coefficient *= 128; //2^7 because we essentially have 7-bit bytes
        tmp = getc(file);
    }
    if (tmp == EOF){
        //assume Mallob killed this instance while writing
        return END_OF_FILE;
    }
    //add the last byte
    unadjusted += coefficient * tmp; //first bit is 0, so can leave it

    if (unadjusted % 2){ //odds map to negatives
        *id = -(unadjusted - 1) / 2;
    }
    else{
        *id = unadjusted / 2;
    }

    return SUCCESS;
}


//Read a binary original clause line, assuming the 'o' has already been removed
result_code_t read_binary_original_clause(FILE *file, char *filename, bool_t is_loose){
    clause_id_t id;
    int32_t lit;
    //read the clause ID
    result_code_t tmp = read_unsigned_clause_id(file, &id);
    if (tmp == PARSE_ERROR){
        printf("Parse error in file %s:  ", filename);
        printf("File ended before original clause completed\n");
        return PARSE_ERROR;
    }
    else if (tmp == END_OF_FILE){
        return END_OF_FILE;
    }
    if (id == 0){
        if (is_loose){
            return END_OF_FILE;
        }
        printf("Parse error in file %s:  ", filename);
        printf("Original clause ended without clause ID\n");
        return PARSE_ERROR;
    }
    //read the literals
    tmp = read_lit(file, &lit);
    while (tmp != PARSE_ERROR && lit != 0){
        tmp = read_lit(file, &lit);
    }
    if (tmp == PARSE_ERROR){
        if (is_loose){
            return END_OF_FILE;
        }
        printf("Parse error in file %s:  ", filename);
        printf("File ended before original clause terminated\n");
        return PARSE_ERROR;
    }
    else if (tmp == END_OF_FILE){
        return END_OF_FILE;
    }
    return SUCCESS;
}


//Read a binary add clause line, assumgin the 'a' hass been removed
result_code_t read_binary_add_line(FILE *file, char *filename, clause_t *clause,
                                   bool_t read_lrat, bool_t is_loose){
    reset_clause(clause);
    clause_id_t id;
    int32_t lit;
    result_code_t tmp;
    //read the clause ID
    if (read_lrat){ //LRAT uses a signed clause ID
        tmp = read_clause_id(file, &id);
    }
    else{ //FRAT uses an unsigned clause ID
        tmp = read_unsigned_clause_id(file, &id);
    }
    if (tmp == PARSE_ERROR){
        printf("Parse error in file %s:  ", filename);
        printf("File ended before add clause completed\n");
        return PARSE_ERROR;
    }
    else if (tmp == END_OF_FILE){
        return END_OF_FILE;
    }
    if (id == 0){
        if (is_loose){
            return END_OF_FILE;
        }
        printf("Parse error in file %s:  ", filename);
        printf("Add clause ended without clause ID\n");
        return PARSE_ERROR;
    }
    set_clause_id(clause, id);

    //read the literals
    int32_t this_clause_lit_count = 0;
    tmp = read_lit(file, &lit);
    while (tmp == SUCCESS && lit != 0){
        add_clause_literal(clause, lit);
        this_clause_lit_count++;
        tmp = read_lit(file, &lit);
    }
    if (tmp == PARSE_ERROR){
        printf("Parse error in file %s:  ", filename);
        printf("File ended before add clause completed\n");
        return PARSE_ERROR;
    }
    else if (tmp == END_OF_FILE){
        return END_OF_FILE;
    }

    //read l to get it out of the way in FRAT file
    if (!read_lrat){
        tmp = getc(file);
        if (tmp == EOF){
            //assume Mallob killed this instance while writing
            return END_OF_FILE;
        }
        if (tmp != 'l'){ //had better actually be l
            if (is_loose){
                return END_OF_FILE;
            }
            printf("Parse error in file %s:  ", filename);
            printf("Expected 108 ('l') but found %" PRId32 " (%c)\n", tmp, tmp);
            return PARSE_ERROR;
        }
    }

    //read the clause proof
    int32_t proof_clause_count = 0;
    clause_id_t proof_clause_id;
    tmp = read_clause_id(file, &proof_clause_id);
    while (tmp == SUCCESS && proof_clause_id != 0){
        add_clause_proof_clause(clause, proof_clause_id);
        proof_clause_count++;
        tmp = read_clause_id(file, &proof_clause_id);
    }
    if (tmp == PARSE_ERROR){
        printf("Parse error in file %s:  ", filename);
        printf("File ended before add clause completed\n");
        return PARSE_ERROR;
    }
    else if (tmp == END_OF_FILE){
        return END_OF_FILE;
    }

    return SUCCESS;
}


//Read a binary proof line starting with d, r, or t, assuming the first character has been removed
result_code_t read_binary_other_line(FILE *file, char *filename, bool_t is_loose){
    clause_id_t id;
    result_code_t tmp = read_unsigned_clause_id(file, &id);
    int32_t lit;
    tmp = read_lit(file, &lit);
    while (tmp == SUCCESS && lit != 0){
        tmp = read_lit(file, &lit);
    }
    if (tmp == PARSE_ERROR){
        printf("Parse error in file %s:  ", filename);
        printf("File ended before other clause completed\n");
        return PARSE_ERROR;
    }
    else if (tmp == END_OF_FILE){
        return END_OF_FILE;
    }
    return SUCCESS;
}


//Read and parse a single line in a file
//Based on parsing in lrat-check from drat-trim
result_code_t parse_binary_line(FILE *file, char *filename, clause_t *clause,
                                bool_t read_lrat, bool_t is_loose){
    int32_t line_mode;
    result_code_t result = SUCCESS;

    bool_t add_clause_read = false;

    while (!add_clause_read && result == SUCCESS){
        //get first character to determine what kind of line it is
        line_mode = getc(file);
        switch (line_mode){
        case EOF:
            result = END_OF_FILE;
            break;
        case 'o':  //o for original clause
            if (read_lrat){
                if (is_loose){
                    result = END_OF_FILE;
                }
                else{
                    printf("Error:  Cannot have 'o' lines in LRAT file %s\n", filename);
                    result = PARSE_ERROR;
                }
            }
            else{
                result = read_binary_original_clause(file, filename, is_loose);
            }
            break;
        case 'a':  //a for add clause
            result = read_binary_add_line(file, filename, clause, read_lrat, is_loose);
            add_clause_read = true;
            break;
        case 'r':
            if (read_lrat){
                if (is_loose){
                    result = END_OF_FILE;
                }
                else{
                    printf("Error:  Cannot have 'r' lines in LRAT file %s\n", filename);
                    result = PARSE_ERROR;
                }
                break;
            }
        case 't':
            if (read_lrat){
                if (is_loose){
                    result = END_OF_FILE;
                }
                else{
                    printf("Error:  Cannot have 't' lines in LRAT file %s\n", filename);
                    result = PARSE_ERROR;
                }
                break;
            }
        case 'd': //move past other clauses we don't care about---handles d, r, and t
            result = read_binary_other_line(file, filename, is_loose);
            break;
        case 'f':  //f for final
            //nothing relevant to us can come after a finalize line
            result = END_OF_FILE;
            break;
        default:  //something else which shouldn't be here
            if (is_loose){
                result = END_OF_FILE;
            }
            else{
                printf("Parse error in file %s:  ", filename);
                printf("Cannot parse binary file line starting with %c (%" PRId32 ")\n", line_mode, line_mode);
                result = PARSE_ERROR;
            }
        }

        if (result != SUCCESS){
            return result;
        }
    }

    return SUCCESS;
}





/********************************************************************
 *                     INPUT PROOF FILE READING                     *
 ********************************************************************/

result_code_t read_input_file_line(int32_t file_index, combine_problem_t *p,
                                   bool_t binary, bool_t read_lrat, bool_t is_loose){
    file_t *f = get_file(p, file_index);
    clause_t *clause = get_file_clause(f);

    result_code_t result;

    if (binary){
        result = parse_binary_line(get_file_file(f), get_file_filename(f),
                                   clause, read_lrat, is_loose);
    }
    else{
        result = parse_line(get_file_file(f), get_file_filename(f),
                            clause, read_lrat, is_loose);
    }

    if (result == SUCCESS){
        //another clause was found
        set_file_has_another_clause(f, true);

        //check the clause was in the right file
        int32_t read_file_index = get_file_index(get_clause_id(clause), get_num_original_clauses(p),
                                                 get_num_files(p));
        if (read_file_index != file_index){
            printf("Error in file %s:  ", get_file_filename(f));
            printf("Read clause %" PRI_CLAUSE_ID " from file index %" PRId32 " ", get_clause_id(clause), file_index);
            printf("but it should have been in file index %" PRId32 "\n", read_file_index);
            return WRONG_FILE_CLAUSE;
        }
    }

    return result;
}





/********************************************************************
 *                         CNF FILE PARSING                         *
 ********************************************************************/

result_code_t parse_cnf_file(char *filename, int32_t *num_original_clauses_loc){
    int32_t tmp;
    char c = 0;

    //open the DIMACS file
    FILE *file = fopen(filename, "r");
    if (file == NULL){
        printf("Error:  Unable to open DIAMCS file %s\n", filename);
        return UNABLE_TO_OPEN;
    }

    //Remove things until we reach a line starting with p
    c = getc(file);
    while (c != EOF && c != 'p'){
        //comment case, so read rest of line
        if (c == 'c'){
            while (c != EOF && c != '\n'){
                c = getc(file);
            }
            if (c == EOF){
                printf("Parse error in file %s:  ", filename);
                printf("File ended before finding DIMACS header\n");
                return PARSE_ERROR;
            }
        }
        c = getc(file);
    }

    //Parse the header line
    int32_t vars;
    int32_t clauses;
    //assume we already read the p to get here
    tmp = fscanf(file, " cnf %" SCNd32 " %" SCNd32, &vars, &clauses);
    if (tmp != 2){
        printf("(%" PRId32 ") ", tmp);
        printf("Parse error in file %s:  ", filename);
        printf("Did not find DIMACS header at beginning of file\n");
        return PARSE_ERROR;
    }
    *num_original_clauses_loc = clauses;

    //close the file
    if (fclose(file)){
        printf("Error:  Unable to close file %s\n", filename);
    }

    return SUCCESS;
}





/********************************************************************
 *                           OUTPUT PROOF                           *
 ********************************************************************/

result_code_t output_delete_clauses(clause_id_t id, std::vector<clause_id_t>& clauses, FILE *outfile){
    //write the id and d
    fprintf(outfile, "%" PRI_CLAUSE_ID " d", id);
    //write the clauses to delete
    std::vector<clause_id_t>::iterator itr = clauses.begin();
    while(itr != clauses.end()){
        fprintf(outfile, " %" PRI_CLAUSE_ID, *itr);
        itr++;
    }
    //end the line
    fprintf(outfile, " 0\n");
    return SUCCESS;
}

