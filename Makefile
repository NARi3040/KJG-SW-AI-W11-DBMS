# 빌드: make   테스트: make test   정리: make clean
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g -O2
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer

SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
LIB := $(filter-out src/main.c,$(SRC))
UNIT := $(patsubst tests/unit/%.c,build/%,$(wildcard tests/unit/test_*.c))

.PHONY: all test clean

all: minidb

minidb: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

# 테스트는 AddressSanitizer·UBSan을 켠 빌드로 돌려서 메모리 오류도 실패로 잡아요.
build/minidb-san: $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SAN) -o $@ $(SRC)

build/test_%: tests/unit/test_%.c $(LIB) $(HDR) | build
	$(CC) $(CFLAGS) $(SAN) -Isrc -o $@ $< $(LIB)

build:
	mkdir -p build

test: build/minidb-san $(UNIT)
	@set -e; for t in $(UNIT); do echo "== $$t"; ./$$t; done
	@tests/run_sql.sh build/minidb-san

clean:
	rm -rf minidb build
