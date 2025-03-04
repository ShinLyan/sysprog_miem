#include "shell.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

// Функция для выполнения одной команды
static void execute_single_command(const struct command_line *line)
{
    // Если пустая команда, ничего не делаем
    if (!line || !line->head)
        return;

    struct expr *cmd_expr = line->head;
    if (cmd_expr->type != EXPR_TYPE_COMMAND)
        return;

    struct command *cmd = &cmd_expr->cmd;
    char *args[cmd->arg_count + 2];
    args[0] = cmd->exe;
    for (uint32_t i = 0; i < cmd->arg_count; i++)
        args[i + 1] = cmd->args[i];
    args[cmd->arg_count + 1] = NULL;

    // Встроенная команда cd
    if (strcmp(cmd->exe, "cd") == 0)
    {
        if (cmd->arg_count > 0)
            if (chdir(cmd->args[0]) == -1)
                perror("Ошибка при смене директории");
        return;
    }

    // Встроенная команда exit
    if (strcmp(cmd->exe, "exit") == 0)
        exit(cmd->arg_count > 0 ? atoi(cmd->args[0]) : 0);

    // Создаем дочерний процесс
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("Ошибка при fork");
        return;
    }

    if (pid == 0) // Дочерний процесс
    {
        // Перенаправление вывода в файл, если указано `> или >>`
        if (line->out_file)
        {
            int flags = O_WRONLY | O_CREAT;
            flags |= (line->out_type == OUTPUT_TYPE_FILE_APPEND) ? O_APPEND : O_TRUNC;
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

// Функция для выполнения команд с пайпами и перенаправлением
static void execute_piped_commands(const struct command_line *line)
{
    // Подсчитываем количество пайпов
    int num_pipes = 0;
    for (struct expr *e = line->head; e; e = e->next)
        if (e->type == EXPR_TYPE_PIPE)
            num_pipes++;

    int pipes[num_pipes][2];

    struct expr *cmd_expr = line->head;
    int i = 0, prev_fd = -1;
    while (cmd_expr)
    {
        // Перебираем все команды, пропуская пайпы
        if (cmd_expr->type == EXPR_TYPE_PIPE)
        {
            cmd_expr = cmd_expr->next;
            continue;
        }

        // Создаём пайп
        if (i < num_pipes)
        {
            if (pipe(pipes[i]) == -1)
            {
                perror("Ошибка при создании пайпа");
                return;
            }
        }

        // Создаём дочерний процесс
        pid_t pid = fork();
        if (pid == -1)
        {
            perror("Ошибка при fork");
            return;
        }

        if (pid == 0) // Дочерний процесс
        {
            if (prev_fd != -1) // Если не первая команда
            {
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }

            if (i < num_pipes) // Если не последняя команда
            {
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            // Закрываем ненужные дескрипторы
            for (int j = 0; j < num_pipes; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            struct command_line temp_line;
            temp_line.head = cmd_expr;
            temp_line.tail = cmd_expr;
            temp_line.out_file = NULL;
            temp_line.out_type = OUTPUT_TYPE_STDOUT;
            temp_line.is_background = 0;

            execute_single_command(&temp_line);
            exit(0);
        }
        else
        {
            // Родительский процесс закрывает пайпы
            if (prev_fd != -1)
                close(prev_fd);

            if (i < num_pipes)
                close(pipes[i][1]);

            prev_fd = pipes[i][0];
        }

        cmd_expr = cmd_expr->next;
        i++;
    }

    for (int j = 0; j < num_pipes; j++)
    {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }

    while (wait(NULL) > 0)
        ;
}

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