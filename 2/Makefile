EXE = mybash
GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all:
	gcc $(GCC_FLAGS) solution.c parser.c shell.c -o $(EXE)

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c -o $(EXE)

clean:
	rm -rf $(EXE) "__pycache__" "testdir"

test: clean all
	python3 checker.py -e $(EXE)

.PHONY: all test_glob clean test