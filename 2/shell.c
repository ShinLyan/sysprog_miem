#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 100
#define MAX_CMDS 10

// Функция для обработки вывода в файл (>, >>)
int handle_output_redirection(char *cmd, char **new_cmd, char **file, int *append)
{
    *append = 0;
    *file = NULL;

    char *out = strstr(cmd, ">>");
    if (out)
        *append = 1;
    else
        out = strstr(cmd, ">");

    if (out)
    {
        *out = '\0'; // Обрезаем команду перед `>` или `>>`
        *new_cmd = cmd;

        out += (*append ? 2 : 1); // Пропускаем `>` или `>>`
        while (*out == ' ')
            out++; // Убираем пробелы

        if (*out == '\0')
        {
            fprintf(stderr, "Ошибка: отсутствует имя файла для перенаправления.\n");
            return -1;
        }

        *file = out;
    }
    else
        *new_cmd = cmd;

    return 0;
}

// Функция для выполнения одной команды с учетом `>` и `>>`
void execute_single_command(char *cmd)
{
    char *args[MAX_ARGS];
    int arg_count = 0;
    char *file;
    int append;

    // Обрабатываем перенаправление вывода
    if (handle_output_redirection(cmd, &cmd, &file, &append) == -1)
        return;

    // Разбираем строку на аргументы
    char *token = strtok(cmd, " \t\n");
    while (token != NULL)
    {
        args[arg_count++] = token;
        token = strtok(NULL, " \t\n");
    }
    args[arg_count] = NULL; // Завершаем список аргументов

    if (arg_count == 0) // Пустой ввод
        return;

    // Создаем дочерний процесс
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("Ошибка при fork");
        return;
    }

    if (pid == 0)
    {
        // Если есть файл для вывода, перенаправляем stdout
        if (file)
        {
            int fd = open(file, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
            if (fd == -1)
            {
                perror("Ошибка открытия файла");
                exit(1);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        if (execvp(args[0], args) == -1)
        {
            perror("Ошибка при exec");
            exit(1);
        }
    }
    else
        // Родительский процесс: ждем завершения команды
        waitpid(pid, NULL, 0);
}

// Функция для выполнения команд с пайпами и перенаправлением
void execute_piped_commands(char *input)
{
    char *cmds[MAX_CMDS];
    int cmd_count = 0;

    // Разделяем строку по пайпам "|"
    char *token = strtok(input, "|");
    while (token != NULL)
    {
        cmds[cmd_count++] = token;
        token = strtok(NULL, "|");
    }

    int fd[2];       // Дескрипторы пайпа
    int prev_fd = 0; // Предыдущий дескриптор чтения

    for (int i = 0; i < cmd_count; i++)
    {
        pipe(fd);
        pid_t pid = fork();

        if (pid == -1)
        {
            perror("Ошибка при fork");
            return;
        }

        if (pid == 0) // Дочерний процесс
        {
            dup2(prev_fd, STDIN_FILENO); // Входные данные из предыдущей команды
            if (i < cmd_count - 1)
            {
                dup2(fd[1], STDOUT_FILENO); // Вывод в пайп
            }

            close(fd[0]);                    // Закрываем неиспользуемый дескриптор
            execute_single_command(cmds[i]); // Выполняем команду
            exit(0);
        }
        else // Родительский процесс
        {
            waitpid(pid, NULL, 0);
            close(fd[1]);    // Закрываем запись в пайп
            prev_fd = fd[0]; // Обновляем предыдущий дескриптор чтения
        }
    }
}

int main()
{
    char input[MAX_INPUT_SIZE];
    while (1)
    {
        // Читаем ввод от пользователя
        printf("> "); // Выводим приглашение терминала
        if (fgets(input, sizeof(input), stdin) == NULL)
        {
            printf("\nВыход...\n");
            break;
        }

        // Удаляем символ новой строки в конце ввода
        input[strcspn(input, "\n")] = '\0';

        // Проверка на команду "exit"
        if (strcmp(input, "exit") == 0)
        {
            printf("Завершение shell...\n");
            break;
        }

        // Проверяем, есть ли пайп
        if (strchr(input, '|'))
        {
            execute_piped_commands(input);
        }
        else
        {
            execute_single_command(input);
        }
    }

    return 0;
}