#include "shell.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_CHILD_PROCESSES 1024

static int execute_pipeline(const struct command_line *line);
static void redirect_pipes(int input_fd, int output_fd);
static void close_unused_pipes(int input_fd, int is_last, int pipe_fds[2]);
static int execute_single_command(const struct command_line *line, bool *need_exit);
static int handle_builtin_commands(const struct command *cmd, bool *need_exit);
static void apply_output_redirection(const struct command_line *line);

int execute_command_line(const struct command_line *line, bool *need_exit)
{
    if (!line || !line->head)
        return 0;

    return line->head->next
               ? execute_pipeline(line)
               : execute_single_command(line, need_exit);
}

static int execute_pipeline(const struct command_line *line)
{
    struct expr *current_expression = line->head;
    int previous_pipe_read_end = -1;
    int pipe_fd[2];
    pid_t child_pids[MAX_CHILD_PROCESSES];
    int child_count = 0;
    int last_exit_code = 0;

    while (current_expression)
    {
        if (current_expression->type == EXPR_TYPE_PIPE)
        {
            current_expression = current_expression->next;
            continue;
        }

        int is_last_command = !(current_expression->next && current_expression->next->type == EXPR_TYPE_PIPE);
        int input_fd = previous_pipe_read_end;
        int output_fd = STDOUT_FILENO;

        if (!is_last_command && pipe(pipe_fd) == -1)
        {
            perror("pipe");
            exit(1);
        }

        if (!is_last_command)
            output_fd = pipe_fd[1];

        pid_t child_pid = fork();
        if (child_pid == -1)
        {
            perror("fork");
            exit(EXIT_FAILURE);
        }

        if (child_pid == 0)
        {
            redirect_pipes(input_fd, output_fd);
            close_unused_pipes(input_fd, is_last_command, pipe_fd);

            struct command_line single_command = {
                .head = current_expression,
                .tail = current_expression,
                .out_file = is_last_command ? line->out_file : NULL,
                .out_type = is_last_command ? line->out_type : OUTPUT_TYPE_STDOUT,
                .is_background = 0};

            bool exit_flag = false;
            int code = execute_single_command(&single_command, &exit_flag);
            _exit(code);
        }

        if (child_count < MAX_CHILD_PROCESSES)
            child_pids[child_count++] = child_pid;

        if (input_fd != -1)
            close(input_fd);

        if (!is_last_command)
            close(pipe_fd[1]);

        previous_pipe_read_end = is_last_command ? -1 : pipe_fd[0];
        current_expression = current_expression->next;
    }

    if (previous_pipe_read_end != -1)
        close(previous_pipe_read_end);

    for (int i = 0; i < child_count; i++)
    {
        int status;
        waitpid(child_pids[i], &status, 0);
        if (i == child_count - 1 && WIFEXITED(status))
            last_exit_code = WEXITSTATUS(status);
    }

    return last_exit_code;
}

static void redirect_pipes(int input_fd, int output_fd)
{
    if (input_fd != STDIN_FILENO)
        dup2(input_fd, STDIN_FILENO);

    if (output_fd != STDOUT_FILENO)
        dup2(output_fd, STDOUT_FILENO);
}

static void close_unused_pipes(int input_fd, int is_last, int pipe_fds[2])
{
    if (input_fd != -1)
        close(input_fd);

    if (!is_last)
    {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }
}

static int execute_single_command(const struct command_line *line, bool *need_exit)
{
    if (!line || !line->head || line->head->type != EXPR_TYPE_COMMAND)
        return 0;

    struct command *command = &line->head->cmd;

    if (handle_builtin_commands(command, need_exit))
        return command->arg_count > 0 ? atoi(command->args[0]) : 0;

    char *args[command->arg_count + 2];
    args[0] = command->exe;
    for (uint32_t i = 0; i < command->arg_count; i++)
        args[i + 1] = command->args[i];

    args[command->arg_count + 1] = NULL;

    // Создаем дочерний процесс
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return 0;
    }

    if (pid == 0) // Дочерний процесс
    {
        apply_output_redirection(line);
        execvp(args[0], args);
        perror("execvp");
        exit(EXIT_FAILURE);
    }

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int handle_builtin_commands(const struct command *cmd, bool *need_exit)
{
    if (strcmp(cmd->exe, "cd") == 0)
    {
        if (cmd->arg_count > 0 && chdir(cmd->args[0]) == -1)
            perror("chdir");

        return 1;
    }

    if (strcmp(cmd->exe, "exit") == 0)
    {
        *need_exit = true;
        return 1;
    }

    return 0;
}

static void apply_output_redirection(const struct command_line *line)
{
    if (!line->out_file)
        return;

    int flags = O_WRONLY | O_CREAT |
                (line->out_type == OUTPUT_TYPE_FILE_APPEND ? O_APPEND : O_TRUNC);
    int fd = open(line->out_file, flags, 0644);
    if (fd == -1)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }

    dup2(fd, STDOUT_FILENO);
    close(fd);
}