# 테스트 (이슈 #9)

실제 모듈을 쓰는 SQL 통합 테스트와 최소 파서·저장 단위 테스트예요. 이 브랜치에는
`main.c`, `parser.c`, `execute.c`, `storage.c`가 없으므로 통합 테스트는 실행되지
않고, `run_tests.py`가 누락 모듈을 보고하며 0이 아닌 코드로 끝나요. 가짜 구현이나
skip-as-success는 쓰지 않아요.

## 파일

| 파일 | 범위 |
| --- | --- |
| `test_integration.py` | `docs/test.md` 01~18, CLI 인자·빈 입력·128MiB 경계 |
| `test_parser.c` | P01 INSERT/WHERE SELECT, P02 문법 오류 한 줄 NUL |
| `test_storage.c` | S01 빈 스캔→2행 저장→재스캔, S02 close/reopen 후 1행 추가 |
| `run_tests.py` | header, P/S, 있으면 CLI/execute 테스트, 통합 테스트 |

## 의존성

- Python 3.10+ 표준 라이브러리만 사용
- `gcc` (C11). `GCC` 환경 변수로 경로를 지정할 수 있어요.
- 통합 테스트는 `DBMS_EXE`로 실제 실행 파일을 받거나, 저장소 루트의
  `main.c`·`parser.c`·`execute.c`·`storage.c`를 임시 디렉터리에서 빌드해요.
- 단위 테스트는 각각 `parser.c`, `storage.c`, `execute.c`가 필요해요.

## 명령

```bash
# 이 브랜치 전체: 누락 모듈이 있으면 INCOMPLETE + 종료 코드 1
python tests/run_tests.py

# 헤더 계약
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . -fsyntax-only tests/header_contract.c

# 새 P/S 테스트 문법 검사 (모듈 없이 가능)
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . -fsyntax-only tests/test_parser.c
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . -fsyntax-only tests/test_storage.c

# 실제 모듈이 있을 때
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . parser.c tests/test_parser.c -o parser_test
./parser_test
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . execute.c tests/test_execute.c -o execute_test
./execute_test

# 저장 테스트는 빈 임시 디렉터리를 cwd로 실행해야 해요 (상대 경로 users.sans)
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . storage.c tests/test_storage.c -o storage_test
tmp=$(mktemp -d) && (cd "$tmp" && /absolute/path/to/storage_test)

# 통합 테스트: 소스가 있으면 자동 빌드, 없으면 DBMS_EXE 지정
python tests/test_integration.py -v
DBMS_EXE=/path/to/dbms python tests/test_integration.py -v
```

## 한계

- `run_tests.py`는 이 브랜치에서 `main.c`·`parser.c`·`execute.c`·`storage.c`와
  #7/#8 테스트가 없으므로 `INCOMPLETE`와 종료 코드 1을 내요. 이는 정상적인 누락
  보고이며 전체 통과가 아니에요.
- P01/P02, S01/S02는 기본 계약만 확인해요. 파서·저장 설계의 전체 T 표를 복사하지
  않아요. E01~E03은 #8의 `tests/test_execute.c`, CLI 대역은 #7 테스트가 맡아요.
- 통합 테스트는 최초 ID를 1로 가정하지 않고 실제 `INSERT OK id=N`과 조회 결과를
  사용해요. 128MiB 경계 테스트는 공백 파일을 만들기 때문에 시간과 디스크를 써요.
- 저장 테스트는 `users.sans`가 이미 있는 cwd에서 실행하면 데이터 보호를 위해
  바로 실패해요. 반드시 빈 임시 디렉터리에서 실행하세요.
