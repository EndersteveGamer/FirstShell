#include <stdio.h>
#include <unistd.h>
#include <wait.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <libgen.h>
#include <pwd.h>

#include "cmdline.h"

#define BUFLEN 512
#define ENDSTATUS_BUF_LEN 4096

#define YES_NO(i) ((i) ? "Y" : "N")

char *endstatus;

/**
 * Prints how a process ended
 * @param stat The stat of how the process ended
 * @param pid The PID of the process that ended
 */
void display_process_end(int stat, pid_t pid) {
    // Print child end status
    if (WIFEXITED(stat)) {
        snprintf(
                endstatus + strlen(endstatus), ENDSTATUS_BUF_LEN - strlen(endstatus),
                "PID %d finished with exit status %i\n", pid, WEXITSTATUS(stat)
        );
    }
    if (WIFSIGNALED(stat)) {
        snprintf(
                endstatus + strlen(endstatus), ENDSTATUS_BUF_LEN - strlen(endstatus),
                "PID %d finished with signal %i\n", pid, WTERMSIG(stat)
        );
    }
}

/**
 * An empty handler for SIGINT
 */
void sigint_handler() {}

/**
 * The handler for SIGCHLD
 */
void sigchld_handler() {
    int stat;
    pid_t pid;
    do {
        pid = waitpid(-1, &stat, WNOHANG);
        if (pid > 0) display_process_end(stat, pid);
    } while (pid != -1);
}

/**
 * Executes a command
 * @param line The command line the command is from
 * @param command The command to execute
 * @param commandIndex The index of the command in the list of commands
 * @param pipeIn The fid of the pipe to use. -1 if no pipe has to be used
 * @return The fid of the pipe opened for the command, -1 if an error occured
 */
int execute_command(struct line *line, struct cmd *command, size_t commandIndex, int pipeIn) {
    // Opening pipe if needed
    int pipes[2];
    if (commandIndex != line->n_cmds - 1) pipe(pipes);

    // Forking
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed");
        return -1;
    }


    if (pid == 0) {
        if (line->background) {
            // Installing SIGINT signal handler
            struct sigaction action;
            action.sa_flags = 0;
            sigemptyset(&action.sa_mask);
            action.sa_handler = SIG_IGN;
            sigaction(SIGCHLD, &action, NULL);
        }

        // Redirecting input
        if (pipeIn > 0) {
            dup2(pipeIn, STDIN_FILENO);
            close(pipeIn);
        }
        else if ((commandIndex == 0 && line->file_input != NULL) || line->background) {
            int input = open(
                    line->file_input != NULL ? line->file_input : "/dev/null",
                    O_RDONLY
            );
            if (input == -1) perror("Input redirection failed");
            else {
                dup2(input, STDIN_FILENO);
                close(input);
            }
        }

        // Redirecting output
        if (commandIndex != line->n_cmds - 1) {
            dup2(pipes[1], STDOUT_FILENO);
            close(pipes[1]);
        }
        if (commandIndex == line->n_cmds - 1 && line->file_output != NULL) {
            int output = open(
                    line->file_output,
                    O_WRONLY | O_CREAT | (line->file_output_append ? O_APPEND : O_TRUNC)
            );
            if (output == -1) perror("Output redirection failed");
            else {
                dup2(output, STDOUT_FILENO);
                close(output);
            }
        }

        // Execute the command
        execvp(command->args[0], command->args);
        perror("execvp failed");
        exit(1);
    }

    if (commandIndex != line->n_cmds - 1) close(pipes[1]);
    if (pipeIn > 0) close(pipeIn);

    if (!line->background && commandIndex == line->n_cmds - 1) {
        int stat;
        waitpid(pid, &stat, 0);
        display_process_end(stat, pid);
    }
    if (commandIndex != line->n_cmds - 1) return pipes[0];
    else return -1;
}

/**
 * Change the current working directory
 * @param path The path to set the current working directory to
 */
