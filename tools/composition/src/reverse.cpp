
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <fstream>

#include "reverse.hpp"


/********************************************************************
 *                      TEXT REVERSE WITH TAC                       *
 ********************************************************************/

result_code_t text_reverse_file(char *filename, char *revfilename){
    //open file to place reversed lines
    int fd = open(revfilename, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
    result_code_t result = SUCCESS;
    if (fd < 0){
        printf("Error:  Unable to open file %s for writing", revfilename);
        printf(" (Error number %d)\n", errno);
        return UNABLE_TO_OPEN;
    }

    pid_t pid = fork();
    if (pid < 0){
        //error
        printf("Error:  Could not reverse file %s\n", filename);
        return FORK_ERROR;
    }
    else if (pid == 0){
        //change STDOUT to the reversed file
        result = dup2(fd, STDOUT_FILENO);
        if (result < 0){
            printf("Error:  Unable to replace stdout with file %s ", revfilename);
            printf(" for reversing (Error number %d)\n", errno);
            if (close(fd) < 0){
                printf("Error:  Unable to close file %s\n", revfilename);
            }
            return FORK_ERROR;
        }
        //reverse the file:   https://www.baeldung.com/linux/reverse-order-of-file-lines
        result = execlp("tac", "tac", filename, NULL);
        printf("Error:  Unable to execute reverse process (Error number %d:  %s)\n", errno, strerror(errno));
    }
    else{
        int32_t status;
        result = SUCCESS;
        waitpid(pid, &status, 0);
        //check return status
        if (WIFEXITED(status)){
            //terminated normally
            result = WEXITSTATUS(status);
            if (result != 0){
                printf("Error:  Reversing file returned non-zero result %" PRI_RESULT_CODE "\n", result);
                result = FORK_ERROR;
            }
        }
        else{
            //some error
            printf("Error:  Reversing file subprocess did not exit normally\n");
            result = FORK_ERROR;
        }
        if (close(fd) < 0){
            printf("Error:  Unable to close file %s\n", revfilename);
            result = (result == SUCCESS) ? FILE_CLOSE_ERROR : result;
        }
        return result;
    }
    return result; //to suppress a warning
}





/********************************************************************
 *                          BINARY REVERSE                          *
 ********************************************************************/

//This is based on the reverse reading in Mallob, with the basic class
//taken from there directly, then modified to be LRAT-specific
//github.com/domschrei/mallob/blob/d41ec2851417aca4d75183ecbd896b4da4403beb/src/util/reverse_file_reader.hpp

#define REVERSE_READER_BUF_SIZE 65536

class ReverseReader {

private:
    std::ifstream _stream;
    size_t _file_size;

    char _buffer[REVERSE_READER_BUF_SIZE];
    int _buffer_pos = -1;

public:
    ReverseReader(const std::string& filename) : 
            _stream(filename, std::ios_base::ate | std::ios_base::binary) {
        _file_size = _stream.tellg();
    }

    bool nextAsByte(char& c) {
        if (_buffer_pos < 0) refillBuffer();
        if (_buffer_pos < 0) return false;
        c = _buffer[_buffer_pos];
        _buffer_pos--;
        return true;
    }

private:
    void refillBuffer() {
        if (!_stream.good()) return;
        
        // Check by how far you can go back
        auto sizeBefore = _file_size;
        _file_size = std::max(0LL, ((long long) sizeBefore) - REVERSE_READER_BUF_SIZE);
        int numDesired = sizeBefore - _file_size;

        // Go back and read the corresponding chunk of data
        _stream.seekg(_file_size, std::ios_base::beg);
        _stream.read(_buffer, numDesired);

        // Check how much has been read
        if (_stream.eof()) _buffer_pos = _stream.gcount()-1;
        else _buffer_pos = numDesired-1;
    }
};



class ReverseBinaryLratReader {

private:
    ReverseReader rev;
    //store bytes for intermediate processing
    std::vector<char> vec;
    //whether we have read some line yet
    bool readAlready;

    const char lower7 = 0b01111111;
    const char upper1 = 0b10000000;

public:
    ReverseBinaryLratReader(const std::string& filename) :
        rev(filename), vec(100), readAlready(false) { }

    //Put the next clause in the file into clause
    result_code_t readRevClause(clause_t *clause){
        bool readFine;
        clause_id_t cid;
        result_code_t res;
        clause->clause_id = 0;

        if (!readAlready){
            //Remove the first (last) 0
            char zeroByte;
            rev.nextAsByte(zeroByte);
            if (zeroByte != 0){
                printf("Parse error:  Binary LRAT file did not end with 0\n");
                return PARSE_ERROR;
            }
            readAlready = true;
            vec.clear();
        }

        //read proof chain
        readFine = readAllNonzeroBytes();
        if (!readFine){
            if (vec.empty()){
                return END_OF_FILE;
            }
            printf("Parse error:  Reversed binary LRAT file ended");
            printf(" in middle of line\n");
            return PARSE_ERROR;
        }
        //parse bytes into proof
        res = parseNumFromVec(&cid);
        while (res == SUCCESS && !vec.empty()){
            clause->proof_clauses.push_back(cid);
            res = parseNumFromVec(&cid);
        }
        clause->proof_clauses.push_back(cid); //add last one
        if (res != SUCCESS){
            return res;
        }

        //read lits and clause ID
        readAllNonzeroBytes();
        //remove 'a', parse bytes into clause ID and lits
        if (vec.back() != 'a'){
            printf("Parse error:  Binary LRAT file line does not");
            printf(" start with 'a' as expected\n");
        }
        vec.pop_back(); //'a'
        res = parseNumFromVec(&cid);
        if (res != SUCCESS){
            return res;
        }
        clause->clause_id = cid;
        if (!vec.empty()){
            res = parseNumFromVec(&cid);
            while (res == SUCCESS && !vec.empty()){
                clause->literals.push_back(cid);
                res = parseNumFromVec(&cid);
            }
            clause->literals.push_back(cid);
        }

        return res;
    }

private:
    //Read all bytes until a 0 byte, or the start of the file
    bool readAllNonzeroBytes(){
        char byte;
        bool readFine = rev.nextAsByte(byte);
        while (readFine && (byte != 0)){
            vec.push_back(byte);
            readFine = rev.nextAsByte(byte);
        }
        return readFine;
    }

    //Read a number from the back of the vector of bytes
    result_code_t parseNumFromVec(clause_id_t *out){
        clause_id_t unadjusted = 0;
        clause_id_t coefficient = 1;
        char byte = vec.back();
        vec.pop_back();
        while (byte & upper1){
            unadjusted += coefficient * (byte & lower7);
            coefficient *= 128; //2^7 to shift by another byte
            byte = vec.back();
            vec.pop_back();
        }
        //add last byte
        unadjusted += coefficient * (byte & lower7);

        if (unadjusted % 2){ //odds are negative
            *out = -(unadjusted - 1) / 2;
        }
        else{ //evens are positive
            *out = unadjusted / 2;
        }
        return SUCCESS;
    }
};


result_code_t binary_reverse_file(char *filename, char *revfilename){
    ReverseBinaryLratReader rev(filename);
    FILE *outfile = fopen(revfilename, "w");
    if (outfile == NULL){
        printf("Error:  Unable to open file %s\n", revfilename);
        return UNABLE_TO_OPEN;
    }

    clause_t clause;
    reset_clause(&clause);

    //read all the clauses and output them
    result_code_t res = rev.readRevClause(&clause);
    while (res == SUCCESS){
        output_added_clause(&clause, outfile, false, true);
        reset_clause(&clause);
        res = rev.readRevClause(&clause);
    }
    if (res == END_OF_FILE){
        res = SUCCESS;
    }
    if (fclose(outfile) == EOF){
        printf("Error:  Unable to close file %s\n", revfilename);
    }
    return res;
}


result_code_t byte_reverse_file(char *filename, char *revfilename){
    ReverseReader rev(filename);

    FILE *outfile = fopen(revfilename, "w");
    if (outfile == NULL){
        printf("Error:  Unable to open file %s\n", revfilename);
        return UNABLE_TO_OPEN;
    }

    char byte;
    bool res = rev.nextAsByte(byte);
    while (res){
        putc(byte, outfile);
        res = rev.nextAsByte(byte);
    }
    if (fclose(outfile) == EOF){
        printf("Error:  Unable to close file %s\n", revfilename);
    }
    return SUCCESS;
}





/********************************************************************
 *                       GENERAL REVERSE                            *
 ********************************************************************/

result_code_t reverse_file(char *filename, char *revfilename, bool_t is_binary){
    if (is_binary){
        return binary_reverse_file(filename, revfilename);
    }
    else{
        return text_reverse_file(filename, revfilename);
    }
}

