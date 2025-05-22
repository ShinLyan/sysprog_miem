GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all: test

test: clean build
	./test

build:
	gcc $(GCC_FLAGS) userfs.c test.c ../utils/unit.c -I ../utils -o test

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -o test

clean:
	rm -rf test