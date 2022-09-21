
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

#include "reverse.hpp"


result_code_t reverse_file(char *filename, char *revfilename){
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

