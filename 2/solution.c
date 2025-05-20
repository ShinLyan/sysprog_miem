#include "parser.h"
#include "shell.h"

#include <stdio.h>
#include <unistd.h>

int main(void)
{
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	int exit_code = 0;
	bool need_exit = false;
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0)
	{
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true)
		{
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE)
			{
				printf("Error: %d\n", (int)err);
				continue;
			}
			exit_code = execute_command_line(line, &need_exit);
			command_line_delete(line);
			if (need_exit)
			{
				parser_delete(p);
				return exit_code;
			}
		}
	}
	parser_delete(p);
	return exit_code;
}