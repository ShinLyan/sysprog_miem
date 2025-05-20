#include "shell.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

void execute_command_line(const struct command_line *line);

static void execute_piped_commands(const struct command_line *line);

static void execute_single_command(const struct command_line *line, int is_main_process);

static int handle_builtin_commands(const struct command *cmd, int is_main_process);

void execute_command_line(const struct command_line *line)
{
    if (!line || !line->head)
        return;

    // Проверяем, есть ли пайп
    if (line->head->next)
        execute_piped_commands(line);
    else
        execute_single_command(line, 1);
}

// Выполняем команды с пайпами
static void execute_piped_commands(const struct command_line *line)
{
    struct expr *cmd_expr = line->head;
    int prev_pipe_fd = -1;
    int pipe_fds[2];
    pid_t pids[256]; // максимум процессов в пайпе
    int pid_count = 0;

    while (cmd_expr)
    {
        if (cmd_expr->type == EXPR_TYPE_PIPE)
        {
            cmd_expr = cmd_expr->next;
            continue;
        }

        int is_last = !(cmd_expr->next && cmd_expr->next->type == EXPR_TYPE_PIPE);

        int input_fd = prev_pipe_fd;
        int output_fd = STDOUT_FILENO;

        if (!is_last)
        {
            if (pipe(pipe_fds) == -1)
            {
                perror("pipe");
                exit(1);
            }
            output_fd = pipe_fds[1];
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            perror("fork");
            exit(1);
        }

        if (pid == 0)
        {
            // child
            if (input_fd != -1)
            {
                dup2(input_fd, STDIN_FILENO);
            }
            if (!is_last)
            {
                dup2(output_fd, STDOUT_FILENO);
            }

            // Закрываем оба конца пайпа, если они были
            if (input_fd != -1)
                close(input_fd);
            if (!is_last)
                close(pipe_fds[0]), close(pipe_fds[1]);

            struct command_line temp_line = {
                .head = cmd_expr,
                .tail = cmd_expr,
                .out_file = is_last ? line->out_file : NULL,
                .out_type = is_last ? line->out_type : OUTPUT_TYPE_STDOUT,
                .is_background = 0};

            execute_single_command(&temp_line, 0);
            _exit(0);
        }

        // parent
        pids[pid_count++] = pid;

        if (input_fd != -1)
            close(input_fd);
        if (!is_last)
            close(pipe_fds[1]); // закрыл свою копию писателя

        prev_pipe_fd = is_last ? -1 : pipe_fds[0];
        cmd_expr = cmd_expr->next;
    }

    // ждем всех дочерних процессов
    for (int i = 0; i < pid_count; i++)
        waitpid(pids[i], NULL, 0);
}

// Выполняем одну команду (без пайпов)
static void execute_single_command(const struct command_line *line, int is_main_process)
{
    // Если команда пустая или не является командой – ничего не делаем
    if (!line || !line->head || line->head->type != EXPR_TYPE_COMMAND)
        return;

    // Создаём массив аргументов
    struct command *cmd = &line->head->cmd;
    char *args[cmd->arg_count + 2];
    args[0] = cmd->exe;
    for (uint32_t i = 0; i < cmd->arg_count; i++)
        args[i + 1] = cmd->args[i];

    args[cmd->arg_count + 1] = NULL;

    // Обрабатываем встроенные команды
    if (handle_builtin_commands(cmd, is_main_process))
        return;

    // Создаем дочерний процесс
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("Ошибка при fork");
        return;
    }

    if (pid == 0) // Дочерний процесс
    {
        // Перенаправление вывода в файл (если есть `>` или `>>`)
        if (line->out_file)
        {
            int flags = O_WRONLY | O_CREAT |
                        (line->out_type == OUTPUT_TYPE_FILE_APPEND ? O_APPEND : O_TRUNC);
            int fd = open(line->out_file, flags, 0644);
            if (fd == -1)
            {
                perror("Ошибка открытия файла");
                exit(1);
            }

            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        execvp(args[0], args);
        perror("Ошибка при exec");
        exit(1);
    }
    else // Родительский процесс
        waitpid(pid, NULL, 0);
}

// Проверяем встроенные команды (cd, exit)
static int handle_builtin_commands(const struct command *cmd, int is_main_process)
{
    if (strcmp(cmd->exe, "cd") == 0)
    {
        if (cmd->arg_count > 0 && chdir(cmd->args[0]) == -1)
            perror("Ошибка при смене директории");

        return 1;
    }

    if (strcmp(cmd->exe, "exit") == 0)
    {
        int code = cmd->arg_count > 0 ? atoi(cmd->args[0]) : 0;
        if (is_main_process)
            exit(code); // Завершает shell
        else
            _exit(code); // Завершает только текущий процесс

        return 1;
    }

    return 0;
}