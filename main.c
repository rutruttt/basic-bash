#include "Constants.h"
#include "ErrorReport.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>   // For access to POSIX operating system API (read, write, close etc)
#include <fcntl.h>    // For open files control (open, O_RDONLY, O_WRONLY, O_CREAT, O_APPEND, O_TRUNC)
#include <sys/wait.h> // For waitpid
#include <sys/stat.h> // For stat, S_ISDIR, umask, S_IRUSR, S_IWUSR, S_IRGRP, S_IROTH
#include <stdlib.h>   // For exit, strtol
#include <string.h>

#include <stdbool.h>    //for C and not CPP :'( alsooooooo NULL not nullptr!!

//a little helper that cstring doesnt have:
void strcpy_till_space(char* dest, const char* src) {
    while (*src && *src != ' ' && *src != '\n') {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static char path_buffer[MAX_PATH];    // Stores the current working directory path for the prompt
static char line_buffer[MAX_LINE];    // Buffer to read user input line
static char argv[MAX_ARGS][MAX_NAME]; // Stores command arguments (the first word in non-builtin commands is itself an 'argument' - the command is an executable file...)
static int argc;                      // Number of arguments
static mode_t shell_umask;

void get_command() {
    char* ptrBuffer = line_buffer; // Pointer to current position in line_buffer

    int bytes_left_in_line = MAX_LINE;
    while (true) {
        int bytes_read = read(0,ptrBuffer,bytes_left_in_line);
        if (bytes_read>0 && ptrBuffer[bytes_read-1] == '\n'){
            ptrBuffer[bytes_read-1] == '\0';
            return;
        }
        ptrBuffer += bytes_read;
        bytes_left_in_line -= bytes_read;
    }
}

// During redirection we will have the fds 0,1,2 pointing to OTHER files instead of the standard streams.
// So if we save another pointer to the same file - namely, duplicate the fd before redirecting it - then we will be able to restore the fd to the original file later.
// In case the stream wasn't redirected, there is no need to duplicate its fd so original_fds[i] = UNREDIRECTED
static int original_fds[3]; // 0: stdin, 1: stdout, 2: stderr

void redirections_init(){
    original_fds[0] = UNREDIRECTED;
    original_fds[1] = UNREDIRECTED;
    original_fds[2] = UNREDIRECTED;
}

// Function to set up I/O redirection for a command
// This function needs to be called *before* executing the command
// It returns true on success, false on error.
bool redirections_setup(const char* filename, int flags, int target_fd) {
    // For output redirection, define default permissions.
    // S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH corresponds to 0644 (rw-r--r--)
    // This is a common default and often matches the permissions after applying a typical umask (0022).
    mode_t requested_file_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH; // 0666 (rw-rw-rw-)

    // Apply the umask: (requested_permissions & ~shell_umask)
    mode_t final_mode = requested_file_mode & ~shell_umask;

    // Open the file with specified flags and calculated permissions
    int fd = open(filename, flags, final_mode);
    if (fd == SYS_FAIL) {
        report_shell_error(NULL, filename, errno);
        return false;
    }

    if (original_fds[target_fd] == UNREDIRECTED) { 
        original_fds[target_fd] = dup(target_fd); 
    }
    
    dup2(fd, target_fd);
    close(fd); // Close the original new FD, as target_fd now points to it
    return true;
}

// Restore the standard stream fds.
void redirections_cleanup() {
    for (int i = 0; i < 3; ++i) {
        if (original_fds[i] != UNREDIRECTED) { // If an original FD was saved
            dup2(original_fds[i], i); // Restore it
            close(original_fds[i]);  // Close the duplicated original FD
            original_fds[i] = UNREDIRECTED;    // Reset the flag
        }
    }
}

// Function to parse the command line and set up redirections
// Returns true if parsing is successful, false if a redirection or argument limit error occurs.
bool parse_command() {
    argc = 0;
    const char* p = line_buffer; // Pointer to traverse the input line
    char name[MAX_NAME]; // Buffer for redirection filenames

    // Loop through the input line to parse command, arguments, and redirections
    while (true) {
        // Advance pointer past any spaces to the start of the next word or redirection
        while (*p == ' ') p++;
        if (*p == '\0') 
            return true;

        if (*p == '>' || (*p == '2' && *(p+1) == '>')) { // Output redirection (>, >>, 2>, 2>>)
            int new_flags = O_WRONLY | O_CREAT; // Default flags for output redirection
            int target_fd = STDOUT_FILENO;      // Default target is stdout

            if (*p == '2') { // Check for stderr redirection (2>)
                target_fd = STDERR_FILENO;
                p++; // Consume '2'
            }
            p++; // Consume first '>'

            if (*p == '>') { // Check for append redirection (>>)
                new_flags |= O_APPEND;
                p++; // Consume second '>'
            } else {
                new_flags |= O_TRUNC;
            }

            while (*p == ' ') p++; // Skip spaces after redirection operator

            strcpy_till_space(name, p); // Extract filename for redirection
            if (!redirections_setup(name, new_flags, target_fd)) {
                // If redirection setup fails, restore and return false. 
                //This is exactly real bash behavior (redirection are serial from first to last, and only when reaches error stop the command)
                redirections_cleanup(); // Ensure fds are restored before exiting
                return false;
            }
        }
        else if (*p == '<') { // Input redirection (<)
            int new_flags = O_RDONLY; // Flags for input redirection

            p++; // Consume '<'
            while (*p == ' ') p++; // Skip spaces after redirection operator

            strcpy_till_space(name, p); // Extract filename for redirection
            if (!redirections_setup(name, new_flags, STDIN_FILENO)) {
                // If redirection setup fails, restore and return false.
                redirections_cleanup();
                return false;
            }
        }
        else { // Command name or argument
            if (argc >= MAX_ARGS) { 
                report_shell_error(NULL, NULL, E2BIG); // E2BIG: Argument list too long
                return false; 
            }
            strcpy_till_space(argv[argc], p);
            argc++;
        }
        // Advance pointer to the end of the current word
        while (*p && *p != ' ') p++;
    }
}


// Builtin commands implementations: cd, exit

void cd_command() {
    if (argc > 2) { // Too many arguments
        //errno = E2BIG;    nopeee, real bash doesnt use it for this case either (and therefore the message this time is not "Argument list too long")
        fprintf(stderr, "-bash: %s: too many arguments\n", argv[0]);
        return;
    }

    if (argc < 2) // No argument, change to home directory (or root for this simple shell)
        // For a full bash, this would go to HOME. Here, we'll just go to root (/) for simplicity
        strcpy(argv[1], "/");

    const char* target_dir = argv[1]; // Directory to change to

    if (chdir(target_dir) == SYS_FAIL) { // Attempt to change directory
        report_shell_error(argv[0], target_dir, errno);
        return;
    }

    getcwd(path_buffer, sizeof(path_buffer));
}

void exit_command() {
    redirections_cleanup(); // Restore FDs before exiting
    if (argc == 1){
        printf("Exiting shell.\n");
        exit(0); // Exit with the specified code
    }

    // If we get here then argc > 1 - so there is for SURE argument provided: argv[1][0] != '\0' is true.
    char* endptr;
    long exit_code = strtol(argv[1], &endptr, 10); // Attempt to convert argument to a long integer

    // Check for conversion errors or if extra characters exist after the number
    if (*endptr != '\0') {
        fprintf(stderr, "-bash: exit: %s: numeric argument required\n", argv[1]); //
        exit(2);    // this is the exit code for MISUSE OF SHELL BUILTINS
    }
    printf("Exiting shell.\n");
    exit((int)exit_code); // Exit with the specified code
}

void execute_external_command() {
    pid_t pid = fork(); // Create a child process

    if (pid == -1) {
        // Fork failed
        report_shell_error(argv[0], NULL, errno);
    } 
    else if (pid == 0) {
        // Child process
        // Prepare arguments for execvp
        char* exec_argv[MAX_ARGS + 1]; // +1 for NULL terminator
        for (int i = 0; i < argc; ++i) {
            exec_argv[i] = argv[i];
        }
        exec_argv[argc] = NULL; // execvp requires null-terminated argument list

        // Execute the command
        execvp(argv[0], exec_argv);

        // If execvp returns, it means an error occurred
        if (errno == ENOENT)
            fprintf(stderr,"%s: command not found\n", argv[0]); // This message format is common for ENOENT
        else    // For other execvp errors, use report_shell_error to get the -bash: prefix
            report_shell_error(argv[0], NULL, errno);
        exit(127); // Standard exit status for command execution failures
    } 
    else {
        // Parent process
        int status;
        waitpid(pid, &status, 0); // Wait for the child process to complete

        // Optionally, check child's exit status
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 127) {
            //fprintf(stderr, "%s: exited with status %d\n", command_name, WEXITSTATUS(status));
        }
    }
}

// Main entry point for the shell
int main() {
    getcwd(path_buffer, sizeof(path_buffer));
    redirections_init();

    while (true) {
        printf("%s$ ", path_buffer);
        fflush(stdout);

        get_command();

        if (!parse_command() || argc == 0) {
            // Error occurred during parsing or no command was found (e.g., only redirections)
            // parse_command already reports the error if occured, so just continue to next prompt
            redirections_cleanup();
            continue;
        }

        // --- Execute Command ---
        // Check for built-in commands first
        if (strcmp(argv[0], "cd") == 0)
            cd_command();
        else if (strcmp(argv[0], "exit") == 0)
            exit_command();
        else // If not a built-in, try to execute as an external command
            execute_external_command();
        
        redirections_cleanup();
    }

    return 0; // Should not be reached in an infinite loop
}