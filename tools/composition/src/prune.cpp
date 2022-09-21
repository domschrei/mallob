
#include <unordered_set>
#include <vector>

#include "util.hpp"
#include "parse.hpp"
#include "prune.hpp"


/********************************************************************
 *                      NEEDED CLAUSE STORAGE                       *
 ********************************************************************/

typedef std::unordered_set<clause_id_t> needed_clause_store_t;


//Check whether id is needed, removing it from the store if it is found
//Returns 1 if it was found, 0 if it was not found, and -1 if a higher
//   clause was expected
static inline
bool_t exists_and_remove(needed_clause_store_t *store, clause_id_t id,
                         int32_t num_original_clauses){
    bool_t exists = store->find(id) != store->end();
    if (exists){
        store->erase(id);
    }
    return exists;
}


//Add a clause dependency to the store
//Put true into already_contained if it was already in the store, false otherwise
//If id is an original clause, sets already_contained to true
static inline
result_code_t add_clause_dependency(needed_clause_store_t *store, clause_id_t id,
                                    bool_t *already_contained, int32_t num_original_clauses){
    //check for original clause
    if (id <= num_original_clauses){
        *already_contained = true;
        return SUCCESS;
    }

    if (store->find(id) != store->end()){
        *already_contained = true;
    }
    else{
        *already_contained = false;
        store->insert(id);
    }
    return SUCCESS;
}


//Check whether there is at least one clause still expected somewhere
//Returns a clause if one exists, otherwise 0
static inline clause_id_t store_nonempty(needed_clause_store_t *store){
    std::unordered_set<clause_id_t>::iterator itr = store->begin();
    if (itr != store->end()){
        return *itr; //if there is an element, return that
    }
    return 0; //no clause found
}





/********************************************************************
 *                             PRUNING                              *
 ********************************************************************/

//Read a non-empty clause, determine whether it is needed, and output it
//   along with appropriate deletes
//We could create clause and delete_clauses here since we don't need them
//   elsewhere, but that would be wasteful since we would need to do large
//   allocations each time
result_code_t handle_one_clause(FILE *infile, char *infilename, FILE *outfile,
                                int32_t num_original_clauses, clause_t *clause,
                                needed_clause_store_t *store,
                                std::vector<clause_id_t>& delete_clauses){
    bool_t already_contained;

    //set up and read a clause
    reset_clause(clause);
    result_code_t result = parse_line(infile, infilename, clause, true, false);
    if (result != SUCCESS){
        return result;
    }

    //check if we need this clause and take it out since we found it
    bool_t exists = exists_and_remove(store, clause->clause_id, num_original_clauses);
    if (exists){
        //store its dependencies
        std::vector<clause_id_t>::iterator itr;
        itr = clause->proof_clauses.begin();
        while (itr != clause->proof_clauses.end()){
            add_clause_dependency(store, *itr, &already_contained,
                                  num_original_clauses);
            if (!already_contained){
                //last use, so delete it
                delete_clauses.push_back(*itr);
            }
            itr++;
        }

        //write out deleted clauses, if there are any
        if (delete_clauses.size() > 0){
            output_delete_clauses(clause->clause_id, delete_clauses, outfile);
            delete_clauses.clear();
        }

        //write it out
        output_added_clause(clause, outfile, false);
    }

    return SUCCESS;
}


//Actually prune the file
result_code_t prune_proof_file(FILE *infile, char *infilename, FILE *outfile,
                               int32_t num_instances, int32_t num_original_clauses){
    result_code_t result;
    bool_t already_contained;
    std::vector<clause_id_t>::iterator itr;

    //initialze clause as empty to use for reading clauses in
    clause_t clause;
    reset_clause(&clause); //initialize to empty by "resetting"

    //create the storage
    needed_clause_store_t store;

    //hold clauses to output just one delete clause each time
    std::vector<clause_id_t> delete_clauses(5000);
    delete_clauses.clear(); //otherwise it has a bunch of zeroes the first time

    //read the empty clause
    result = parse_line(infile, infilename, &clause, true, false);
    if (result != SUCCESS){
        printf("Result code for reading first clause in pruning %s:  %d\n", infilename, result);
        return result;
    }
    //check it is the empty clause
    if (clause.literals.size() != 0){
        return EMPTY_CLAUSE_NOT_FOUND;
    }
    //store its dependencies
    itr = clause.proof_clauses.begin();
    while (itr != clause.proof_clauses.end()){
        add_clause_dependency(&store, *itr, &already_contained, num_original_clauses);
        itr++;
    }
    //write it out
    output_added_clause(&clause, outfile, false);

    //read the rest of the clauses
    while (result == SUCCESS){
        result = handle_one_clause(infile, infilename, outfile, num_original_clauses,
                                   &clause, &store, delete_clauses);
    }

    if (result == END_OF_FILE){
        //actually a success, just done
        result = SUCCESS;
    }
    //anything else can just be returned

    if (result == SUCCESS){
        //check if we found all the used clauses
        clause_id_t left = store_nonempty(&store);
        if (left != 0){
            printf("Error:  Not all used clauses found in the end (clause %" PRI_CLAUSE_ID ")\n", left);
            result = UNKNOWN_CLAUSE;
        }
    }

    return result;
}


result_code_t prune_proof(char *infilename, char *outfilename, int32_t num_instances,
                          int32_t num_original_clauses){
    FILE *infile = fopen(infilename, "r");
    if (infile == NULL){
        printf("Error:  Unable to open pruning input file %s\n", infilename);
        return UNABLE_TO_OPEN;
    }
    FILE *outfile = fopen(outfilename, "w");
    if (outfile == NULL){
        printf("Error:  Unable to open pruning output file %s\n", outfilename);
        if (fclose(infile) == EOF){
            printf("Error:  Unable to close file %s\n", infilename);
        }
        return UNABLE_TO_OPEN;
    }

    //actually prune
    result_code_t result = prune_proof_file(infile, infilename, outfile, num_instances, num_original_clauses);
    switch (result){
    case SUCCESS:
        //do nothing
        break;
    case FAILED_READ:
        //message should already be printed
        break;
    case EMPTY_CLAUSE_NOT_FOUND:
        printf("Error:  File %s did not end with the empty clause\n", infilename);
        break;
    case PARSE_ERROR:
        //message already printed
        break;
    case FAILED_ALLOCATION:
        printf("Error:  Unable to allocate enough memory in pruning proof\n");
        break;
    case UNKNOWN_CLAUSE:
        //message should be printed for specific clause already
        break;
    default:
        printf("Internal error:  Error code %" PRI_RESULT_CODE " should ", result);
        printf("not be returned from pruning file\n");
    }

    //close files
    if (fclose(infile) == EOF){
        printf("Error:  Unable to close file %s\n", infilename);
    }
    if (fclose(outfile) == EOF){
        printf("Error:  Unable to close file %s\n", outfilename);
    }

    return result;
}

