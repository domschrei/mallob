
#include "util.hpp"

/********************************************************************
 *                              CLAUSE                              *
 ********************************************************************/

void print_clause(clause_t *clause){
    //print ID
    printf("a %" PRI_CLAUSE_ID "  ", clause->clause_id);
    //print literals
    std::vector<int32_t>::iterator lititr = clause->literals.begin();
    while (lititr != clause->literals.end()){
        printf(" %" PRId32, *lititr);
        lititr++;
    }
    printf("   0  ");
    //print proof clauses
    std::vector<clause_id_t>::iterator pcitr = clause->proof_clauses.begin();
    while (pcitr != clause->proof_clauses.end()){
        printf(" %" PRI_CLAUSE_ID, *pcitr);
        pcitr++;
    }
    printf("   0\n");
}

//These are from CaDiCaL with FRAT/LRAT support
//github.com/RandomActsOfGrammar/cadical:src/tracer.cpp
inline void put_binary_zero (FILE *file) {
    putc(0, file);
}
inline void put_binary_unsigned (int64_t n, FILE *file) {
    unsigned char ch;
    while (n & ~0x7f) {
        ch = (n & 0x7f) | 0x80;
        fputc(ch, file);
        n >>= 7;
    }
    ch = n;
    fputc(ch, file);
}
inline void put_binary_signed (int64_t n, FILE *file) {
    put_binary_unsigned(2*abs (n) + (n < 0), file);
}
inline void put_binary_lit (int lit, FILE *file) {
    put_binary_signed(lit, file);
}

result_code_t output_added_clause(clause_t *clause, FILE *outfile,
                                  bool_t frat_format, bool_t is_binary){
    if (is_binary){
        fputc('a', outfile);
    }
    else if (frat_format){
        fprintf(outfile, "a ");
    }
    //write the clause ID
    if (is_binary){
        if (frat_format){
            put_binary_unsigned(clause->clause_id, outfile);
        }
        else{
            put_binary_signed(clause->clause_id, outfile);
        }
    }
    else{
        fprintf(outfile, "%" PRI_CLAUSE_ID " ", clause->clause_id);
    }
    //write the literals
    for (const auto & external_lit : clause->literals){
        if (is_binary){
            put_binary_lit(external_lit, outfile);
        }
        else{
            fprintf(outfile, "%" PRId32 " ", external_lit);
        }
    }
    if (is_binary){
        put_binary_zero(outfile);
    }
    else{
        fprintf(outfile, "0 ");
    }
    //write the proof
    if (frat_format){
        if (is_binary){
            fputc('l', outfile);
        }
        else{
            fprintf(outfile, "l ");
        }
    }
    for (const auto & c : clause->proof_clauses){
        if (is_binary){
            put_binary_signed (c, outfile);
        }
        else{
            fprintf(outfile, "%" PRI_CLAUSE_ID " ", c);
        }
    }
    if (is_binary){
        put_binary_zero(outfile);
    }
    else{
        fprintf(outfile, "0\n");
    }

    return SUCCESS;
}


inline void put_binary_unsigned_backward(int64_t n, FILE *file){
    unsigned char bytes[74]; //max 74 bytes to fit n
    int32_t next_open = 0;

    //write the bytes into the buffer in order
    unsigned char ch;
    while (n & ~0x7f) {
        ch = (n & 0x7f) | 0x80;
        bytes[next_open] = ch;
        next_open++;
        n >>= 7;
    }
    ch = n;
    bytes[next_open] = ch;
    next_open++;

    //write the bytes into the file in reverse
    while (next_open > 0){
        next_open--;
        fputc(bytes[next_open], file);
    }
}
inline void put_binary_signed_backward(int64_t n, FILE *file){
    put_binary_unsigned_backward(2*abs (n) + (n < 0), file);
}


result_code_t output_lrat_binary_backward(clause_t *clause, FILE *outfile){
    //final 0
    put_binary_zero(outfile);

    //proof hints backward
    auto prf_itr = clause->proof_clauses.end();
    while (prf_itr != clause->proof_clauses.begin()){
        prf_itr--;
        put_binary_signed_backward(*prf_itr, outfile);
    }

    //0 after literals
    put_binary_zero(outfile);

    //literals backward
    auto lit_itr = clause->literals.end();
    while (lit_itr != clause->literals.begin()){
        lit_itr--;
        put_binary_signed_backward(*lit_itr, outfile);
    }

    //clause ID
    put_binary_signed_backward(clause->clause_id, outfile);

    //a
    fputc('a', outfile);

    return SUCCESS;
}

result_code_t output_delete_clauses(clause_id_t id, std::vector<clause_id_t>& clauses,
                                    FILE *outfile, bool is_binary){
    if (is_binary){
        fputc('d', outfile);
        for (auto i : clauses){
            put_binary_signed(i, outfile);
        }
        put_binary_zero(outfile);
    }
    else{
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
    }
    return SUCCESS;
}


