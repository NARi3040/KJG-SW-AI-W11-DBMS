# SANS DBMS

C로 구현하는 바이너리 파일 기반 DBMS의 팀 설계 문서입니다. 데이터 파일 확장자는 `.sans`입니다.

## 설계 문서

- [모듈 구성과 호출 계약](docs/architecture.md) — 담당자, 함수 호출 관계, INSERT·SELECT 흐름, 공개 API와 자원 수명
- [파서 세부 설계](docs/parser.md) — 문장 경계, 토큰·문법 규칙, `Command` 구조, 오류 메시지, 테스트 기대값
- [저장 세부 설계](docs/storage.md) — `.sans` 바이트 형식, 자동 ID, 기록 순서와 복구, `Row` 구조, 테스트 기대값
- [Phase 2·3 계획](docs/phase2-3.md) — B+ 트리 인덱스와 API 서버의 회의 결정, 일정, 계약, 완료 기준

## 빌드와 테스트

```bash
make test   # 파서 단위 테스트 (gcc 또는 MinGW-w64, C11)
```

공통 인터페이스는 [`dbms.h`](dbms.h)에 있습니다. 파서(`parser.c`)·저장소(`storage.c`)·CLI(`main.c`)·실행기(`execute.c`)와 전체 테스트 러너가 구현되어 있습니다.

다음 명령으로 빌드하고 전체 테스트를 실행합니다.

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic main.c parser.c execute.c storage.c -o dbms
python tests/run_tests.py
```

SQL 파일은 `./dbms input.sql`로 실행합니다(Windows에서는 `.\dbms.exe input.sql`).

API 서버는 `127.0.0.1`에서 `POST /query`로 SQL을 받아 JSON으로 돌려줍니다. 실행 인자와 응답 형식은 [Phase 2·3 계획 4장](docs/phase2-3.md#4-phase-3-api-서버)에 있습니다.

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic -pthread server.c parser.c execute.c storage.c -o dbms_server
# Windows MinGW-w64: 끝에 -static -lws2_32
./dbms_server --port 8080 --threads 4
curl -s -X POST --data-binary "INSERT INTO users VALUES ('kim', 23); SELECT * FROM users;" http://127.0.0.1:8080/query
```

전체 테스트의 범위와 의존성은 [`tests/README.md`](tests/README.md)에서 안내합니다. `make test`는 전체 테스트가 아니라 파서 단위 테스트만 실행합니다.
