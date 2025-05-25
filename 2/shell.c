#include "shell.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

static void reap_background_processes();
int execute_command_block(const struct command_line *line, struct expr **expr_ptr, bool *need_exit);
static bool run_builtin_command(const struct command *command, bool *need_exit, int *exit_code);
static bool command_block_has_pipe(const struct expr *start, const struct expr *end);
static int run_pipeline(const struct command_line *line);
static void redirect_io(int input_fd, int output_fd);
static void close_unused_pipes(int input_fd, int is_last, int pipe_fds[2]);
static int run_single_command(const struct command_line *line, bool *need_exit);
static void apply_output_redirection(const struct command_line *line);
static int wait_for_processes(pid_t *process_ids, int process_count, pid_t last_process_id);

int execute_command_line(const struct command_line *line, bool *need_exit)
{
    reap_background_processes();

    int last_exit_code = 0;
    struct expr *expr = line->head;

    while (expr)
    {
        last_exit_code = execute_command_block(line, &expr, need_exit);
        if (*need_exit)
            return last_exit_code;

        if (!expr)
            break;

        if (expr->type == EXPR_TYPE_AND)
        {
            if (last_exit_code != 0)
            {
                do
                    expr = expr->next;
                while (expr && expr->type != EXPR_TYPE_OR && expr->type != EXPR_TYPE_AND);
            }
            expr = expr ? expr->next : NULL;
            continue;
        }

        if (expr->type == EXPR_TYPE_OR)
        {
            if (last_exit_code == 0)
            {
                do
                    expr = expr->next;
                while (expr && expr->type != EXPR_TYPE_OR && expr->type != EXPR_TYPE_AND);
            }
            expr = expr ? expr->next : NULL;
            continue;
        }
    }

    return last_exit_code;
}

static void reap_background_processes()
{
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;
}

int execute_command_block(const struct command_line *line, struct expr **expr_ptr, bool *need_exit)
{
    struct expr *start = *expr_ptr;
    struct expr *end = start;

    while (end->next)
    {
        enum expr_type type = end->next->type;
        if (type == EXPR_TYPE_COMMAND || type == EXPR_TYPE_PIPE)
            end = end->next;
        else
            break;
    }

    if (start == end && start->type == EXPR_TYPE_COMMAND)
    {
        int builtin_exit_code = 0;
        if (run_builtin_command(&start->cmd, need_exit, &builtin_exit_code))
        {
            *expr_ptr = end->next;
            return builtin_exit_code;
        }
    }

    struct command_line command_block = {
        .head = start,
        .tail = end,
        .out_file = line->out_file,
        .out_type = line->out_type,
        .is_background = line->is_background,
    };

    int exit_code = command_block_has_pipe(start, end)
                        ? run_pipeline(&command_block)
                        : run_single_command(&command_block, need_exit);

    *expr_ptr = end->next;
    return exit_code;
}

static bool run_builtin_command(const struct command *command, bool *need_exit, int *exit_code)
{
    if (strcmp(command->exe, "cd") == 0)
    {
        if (command->arg_count > 0 && chdir(command->args[0]) == -1)
            perror("chdir");
        *exit_code = 0;
        return true;
    }

    if (strcmp(command->exe, "exit") == 0)
    {
        *need_exit = true;
        *exit_code = command->arg_count > 0 ? atoi(command->args[0]) : 0;
        return true;
    }

    return false;
}

static bool command_block_has_pipe(const struct expr *start, const struct expr *end)
{
    for (const struct expr *expr = start; expr && expr != end->next; expr = expr->next)
        if (expr->type == EXPR_TYPE_PIPE)
            return true;

    return false;
}

static int run_pipeline(const struct command_line *line)
{
    struct expr *current_expr = line->head;
    int pipe_fds[2];
    int input_fd = -1;
    pid_t *process_ids = NULL;
    pid_t last_process_id = -1;
    int process_count = 0;

    while (current_expr && current_expr != line->tail->next)
    {
        if (current_expr->type == EXPR_TYPE_PIPE)
        {
            current_expr = current_expr->next;
            continue;
        }

        int is_last = !(current_expr->next && current_expr->next->type == EXPR_TYPE_PIPE);
        int output_fd = STDOUT_FILENO;

        if (!is_last && pipe(pipe_fds) == -1)
        {
            perror("pipe");
            exit(EXIT_FAILURE);
        }

        if (!is_last)
            output_fd = pipe_fds[1];

        pid_t child_pid = fork();
        if (child_pid == -1)
        {
            perror("fork");
            exit(EXIT_FAILURE);
        }

        if (child_pid == 0)
        {
            redirect_io(input_fd, output_fd);
            close_unused_pipes(input_fd, is_last, pipe_fds);

            struct command_line single_command = {
                .head = current_expr,
                .tail = current_expr,
                .out_file = is_last ? line->out_file : NULL,
                .out_type = is_last ? line->out_type : OUTPUT_TYPE_STDOUT,
                .is_background = 0};

            bool exit_flag = false;
            int code = run_single_command(&single_command, &exit_flag);
            _exit(code);
        }

        process_ids = realloc(process_ids, sizeof(pid_t) * (process_count + 1));
        if (!process_ids)
        {
            perror("realloc");
            exit(EXIT_FAILURE);
        }

        process_ids[process_count++] = child_pid;

        if (is_last)
            last_process_id = child_pid;

        if (input_fd != -1)
            close(input_fd);

        if (!is_last)
            close(pipe_fds[1]);

        input_fd = is_last ? -1 : pipe_fds[0];
        current_expr = current_expr->next;
    }

    if (input_fd != -1)
        close(input_fd);

    int exit_code = line->is_background
                        ? 0
                        : wait_for_processes(process_ids, process_count, last_process_id);

    free(process_ids);

    return exit_code;
}

static void redirect_io(int input_fd, int output_fd)
{
    if (input_fd != STDIN_FILENO && input_fd != -1)
        dup2(input_fd, STDIN_FILENO);

    if (output_fd != STDOUT_FILENO && output_fd != -1)
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

static int run_single_command(const struct command_line *line, bool *need_exit)
{
    if (!line || !line->head || line->head->type != EXPR_TYPE_COMMAND)
        return 0;

    struct command *command = &line->head->cmd;

    int builtin_exit_code = 0;
    if (run_builtin_command(command, need_exit, &builtin_exit_code))
        return builtin_exit_code;

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

    if (line->is_background)
        return 0;

    int status;
    waitpid(pid, &status, 0);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
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

static int wait_for_processes(pid_t *process_ids, int process_count, pid_t last_process_id)
{
    int status;
    int exit_code = 0;
    if (last_process_id != -1 && waitpid(last_process_id, &status, 0) > 0 && WIFEXITED(status))
        exit_code = WEXITSTATUS(status);

    for (int i = 0; i < process_count; ++i)
        if (process_ids[i] != last_process_id)
            waitpid(process_ids[i], NULL, 0);

    return exit_code;
}