result_code_t output_backward_delete_clauses(clause_id_t id,
                                             std::vector<clause_id_t>& clauses,
                                             FILE *outfile){
    //final 0
    put_binary_zero(outfile);

    //clauses to delete backward
    auto itr = clauses.end();
    while (itr != clauses.begin()){
        itr--;
        put_binary_signed_backward(*itr, outfile);
    }

    //d
    fputc('d', outfile);

    return SUCCESS;
}




/********************************************************************
 *                               FILE                               *
 ********************************************************************/

void remove_output_clause(file_t *f){
    //protect against double calls to this, which would set last_clause_output to 0
    if (f->last_clause_output < f->clause.clause_id){
        f->last_clause_output = f->clause.clause_id;
    }
    reset_clause(&(f->clause));
    f->has_another_clause = false; //assume this until proven otherwise
}




/********************************************************************
 *                         COMBINE PROBLEM                          *
 ********************************************************************/

struct combine_problem_t {
    int32_t num_files;
    //array of pointers to files for space reasons:  when we have a large number of files,
    //   we don't want ot run out of space in a page to hold them
    file_t **files;
    //known number of original clauses
    int32_t num_original_clauses;
};


result_code_t initialize_problem(std::vector<std::string> filenames,
                                 combine_problem_t **p_loc){
    int32_t num_files = filenames.size();

    //allocate space for the overall structure
    combine_problem_t *p = (combine_problem_t *) malloc(sizeof(combine_problem_t));
    if (p == NULL){
        return FAILED_ALLOCATION; //allocation error
    }
    *p_loc = p;
    p->num_files = num_files;
    p->num_original_clauses = 0;

    //allocate space for the files
    file_t **file_arr = (file_t **) malloc(num_files * sizeof(file_t *));
    if (file_arr == NULL){
        free(p);
        return FAILED_ALLOCATION; //allocation error
    }
    p->files = file_arr;

    //open each file and initialize its clause to be unread
    for (int32_t i = 0; i < num_files; i++){
        file_arr[i] = new file_t();

        //Enter filename by copying so it is always there
        size_t len = filenames[i].length();
        file_arr[i]->filename = (char *) malloc(sizeof(char) * (len + 1));
        if (file_arr[i]->filename == NULL){
            free_problem(p);
            return FAILED_ALLOCATION;
        }
        memmove(file_arr[i]->filename, filenames[i].c_str(), len);
        file_arr[i]->filename[len] = 0; //null terminated

        //Open the file
        FILE *f = fopen(filenames[i].c_str(), "r");
        if (f == NULL){
            free_problem(p);
            printf("Error:  Unable to open file %s\n", filenames[i].c_str());
            return UNABLE_TO_OPEN;
        }
        file_arr[i]->file = f;

        //initialze clause as empty
        reset_clause(&file_arr[i]->clause);

        //no clause output yet from this file
        file_arr[i]->last_clause_output = 0;
        //don't know we have another clause until we read it
        file_arr[i]->has_another_clause = false;
    }

    return SUCCESS;
}


int32_t get_num_files(combine_problem_t *p){
    return p->num_files;
}


int32_t get_num_original_clauses(combine_problem_t *p){
    return p->num_original_clauses;
}


file_t *get_file(combine_problem_t *p, int32_t i){
    return p->files[i];
}


void add_original_clause_count(combine_problem_t *p, int64_t num){
    p->num_original_clauses = num;
}


bool_t valid_to_output(clause_t *clause, combine_problem_t *p){
    //go through all proof clauses
    std::vector<clause_id_t>::iterator itr = clause->proof_clauses.begin();
    while (itr != clause->proof_clauses.end()){
        //check if this clause has already been output
        clause_id_t dependency = *itr;
        if (dependency > p->num_original_clauses){
            //not original, so we need to check if it was output from its file
            int32_t file_index = get_file_index(dependency, p->num_original_clauses, p->num_files);
            clause_id_t file_last_out = p->files[file_index]->last_clause_output;
            if (dependency > file_last_out){
                //this dependency has not been output
                return false;
            }
        }
        //else original clause, so valid
        itr++;
    }
    //all dependencies have been output if we make it here
    return true;
}


void free_problem(combine_problem_t *p){
    //go through all the files
    for (int32_t i = 0; i < p->num_files; i++){
        if (p->files[i] != NULL){
            //close the file
            if (p->files[i]->file != NULL && fclose(p->files[i]->file) == EOF){
                printf("Error closing file %s\n", p->files[i]->filename);
            }
            //free filename
            free(p->files[i]->filename);
            //free the file itself
            free(p->files[i]);
        }
    }

    //free the files array itself
    free(p->files);

    //free the structure itself
    free(p);
}

