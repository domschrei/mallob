#include <stdio.h>
#include <boost/program_options.hpp>
#include <iostream>

#include "util.hpp"
#include "parse.hpp"


/*
 * We write our own parsing here instead of using the parsing files
 * directly because there we don't care about delete clauses, and here
 * we do.
 *
 * Parse a single line out of a text LRAT file, either an add line or a delete line
 * If it is an add line:  fill in the clause, wrote_add_clause is set to true
 * If it is a delete line:  fill in the vector, wrote_add_clause is set to false
 */
result_code_t parse_any_line(FILE *infile, clause_t *clause,
                             std::vector<clause_id_t>& delete_clause,
                             bool_t *wrote_add_clause){
    char c;
    clause_id_t id;
    clause_id_t secondary_id;
    int32_t lit;
    result_code_t result = SUCCESS;
    bool_t done = false;

    while (result == SUCCESS && !done){
        int32_t tmp = fscanf(infile, "%" SCN_CLAUSE_ID, &id);
        if (tmp == EOF){
            result = END_OF_FILE;
        }
        else if (tmp == 0){
            c = getc(infile);
            if (c == 'c'){ //move past a comment
                while (c != EOF && c != '\n'){
                    c = getc(infile);
                }
                if (c == EOF){
                    result = END_OF_FILE;
                }
            }
            else{
                printf("Error:  Cannot parse next entry in LRAT file ");
                printf("not starting with a clause ID\n");
                result = PARSE_ERROR;
            }
        }
        else{
            tmp = fscanf(infile, " d %" SCN_CLAUSE_ID, &secondary_id);
            if (tmp == EOF){
                printf("Error:  Incomplete clause %" PRI_CLAUSE_ID "at end of file\n", id);
                result = PARSE_ERROR;
            }
            else if (tmp == 1){ //delete
                *wrote_add_clause = false;
                tmp = 1;
                while (tmp != 0 && tmp != EOF && secondary_id != 0){
                    delete_clause.push_back(secondary_id);
                    tmp = fscanf(infile, " %" SCN_CLAUSE_ID, &secondary_id);
                }
                if (tmp == 0){
                    printf("Error:  Could not parse delete clause ");
                    printf("starting with ID %" PRI_CLAUSE_ID "\n", id);
                    result = PARSE_ERROR;
                }
                else if (tmp == EOF){
                    printf("Error:  File ended before delete clause starting ");
                    printf("with ID %" PRI_CLAUSE_ID " was complete\n", id);
                    result = PARSE_ERROR;
                }
                else{
                    result = SUCCESS;
                    done = true;
                }
            }
            else{ //add line
                *wrote_add_clause = true;
                set_clause_id(clause, id);
                //read literals
                tmp = fscanf(infile, " %" SCNd32, &lit);
                while (tmp != 0 && tmp != EOF && lit != 0){
                    add_clause_literal(clause, lit);
                    tmp = fscanf(infile, " %" SCNd32, &lit);
                }
                if (tmp == 0){
                    printf("Error:  Could not parse add clause with ");
                    printf("ID %" PRI_CLAUSE_ID "\n", id);
                    result = PARSE_ERROR;
                }
                else if (tmp == EOF){
                    printf("Error:  File ended before add clause with ");
                    printf("ID %" PRI_CLAUSE_ID " was complete\n", id);
                    result = PARSE_ERROR;
                }
                else{
                    //read proof hint
                    tmp = fscanf(infile, " %" SCN_CLAUSE_ID, &secondary_id);
                    while (tmp != 0 && tmp != EOF && secondary_id != 0){
                        add_clause_proof_clause(clause, secondary_id);
                        tmp = fscanf(infile, " %" SCN_CLAUSE_ID, &secondary_id);
                    }
                    if (tmp == 0){
                        printf("Error:  Could not parse add clause with ");
                        printf("ID %" PRI_CLAUSE_ID "\n", id);
                        result = PARSE_ERROR;
                    }
                    else if (tmp == EOF){
                        printf("Error:  File ended before add clause with ");
                        printf("ID %" PRI_CLAUSE_ID " was complete\n", id);
                        result = PARSE_ERROR;
                    }
                    else{
                        result = SUCCESS;
                        done = true;
                    }
                }
            }
        }
    }

    return result;
}


result_code_t run_renumbering(FILE *outfile, FILE *infile, int32_t num_original_clauses){
    clause_id_t next_id = num_original_clauses + 1;
    std::map<clause_id_t, clause_id_t> clause_id_map;
    clause_t clause;
    std::vector<clause_id_t> delete_clause(1000);
    bool_t is_add_clause;

    //initialize clause by resetting it
    reset_clause(&clause);
    //initializing the vector size is filling it with 0
    delete_clause.clear();

    result_code_t result = parse_any_line(infile, &clause, delete_clause, &is_add_clause);
    while (result == SUCCESS){
        if (is_add_clause){
            //map new clause ID
            clause_id_map[get_clause_id(&clause)] = next_id;
            set_clause_id(&clause, next_id);
            next_id++;
            //map over proof hint
            for (uint32_t i = 0; i < clause.proof_clauses.size(); i++){
                if (clause.proof_clauses[i] > num_original_clauses){
                    clause.proof_clauses[i] = clause_id_map[clause.proof_clauses[i]];
                }
            }
            //write it out
            output_added_clause(&clause, outfile, false);
            //clean up for next time
            reset_clause(&clause);
        }
        else{
            //map over clause deletions
            for (uint32_t i = 0; i < delete_clause.size(); i++){
                delete_clause[i] = clause_id_map[delete_clause[i]];
            }
            //write it out, with last used clause ID to start line
            clause_id_t last_output = next_id - 1;
            output_delete_clauses(last_output, delete_clause, outfile);
            //clean up for next time
            delete_clause.clear();
        }
        //read the next line
        result = parse_any_line(infile, &clause, delete_clause, &is_add_clause);
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
        ("help", "produce help message");
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

    result = run_renumbering(outfile, infile, num_original_clauses);
    return result;
}
