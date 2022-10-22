#include <stdio.h>
#include <boost/program_options.hpp>
#include <iostream>

#include "util.hpp"
#include "parse.hpp"


result_code_t dratify(FILE *outfile, FILE *infile,
                      std::map<clause_id_t,
                      std::vector<int32_t>>& clause_id_map, bool is_binary){
    clause_t clause;
    std::vector<clause_id_t> delete_clause(100);
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
            //write it to the file
            for (int32_t lit : clause.literals){
                fprintf(outfile, " %" PRId32, lit);
            }
            fprintf(outfile, " 0\n");
            //put the clause in the map
            clause_id_map[clause.clause_id] = clause.literals;
            //clean up for next time
            std::vector<int32_t> new_lits(100);
            new_lits.clear();
            clause.literals = new_lits;
        }
        else{
            //map over clause deletions, outputting one delete for each clause
            for (uint32_t i = 0; i < delete_clause.size(); i++){
                std::vector<int32_t> lits = clause_id_map[delete_clause[i]];
                fprintf(outfile, "d");
                for (int32_t lit : lits){
                    fprintf(outfile, " %" PRId32, lit);
                }
                fprintf(outfile, " 0\n");
                //remove it from the map because we can't need it anymore
                clause_id_map.erase(delete_clause[i]);
            }
            //clean up for next time
            delete_clause.clear();
        }
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


//Read the DIMACS file, building a map containing its clauses
std::map<clause_id_t, std::vector<int32_t>> read_dimacs_into_map(FILE *file, result_code_t *result){
    std::map<clause_id_t, std::vector<int32_t>> clause_id_map;
    clause_id_t next_id = 1;
    int32_t tmp;
    char c = 0;
    int32_t lit;
    *result = SUCCESS;

    //Remove things until we reach a line starting with p
    c = getc(file);
    while (c != EOF && c != 'p'){
        //comment case, so read rest of line
        if (c == 'c'){
            while (c != EOF && c != '\n'){
                c = getc(file);
            }
            if (c == EOF){
                printf("Parse Error:  File ended before finding DIMACS header\n");
                *result = PARSE_ERROR;
                return clause_id_map;
            }
        }
        c = getc(file);
    }

    //Remove the p line
    c = getc(file);
    while (c != EOF && c != '\n'){
        c = getc(file);
    }

    //read the clauses
    while (tmp != EOF){
        std::vector<int32_t> lits(0); //let it get set based on adding lits
        tmp = fscanf(file, " %" SCNd32, &lit);
        while (tmp != 0 && tmp != EOF && lit != 0){
            lits.push_back(lit);
            tmp = fscanf(file, " %" SCNd32, &lit);
        }
        if (tmp == 0){
            c = getc(file);
            if (c == 'c'){
                while (c != EOF && c != '\n'){
                    c = getc(file);
                }
            }
            else{
                printf("Parse error in DIMACS file\n");
                *result = PARSE_ERROR;
            }
        }
        else if (tmp != EOF){
            //add clause to map
            clause_id_map[next_id] = lits;
            next_id++;
        }
    }
    return clause_id_map;
}


void print_usage(char *name, boost::program_options::options_description desc){
    printf("Usage:  %s [options] PROBLEM.dimacs INPUT.lrat OUTPUT.drat\n", name);
    printf("\n");
    std::cout << desc << "\n";
}


int main(int argc, char **argv){
    //options to be displayed to the user in help
    boost::program_options::options_description visible_opts("Allowed options");
    visible_opts.add_options()
        ("help", "produce help message")
        ("binary", "read binary file");
    //options for holding positional arguments
    boost::program_options::options_description hidden_opts;
    hidden_opts.add_options()
        ("dimacs_name", boost::program_options::value<std::string>(),
         "DIMACS file for the problem")
        ("input_name", boost::program_options::value<std::string>(),
         "input LRAT filename")
        ("output_name", boost::program_options::value<std::string>(),
         "output DRAT filename");
    //all the options combined for parsing
    boost::program_options::options_description opts;
    opts.add(visible_opts).add(hidden_opts);

    //telling it the positional arguments
    boost::program_options::positional_options_description pos_opts;
    pos_opts.add("dimacs_name", 1);
    pos_opts.add("input_name", 1);
    pos_opts.add("output_name", 1);

    char *dimacsfilename;
    char *outfilename;
    char *infilename;
    bool is_binary = false;

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

    //open the DIMACS file
    FILE *dimacsfile = fopen(dimacsfilename, "r");
    if (dimacsfile == NULL){
        printf("Error:  Unable to open output file %s\n", dimacsfilename);
        return UNABLE_TO_OPEN;
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

    result_code_t result;
    auto clause_id_map = read_dimacs_into_map(dimacsfile, &result);
    if (result != SUCCESS){
        return result;
    }

    result = dratify(outfile, infile, clause_id_map, is_binary);
    return result;
}
