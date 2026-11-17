# 사용법: make test   (sanitizer: make test CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined")
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g

test: build/parser_test
	./build/parser_test

build/parser_test: parser.c tests/parser_test.c dbms.h
	mkdir -p build
	$(CC) $(CFLAGS) -I. parser.c tests/parser_test.c -o $@

clean:
	rm -rf build

.PHONY: test clean
