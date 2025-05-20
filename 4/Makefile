GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -ldl -rdynamic

all: test

test:
	gcc $(GCC_FLAGS) thread_pool.c test.c ../utils/unit.c -I ../utils ../utils/heap_help/heap_help.c -o test

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -o test

clean:
	rm -rf test