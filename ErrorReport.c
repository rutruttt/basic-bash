#include <stdio.h> // For fprintf, stderr
#include <string.h>   // For strerror

/*
Function to print a user-friendly error message based on errno.
all the non builtin commands are executable files, executed by execvp. the error handling is inside the executable file itself
THEREFORE... this now is used onlyyyyy for the shell builtin errors (the ones that start with -bash)
attention, command not found is not even a shell builtin error, its some different specie... inspect it later.....
*/
void report_shell_error(const char* command, const char* operand, int err_code) {
    fprintf(stderr, "-bash: ");

    if(command)
        fprintf(stderr, "%s: ", command);
    if(operand)
        fprintf(stderr, "%s: ", operand);

    //print the specific error message based on err_code
    fprintf(stderr, "%s\n", strerror(err_code));
}