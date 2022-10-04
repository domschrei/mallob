
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <boost/program_options.hpp>
#include <exception>
#include <iostream>

#include "parse.hpp"
#include "join.hpp"
#include "reverse.hpp"
#include "prune.hpp"


void print_usage(char *name, boost::program_options::options_description desc){
    printf("Usage:  %s [options] PROBLEM.dimacs OUTPUT.lrat INPUT.frat...\n", name);
    printf("\n");
    std::cout << desc << "\n";
}


//Parse the DIMACS file and put the number of original clauses in num_original_clauses
//Also display messages about success/failure
result_code_t main_parse_cnf_file(char *dimacsfilename, int32_t *num_original_clauses){
    result_code_t result = parse_cnf_file(dimacsfilename, num_original_clauses);
    if (result != SUCCESS){
        //all specific messages should be printed in the other function
        printf("Unable to parse DIMACS file.  Quitting\n");
    }
    else{
        printf("Successfully read DIMACS file (%" PRId32 " original clauses)\n", *num_original_clauses);
    }
    return result;
}


//Combine the proof files into combined_file
//Also display messages about success/failure and time
result_code_t main_combine_input_files(char *combined_file, std::vector<std::string> infilenames,
                                       bool_t is_binary, int32_t num_original_clauses,
                                       bool_t read_lrat, bool_t is_loose){
    printf("Combining proof files...\n");
    clock_t begin = clock();
    result_code_t result = combine_input_files(combined_file, infilenames, is_binary,
                                               num_original_clauses, read_lrat, is_loose);
    clock_t end = clock();
    double time_spent = (double) (end - begin) / CLOCKS_PER_SEC;
    if (result != SUCCESS){
        //all specific messages should be printed in the other function
        printf("Unable to combine input files.  Quitting\n");
    }
    else{
        printf("Successfully combined input files (%f seconds)\n", time_spent);
    }
    return result;
}


//Reverse startfile into revfile, displaying the type being rev_type
//Also display messages about success/failure and time
result_code_t main_reverse_file(char* rev_type, char *startfile, char *revfile,
                                bool_t remove_when_done, bool_t is_binary){
    printf("Reversing %s file...\n", rev_type);
    clock_t begin = clock();
    result_code_t result = reverse_file(startfile, revfile, is_binary);
    clock_t end = clock();
    double time_spent = (double) (end - begin) / CLOCKS_PER_SEC;
    if (result != SUCCESS){
        //all specific messages should be printed in the other function
        printf("Unable to reverse file.  Quitting\n");
    }
    else{
        printf("Successfully reversed file (%f seconds)\n", time_spent);
        if (remove_when_done){
            remove(startfile);
        }
    }
    return result;
}


//Turn rev_combined into unpruned_file
//Also display messages about success/failure and time
result_code_t main_create_unpruned_proof(char *rev_combined, char *unpruned_file,
                                         int32_t num_infiles, int32_t num_original_clauses,
                                         bool_t is_binary){
    printf("Building unpruned reversed combined file...\n");
    clock_t begin = clock();
    result_code_t result = prune_proof(rev_combined, unpruned_file, num_infiles,
                                       num_original_clauses, is_binary, true);
    clock_t end = clock();
    double time_spent = (double) (end - begin) / CLOCKS_PER_SEC;
    if (result != SUCCESS){
        //all specific messages should be printed in the other function
        printf("Unable to create unpruned file.  Quitting\n");
    }
    else{
        printf("Successfully wrote unpruned proof file (%f seconds)\n", time_spent);
    }
    return result;
}


//Prune rev_combined into pruned_file
//Also display messages about success/failure and time
result_code_t main_prune_proof(char *rev_combined, char *pruned_file, int32_t num_infiles,
                               int32_t num_original_clauses, bool_t remove_when_done,
                               bool_t is_binary){
    printf("Pruning reversed combined file...\n");
    clock_t begin = clock();
    result_code_t result = prune_proof(rev_combined, pruned_file, num_infiles,
                                       num_original_clauses, is_binary, false);
    clock_t end = clock();
    double time_spent = (double) (end - begin) / CLOCKS_PER_SEC;
    if (result != SUCCESS){
        //all specific messages should be printed in the other function
        printf("Unable to prune file.  Quitting\n");
    }
    else{
        printf("Successfully pruned proof file (%f seconds)\n", time_spent);
        if (remove_when_done){
            remove(rev_combined);
        }
    }
    return result;
}


