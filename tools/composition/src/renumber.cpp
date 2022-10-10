#include <stdio.h>
#include <boost/program_options.hpp>
#include <iostream>

#include "util.hpp"
#include "parse.hpp"

#include "unordered_dense.h"

typedef ankerl::unordered_dense::map<clause_id_t, clause_id_t> ClauseIdMap;

/*
 * Build the initial map from the adjustment file
 * Also set the number of original clauses correctly based on the adjustment
 */
result_code_t read_adjustment_file(char *filename,
                                   ClauseIdMap& clause_id_map,
                                   int32_t *num_original_clauses){
    FILE *f = fopen(filename, "r");
    if (f == NULL){
        printf("Error:  Unable to open adjustment file %s\n", filename);
        return UNABLE_TO_OPEN;
    }
    int32_t max_id = *num_original_clauses;
    clause_id_t first;
    clause_id_t second;
    int32_t tmp = fscanf(f, "%" SCN_CLAUSE_ID " %" SCN_CLAUSE_ID, &first, &second);
    while (tmp == 2){
        max_id = second > max_id ? second : max_id;
        clause_id_map[first] = second;
        tmp = fscanf(f, "%" SCN_CLAUSE_ID " %" SCN_CLAUSE_ID, &first, &second);
    }
    if (tmp == EOF){
        *num_original_clauses = max_id;
        return SUCCESS;
    }
    else{
        printf("Error:  Could not read all entries in adjustment file %s\n", filename);
        return PARSE_ERROR;
    }
}


result_code_t run_renumbering(FILE *outfile, FILE *infile,
                              ClauseIdMap& clause_id_map,
                              int32_t num_original_clauses, bool is_binary){
    clause_id_t next_id = num_original_clauses + 1;
    clause_t clause;
    std::vector<clause_id_t> delete_clause(1000);
    bool_t is_add_clause;

    //initialize clause by resetting it
    reset_clause(&clause);
    //initializing the vector size is filling it with 0
    delete_clause.clear();

    result_code_t result;
    if (is_binary){
        result = parse_any_binary_line(infile, &clause, delete_clause, &is_add_clause);
    }
    else{
        result = parse_any_line(infile, &clause, delete_clause, &is_add_clause);
    }
    while (result == SUCCESS){
        if (is_add_clause){
            //map new clause ID
            clause_id_map[get_clause_id(&clause)] = next_id;
            set_clause_id(&clause, next_id);
            next_id++;
            //map over proof hint
            for (uint32_t i = 0; i < clause.proof_clauses.size(); i++) {
                auto it = clause_id_map.find(clause.proof_clauses[i]);
                if (it != clause_id_map.end()){ //map if an entry exists
                    clause.proof_clauses[i] = it->second;
                }
            }
            //write it out
            output_added_clause(&clause, outfile, false, is_binary);
            //clean up for next time
            reset_clause(&clause);
        }
        else{
            //map over clause deletions
            for (uint32_t i = 0; i < delete_clause.size(); i++){
                clause_id_t new_val = clause_id_map[delete_clause[i]];
                clause_id_map.erase(delete_clause[i]);
                delete_clause[i] = new_val;
            }
            //write it out, with last used clause ID to start line
            clause_id_t last_output = next_id - 1;
            output_delete_clauses(last_output, delete_clause, outfile, is_binary);
            //clean up for next time
            delete_clause.clear();
        }
        //read the next line
        if (is_binary){
            result = parse_any_binary_line(infile, &clause, delete_clause, &is_add_clause);
        }
        else{
            result = parse_any_line(infile, &clause, delete_clause, &is_add_clause);
        }
    }
    //getting through the file is a success
    if (result == END_OF_FILE){
        result = SUCCESS;
    }
    return result;
}


void print_usage(char *name, boost::program_options::options_description desc){
    printf("Usage:  %s [options] PROBLEM.dimacs OUTPUT.lrat INPUT.lrat\n", name);
    printf("\n");
    std::cout << desc << "\n";
}


int main(int argc, char **argv){
    //options to be displayed to the user in help
    boost::program_options::options_description visible_opts("Allowed options");
    visible_opts.add_options()
        ("help", "produce help message")
        ("binary", "read and write binary files")
        ("adjust-file", boost::program_options::value<std::string>(),
         "file for adjusting clause ID's based on preprocessing");
    //options for holding positional arguments
    boost::program_options::options_description hidden_opts;
    hidden_opts.add_options()
        ("dimacs_name", boost::program_options::value<std::string>(),
         "DIMACS file for the problem")
        ("output_name", boost::program_options::value<std::string>(),
         "output LRAT filename")
        ("input_name", boost::program_options::value<std::string>(),
         "input LRAT filename");
    //all the options combined for parsing
    boost::program_options::options_description opts;
    opts.add(visible_opts).add(hidden_opts);

    //telling it the positional arguments
    boost::program_options::positional_options_description pos_opts;
    pos_opts.add("dimacs_name", 1);
    pos_opts.add("output_name", 1);
    pos_opts.add("input_name", 1);

    char *dimacsfilename;
    char *outfilename;
    char *infilename;
    bool is_binary = false;
    bool adjust = false;
    char *adjustfilename;
    int32_t num_original_clauses;

    //variables resulting from parsing
    boost::program_options::variables_map vm;

    //whether the arguments parsed fine
    bool_t good_args = true;

    try{
        //parse the arguments
        boost::program_options::store(
               boost::program_options::command_line_parser(argc, argv).
               options(opts).
               positional(pos_opts).
               run(), vm);
        boost::program_options::notify(vm);

        if (vm.count("help")){
            print_usage(argv[0], visible_opts);
            return SUCCESS;
        }

        if (vm.count("binary")){
            is_binary = true;
        }

        if (vm.count("adjust-file")){
            adjust = true;
            adjustfilename = (char *) vm["adjust-file"].as<std::string>().c_str();
        }

        if (vm.count("dimacs_name")){
            dimacsfilename = (char *) vm["dimacs_name"].as<std::string>().c_str();
        }
        else{
            good_args = false;
        }

        if (vm.count("output_name")){
            outfilename = (char *) vm["output_name"].as<std::string>().c_str();
        }
        else{
            good_args = false;
        }

        if (vm.count("input_name")){
            infilename = (char *)vm["input_name"].as<std::string>().c_str();
        }
        else{
            good_args = false;
        }
    }
    catch (std::exception &e){
        std::cout << "Error:  " << e.what() << "\n";
        good_args = false;
    }
    catch (...){
        good_args = false;
    }

    if (!good_args){
        print_usage(argv[0], visible_opts);
        return BAD_USAGE;
    }

    //get number of original clauses from DIMACS file
    result_code_t result = parse_cnf_file(dimacsfilename, &num_original_clauses);
    if (result != SUCCESS){
        return result;
    }

    ClauseIdMap clause_id_map;

    //if there is an adjustment file, read it
    if (adjust){
        result = read_adjustment_file(adjustfilename, clause_id_map,
                                      &num_original_clauses);
        if (result != SUCCESS){
            return result;
        }
    }

    //open the output file
    FILE *outfile = fopen(outfilename, "w");
    if (outfile == NULL){
        printf("Error:  Unable to open output file %s\n", outfilename);
        return UNABLE_TO_OPEN;
    }

    //open the input file
    FILE *infile = fopen(infilename, "r");
    if (infile == NULL){
        printf("Error:  Unable to open input file %s\n", infilename);
        return UNABLE_TO_OPEN;
    }

    result = run_renumbering(outfile, infile, clause_id_map,
                             num_original_clauses, is_binary);
    return result;
}
