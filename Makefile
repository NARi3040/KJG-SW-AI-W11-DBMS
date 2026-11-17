# 사용법: make test
# sanitizer: make test CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all"
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g

# 파일이 작아서 매번 새로 빌드해요. CFLAGS를 바꿔도 이전 바이너리를 돌리지 않아요.
test:
	mkdir -p build
	$(CC) $(CFLAGS) -I. parser.c tests/parser_test.c -o build/parser_test
	./build/parser_test

clean:
	rm -rf build

.PHONY: test clean