int main(int argc, char **argv){
    //options to be displayed to the user in help
    boost::program_options::options_description visible_opts("Allowed options");
    visible_opts.add_options()
        ("help", "produce help message")
        ("binary", "read input in binary format")
        ("frat", "read input in FRAT format (defaults to reading LRAT format)")
        ("write-unpruned", boost::program_options::value<std::string>(),
         "write the given file with the combined file with delete lines inserted")
        ("loose", "treat parse errors as the end of the file")
        ("keep-temps", "don't remove temporary files when done with them");
    //options for holding positional arguments
    boost::program_options::options_description hidden_opts;
    hidden_opts.add_options()
        ("dimacs_name", boost::program_options::value<std::string>(),
         "DIMACS file for the problem")
        ("output_name", boost::program_options::value<std::string>(),
         "output LRAT filename")
        ("input_files", boost::program_options::value<std::vector<std::string> >(),
         "input proof filenames");
    //all the options combined for parsing
    boost::program_options::options_description opts;
    opts.add(visible_opts).add(hidden_opts);

    //telling it the positional arguments
    boost::program_options::positional_options_description pos_opts;
    pos_opts.add("dimacs_name", 1);
    pos_opts.add("output_name", 1);
    pos_opts.add("input_files", -1);

    //values to be determined by commandline arguments
    bool is_binary = false;
    bool read_lrat = true;
    bool is_loose = false;
    bool remove_when_done = true;
    std::string dimacsfilename;
    std::string outfilename;
    std::vector<std::string> infilenames;
    bool write_unpruned = false;
    std::string unprunedfilename;

    //variables resulting from parsing
    boost::program_options::variables_map vm;

    //whether the arguments parsed fine
    bool good_args = true;

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

        if (vm.count("loose")){
            is_loose = true;
        }

        if (vm.count("frat")){
            read_lrat = false;
        }

        if (vm.count("keep-temps")){
            remove_when_done = false;
        }

        if (vm.count("write-unpruned")){
            write_unpruned = true;
            unprunedfilename = vm["write-unpruned"].as<std::string>();
        }

        if (vm.count("dimacs_name")){
            dimacsfilename = vm["dimacs_name"].as<std::string>();
        }
        else{
            good_args = false;
        }

        if (vm.count("output_name")){
            outfilename = vm["output_name"].as<std::string>();
        }
        else{
            good_args = false;
        }

        if (vm.count("input_files")){
            infilenames = vm["input_files"].as<std::vector<std::string> >();
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

    // make this fancier with ifdefs if you want to use Windows.
    const char separator = '/';

    //names of temporary files.  
    // MWW 8/28/2022: Put them in the same directory as 
    // the output file to make it easier to find them.
    std::string output_path;
    auto found = outfilename.find_last_of(separator);
    if (found == std::string::npos) {
        output_path = "";
    } else {
        output_path = outfilename.substr(0, found+1);
    }
    std::string combined_file = output_path + "._____combined_proof_file.frat";
    std::string rev_combined = output_path + "._____rev_combined_file.frat";
    std::string pruned_file = output_path + "._____pruned_file.lrat";
    std::string unpruned_file = output_path + "._____unpruned_file.lrat";

    result_code_t result;
    int32_t num_original_clauses = 0;


    //read the DIMACS file
    result = main_parse_cnf_file((char *)dimacsfilename.c_str(), &num_original_clauses);
    if (result != SUCCESS){
        return result;
    }

    //combine the separate proof files
    result = main_combine_input_files((char *)combined_file.c_str(), infilenames, is_binary,
                                      num_original_clauses, read_lrat, is_loose);
    if (result != SUCCESS){
        return result;
    }

    //reverse the combined file
    result = main_reverse_file((char *) "combined", (char *)combined_file.c_str(), 
                               (char *)rev_combined.c_str(),
                               remove_when_done, is_binary);
    if (result != SUCCESS){
        return result;
    }

    //check whether we need an unpruned file
    if (write_unpruned){
        result = main_create_unpruned_proof((char *)rev_combined.c_str(),
                                            (char *)unpruned_file.c_str(),
                                            infilenames.size(), num_original_clauses,
                                            is_binary);;
        if (result != SUCCESS){
            return result;
        }
        //reverse the unpruned file
        if (is_binary){
            result = byte_reverse_file((char *)unpruned_file.c_str(),
                                       (char *)unprunedfilename.c_str());
            if (remove_when_done){
                remove((char *)unpruned_file.c_str());
            }
        }
        else{
            result = main_reverse_file((char *) "unpruned", (char *)unpruned_file.c_str(),
                                       (char *)unprunedfilename.c_str(),
                                       remove_when_done, is_binary);
        }
    }

    //prune the reversed combined file
    result = main_prune_proof((char *)rev_combined.c_str(), (char *)pruned_file.c_str(),
                              infilenames.size(), num_original_clauses, remove_when_done,
                              is_binary);
    if (result != SUCCESS){
        return result;
    }

    //reverse the pruned file
    if (is_binary){
        result = byte_reverse_file((char *)pruned_file.c_str(),
                                   (char *)outfilename.c_str());
        if (remove_when_done){
            remove((char *)pruned_file.c_str());
        }
    }
    else{
        result = main_reverse_file((char *) "pruned", (char *)pruned_file.c_str(),
                                   (char *)outfilename.c_str(),
                                   remove_when_done, is_binary);
    }

    if (result == SUCCESS){
        printf("Exiting\n");
    }
    return result;
}

