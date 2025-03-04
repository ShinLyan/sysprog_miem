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
static void execute_single_command(const struct command_line *line);
static int handle_builtin_commands(const struct command *cmd);
static void execute_pipe(struct expr *cmd_expr, int input_fd, int output_fd);

void execute_command_line(const struct command_line *line)
{
    if (!line || !line->head)
        return;

    // Проверяем, есть ли пайп
    if (line->head->next)
        execute_piped_commands(line);
    else
        execute_single_command(line);
}

// Выполняем команды с пайпами
static void execute_piped_commands(const struct command_line *line)
{
    struct expr *cmd_expr = line->head;
    int pipe_fds[2];
    int input_fd = STDIN_FILENO;

    while (cmd_expr)
    {
        // Перебираем все команды, пропуская пайпы
        if (cmd_expr->type == EXPR_TYPE_PIPE)
        {
            cmd_expr = cmd_expr->next;
            continue;
        }

        if (cmd_expr->next && cmd_expr->next->type == EXPR_TYPE_PIPE)
        {
            // Создаём новый пайп
            if (pipe(pipe_fds) == -1)
            {
                perror("Ошибка при создании пайпа");
                return;
            }
            execute_pipe(cmd_expr, input_fd, pipe_fds[1]);

            close(pipe_fds[1]);
            input_fd = pipe_fds[0];
        }
        else
        {
            execute_pipe(cmd_expr, input_fd, STDOUT_FILENO);
            if (input_fd != STDIN_FILENO)
                close(input_fd);
        }

        cmd_expr = cmd_expr->next;
    }

    // Ждём завершения всех процессов
    while (wait(NULL) > 0)
        ;
}

// Запускаем команду в пайпе
static void execute_pipe(struct expr *cmd_expr, int input_fd, int output_fd)
{
    // Создаём дочерний процесс
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("Ошибка при fork");
        return;
    }

    if (pid == 0) // Дочерний процесс
    {
        // Если команда не первая, перенаправляем STDIN в input_fd
        if (input_fd != STDIN_FILENO)
        {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }

        // Если команда не последняя, перенаправляем STDOUT в output_fd
        if (output_fd != STDOUT_FILENO)
        {
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }

        // Запускаем команду
        struct command_line temp_line = {cmd_expr, cmd_expr, OUTPUT_TYPE_STDOUT, NULL, 0};
        execute_single_command(&temp_line);
        exit(0);
    }
}

// Выполняем одну команду (без пайпов)
static void execute_single_command(const struct command_line *line)
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
    if (handle_builtin_commands(cmd))
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
static int handle_builtin_commands(const struct command *cmd)
{
    if (strcmp(cmd->exe, "cd") == 0)
    {
        if (cmd->arg_count > 0 && chdir(cmd->args[0]) == -1)
            perror("Ошибка при смене директории");
        return 1;
    }

    if (strcmp(cmd->exe, "exit") == 0)
        exit(cmd->arg_count > 0 ? atoi(cmd->args[0]) : 0);

    return 0;
}