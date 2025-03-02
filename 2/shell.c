#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 100

void execute_command(char *input)
{
    char *args[MAX_ARGS];
    int arg_count = 0;

    // Разбираем строку на аргументы
    char *token = strtok(input, " \t\n");
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
        // Дочерний процесс: запускаем команду
        if (execvp(args[0], args) == -1)
        {
            perror("Ошибка при exec");
            exit(1);
        }
    }
    else
    {
        // Родительский процесс: ждем завершения команды
        waitpid(pid, NULL, 0);
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

        execute_command(input);
    }

    return 0;
}