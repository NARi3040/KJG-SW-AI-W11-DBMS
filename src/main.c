#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.sql>\n", argv[0]);
        return 2;
    }
    // shortcut: 실행 흐름이 아직 없음, Phase 1 실행기·CLI 이슈에서 구현
    fprintf(stderr, "not implemented yet\n");
    return 1;
}