void cd(char *path) {
    char *newPath = NULL;

    // Get the home path
    if (strcmp(path, "~") == 0) {
        newPath = getenv("HOME");
        if (newPath == NULL) {
            fprintf(stderr, "Error while reading the HOME environment variable");
            return;
        }
    }
    if (strlen(path) >= 2 && path[0] == '~' && path[1] != '/') {
        char* user = calloc(BUFLEN, sizeof(char));
        size_t index = 0;
        path++;
        while (*path != '/' && *path != '\0') {
            user[index] = *path;
            index++;
            path++;
        }
        struct passwd *pw = getpwnam(user);
        if (pw == NULL) {
            fprintf(stderr, "This user does not exist\n");
            return;
        }
        newPath = pw->pw_dir;
    }

    // Set the new current working directory
    int status = chdir(newPath != NULL ? newPath : path);
    if (status == -1) perror("Failed to set working directory");
    else if (status != 0) {
        fprintf(stderr, "Failed to set working directory with error %d", status);
    }
}

/**
 * Process a command line by executing all its commands
 * @param line The line to process
 */
void execute_line(struct line *line) {
    int currPipe = -1;
    for (size_t i = 0; i < line->n_cmds; ++i) {
        // Execute the cd command
        if (line->cmds[i].n_args == 2 && strcmp(line->cmds[i].args[0], "cd") == 0) {
            cd(line->cmds[i].args[1]);
        }
        // Execute other commands
        else currPipe = execute_command(
                line,
                &line->cmds[i],
                i,
                currPipe
        );
    }
}

int main() {
    // Create buffer for displaying end status
    endstatus = calloc(ENDSTATUS_BUF_LEN, sizeof(char));

    // Install SIGINT signal handler
    struct sigaction action;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);
    action.sa_handler = sigint_handler;
    sigaction(SIGINT, &action, NULL);

    // Installing SIGCHLD signal handler
    struct sigaction act2;
    act2.sa_flags = SA_RESTART;
    sigemptyset(&act2.sa_mask);
    act2.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &act2, NULL);

    struct line li;
    char buf[BUFLEN];

    line_init(&li);


    for (;;) {
        // Display end status
        if (strlen(endstatus) > 0) {
            fprintf(stderr, "%s", endstatus);
            for (int i = 0; i < ENDSTATUS_BUF_LEN; ++i) endstatus[i] = '\0';
        }

        // Display prompt
        char *cwd = getcwd(NULL, 0);
        printf("fish %s> ", cwd != NULL ? basename(cwd) : "");
        if (cwd != NULL) free(cwd);

        fgets(buf, BUFLEN, stdin);

        int err = line_parse(&li, buf);
        if (err) {
            //the command line entered by the user isn't valid
            line_reset(&li);
            continue;
        }

        fprintf(stderr, "Command line:\n");
        fprintf(stderr, "\tNumber of commands: %zu\n", li.n_cmds);

        for (size_t i = 0; i < li.n_cmds; ++i) {
            fprintf(stderr, "\t\tCommand #%zu:\n", i);
            fprintf(stderr, "\t\t\tNumber of args: %zu\n", li.cmds[i].n_args);
            fprintf(stderr, "\t\t\tArgs:");
            for (size_t j = 0; j < li.cmds[i].n_args; ++j) {
                fprintf(stderr, " \"%s\"", li.cmds[i].args[j]);
            }
            fprintf(stderr, "\n");
        }

        fprintf(stderr, "\tRedirection of input: %s\n", YES_NO(li.file_input));
        if (li.file_input) {
            fprintf(stderr, "\t\tFilename: '%s'\n", li.file_input);
        }

        fprintf(stderr, "\tRedirection of output: %s\n", YES_NO(li.file_output));
        if (li.file_output) {
            fprintf(stderr, "\t\tFilename: '%s'\n", li.file_output);
            fprintf(stderr, "\t\tMode: %s\n", li.file_output_append ? "APPEND" : "TRUNC");
        }

        fprintf(stderr, "\tBackground: %s\n", YES_NO(li.background));

        // Handle the exit command
        if (
                li.n_cmds == 1
                && li.cmds[0].n_args == 1
                && strcmp(li.cmds[0].args[0], "exit") == 0
        ) {
            free(endstatus);
            line_reset(&li);
            return 0;
        }

        execute_line(&li);

        line_reset(&li);
    }
}
