# 테스트 (이슈 #9)

실제 모듈을 쓰는 SQL 통합 테스트와 최소 파서·저장 단위 테스트예요.
전체 러너는 파서·저장·CLI·실행기와 각 모듈의 회귀 테스트도 함께 검증해요.
필수 구현이나 테스트가 빠졌다면 `run_tests.py`는 누락 항목을 보고하고 0이 아닌 코드로 끝나요.
가짜 구현이나 테스트 건너뛰기를 전체 통과로 처리하지 않아요.

`docs/test.md`의 SQL 통합 테스트는 실패 주입을 제외하지만, 전체 러너에는
CLI·실행기 등 모듈별 실패 주입 테스트도 포함돼요. `test_harness.py`는 DBMS
기능 검사와 별도로 테스트 도구 자체를 검사하는 mock 기반 보조 회귀 테스트예요.

## 파일

| 파일 | 범위 |
| --- | --- |
| `test_integration.py` | `docs/test.md` 01~18, CLI 인자·빈 입력·128MiB 경계 |
| `test_parser_contract.c` | P01 INSERT/WHERE SELECT, P02 문법 오류·커서·출력 초기화 계약 |
| `test_storage_contract.c` | S01 빈 스캔→2행 저장→재스캔, S02 close/reopen 후 1행 추가 |
| `test_harness.py` | timeout 실패 보고, 컴파일러 탐색, 오류 원인·한 줄 stderr 검사 회귀 |
| `test_server.py` | HTTP 서버(#21): 응답 JSON, 400·413, `Expect: 100-continue`, 동시 INSERT id 중복 없음, 강제 종료 뒤 재시작, 실행 인자 |
| `run_tests.py` | 헤더, 기존 파서·저장 회귀 테스트, P/S, CLI/execute, 통합 테스트 |

## 의존성

- Python 3.10+ 표준 라이브러리만 사용
- `gcc` (C11). `GCC`로 명령명이나 경로를 지정하고, 미지정 시 PATH의 `gcc`를 사용해요.
- 통합 테스트는 `DBMS_EXE`로 실제 실행 파일을 받거나, 저장소 루트의
  `main.c`·`parser.c`·`execute.c`·`storage.c`를 임시 디렉터리에서 빌드해요.
- 단위 테스트는 각각 `parser.c`, `storage.c`, `execute.c`가 필요해요.
- PR #13(CLI와 CLI 대역 테스트)·#14(실행기와 E01~E03)는 `main`에 병합되었고
  이 브랜치에도 반영됐어요. 병합 전 전체 러너가 누락 없이 통과해야 해요.
  해당 테스트를 중복 작성하거나 누락을 성공으로 처리하지 않아요.

통합 테스트의 오류 분류는 설계 문서에서 새로 확정한 이름이 아니라 PR #13의
`main.c` 구현과 연결하는 기대값이에요. CLI 계약이 바뀌면 같이 갱신해요.

| 상태 | 현재 CLI 분류 |
| --- | --- |
| `DB_ERR_SYNTAX` | `syntax` |
| `DB_ERR_SCHEMA` | `schema` |
| `DB_ERR_TYPE` | `type` |
| `DB_ERR_RANGE` | `range` |

영어 원인 문구 전체는 고정하지 않고 사례별 원인 단어를 검사해요.
현재 CLI의 schema 원인은 테이블·열·값 개수를 함께 나열하므로, 테스트 통과가
CLI의 상세 원인 구분까지 보장하지는 않아요.

전체 러너가 직접 수행하는 C 컴파일·실행과 SQL 통합 테스트 내부의 개별
컴파일·실행 제한은 300초예요. 전체 러너가 호출하는 Python 테스트 묶음은
각각 900초 제한이에요. CLI 대역 테스트 내부의 컴파일·실행에는 개별 제한이
없으므로, `test_cli.py`를 단독 실행하면 이 900초 제한도 적용되지 않아요.
timeout은 실패로 보고해요. Windows에서는 하위 프로세스 트리 전체 종료까지
보장하지 않아요.

## 명령

```bash
# 이 브랜치 전체: 누락 모듈이 있으면 INCOMPLETE + 종료 코드 1
python tests/run_tests.py

# 헤더 계약
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . -fsyntax-only tests/header_contract.c

# 새 P/S 테스트 문법 검사 (모듈 없이 가능)
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . -fsyntax-only tests/test_parser_contract.c
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . -fsyntax-only tests/test_storage_contract.c

# 실제 모듈이 있을 때
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . parser.c tests/test_parser_contract.c -o parser_test
./parser_test
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . execute.c tests/test_execute.c -o execute_test
./execute_test

# 저장 테스트는 빈 임시 디렉터리를 cwd로 실행해야 해요 (상대 경로 users.sans)
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I . storage.c tests/test_storage_contract.c -o storage_test
tmp=$(mktemp -d) && (cd "$tmp" && /absolute/path/to/storage_test)

# 서버 테스트: server.c와 모듈을 임시 디렉터리에서 -pthread로 빌드해 빈 포트에 띄워요
python tests/test_server.py -v

# 통합 테스트: 소스가 있으면 자동 빌드, 없으면 DBMS_EXE 지정
python tests/test_integration.py -v
DBMS_EXE=/path/to/dbms python tests/test_integration.py -v
```

## 한계

- `main.c`·`execute.c` 및 #7/#8 테스트가 없는 브랜치에서는 `run_tests.py`가
  `INCOMPLETE`와 종료 코드 1을 내요. 이는 누락 보고이며 전체 통과가 아니에요.
- P01/P02, S01/S02는 기본 계약만 확인해요. 파서·저장 설계의 전체 T 표를 복사하지
  않아요. E01~E03은 #8의 `tests/test_execute.c`, CLI 대역은 #7 테스트가 맡아요.
- 통합 테스트는 최초 ID를 1로 가정하지 않고 실제 `INSERT OK id=N`과 조회 결과를
  사용해요. 128MiB 경계 테스트는 공백 파일을 만들기 때문에 시간과 디스크를 써요.
- 저장 테스트는 `users.sans`가 이미 있는 cwd에서 실행하면 데이터 보호를 위해
  바로 실패해요. 반드시 빈 임시 디렉터리에서 실행하세요.

## 최초 검증 결과

Windows MinGW-w64와 Python 3.12에서 최신 `main`의 파서·저장 모듈로 검증했어요.

- 이 브랜치에서 `DBMS_EXE`를 실제 조합 실행 파일로 지정한 뒤
  `python tests/run_tests.py`: `6 passed, 0 failed, 2 missing`, 종료 코드 1.
  누락은 별도 #7/#8 브랜치의 CLI·실행기 단위 테스트예요.
- 임시 디렉터리에 이 브랜치와 #7/#8의 구현·테스트를 조합한 뒤
  `python <임시 디렉터리>/tests/run_tests.py`:
  `8 passed, 0 failed, 0 missing`, `FULL SUITE PASSED`, 종료 코드 0.
  헤더, 기존 회귀 테스트, P/S/E 단위 테스트, CLI 19개 및 실제 SQL 통합 25개를 포함해요.
- 브랜치 간 구현 코드는 이 브랜치에 병합하지 않았어요.
  Linux 전용 경로와 출력·flush 실패 주입은 이번 실행 범위가 아니에요.

## PR 리뷰 반영 후 검증

- PR #13의 `8a84f52`, PR #14의 `69a3624`와 이 변경을 별도 임시 소스 트리에
  조합해 `python <임시 디렉터리>/tests/run_tests.py`를 실행했어요.
- 결과: `9 passed, 0 failed, 0 missing`, `FULL SUITE PASSED`, 종료 코드 0.
  추가된 하니스 회귀 6개와 실제 SQL 통합 25개를 포함해요.
- 검증 환경은 Windows MinGW-w64 / Python 3.12예요. 다른 PR의 작업 폴더나
  미커밋 변경은 검증 입력으로 사용하지 않았어요.
