# 저장 세부 설계 (`storage.c` · 노도현)

> **기준 문서:** [모듈 구성과 호출 계약](architecture.md)(큰 설계), [실행기](execute.md), [CLI](main.md), [테스트](test.md) 세부 설계.
> 이 문서는 그중 저장 담당 부분만 자세히 적어요. **범위는 Phase 1**이고, Phase 2·3은 지금 형식이 막지 않도록 대비만 해요([8장](#8-phase-23-대비)). 구현 전 설계예요.
> [2장](#2-큰-설계에-제안하는-변경)의 S-1~S-5는 **아직 합의 전**이에요. 받지 않는 항목은 문서에서 빼거나 고칠게요.

## 목차

1. [맡는 것과 맡지 않는 것](#1-맡는-것과-맡지-않는-것)
2. [큰 설계에 제안하는 변경](#2-큰-설계에-제안하는-변경)
3. [공개 타입과 함수](#3-공개-타입과-함수)
4. [파일 형식](#4-파일-형식)
5. [함수별 동작](#5-함수별-동작)
6. [반환값과 오류](#6-반환값과-오류)
7. [내부 구조](#7-내부-구조)
8. [Phase 2·3 대비](#8-phase-23-대비)
9. [테스트](#9-테스트)
10. [정하지 않은 것](#10-정하지-않은-것)

## 1. 맡는 것과 맡지 않는 것

| 맡는 것 | 맡지 않는 것 (담당) |
| --- | --- |
| `.sans` 파일 만들기, 열기, 형식 검사 | 저장 경로 정하기 (`main.c` · 최무헌) |
| 자동 ID 발급과 재시작 후 다음 ID 복원 | 테이블이 `users`인지, 값 개수·자료형 검사 (`execute.c` · 최무헌) |
| 행을 바이트로 바꿔 쓰기, 바이트를 행으로 되돌리기 | WHERE 조건 비교와 행 거르기 (`execute.c` · 최무헌) |
| 행마다 전달받은 콜백 호출, 성공 횟수 세기 | 행 출력 (`main.c` · 최무헌) |
| 기록 순서로 "어디까지 저장됐는지" 보장 | SQL 문자열 해석 (`parser.c` · 박준호) |

- 저장 모듈은 다른 모듈의 함수를 직접 호출하지 않아요. 전달받은 `on_row` 함수 포인터만 불러요.
- 저장 모듈은 아무것도 출력하지 않아요. 표준 출력·표준 오류 모두 쓰지 않아요.
- 실행기가 이미 검사한 값도 저장 직전에 한 번 더 범위를 검사해요(5.2절). 잘못된 값이 파일에 들어가면 되돌릴 수 없어서예요.

## 2. 큰 설계에 제안하는 변경

큰 설계 4.1절에서 "함께 정의"로 남은 타입을 저장 쪽에서 먼저 제안해요. **합의가 필요해요.**

| 번호 | 지금 큰 설계 | 제안 | 이유 | 영향받는 곳 |
| --- | --- | --- | --- | --- |
| S-1 | `Row`는 "조회한 `id`, `name`, `age`" | `name`을 포인터가 아니라 **고정 크기 배열**로 두고, `row_index`(레코드 위치)를 더해요 (3.1절) | 포인터가 없으면 "언제까지 유효한가"를 따질 곳이 하나 줄어요. `row_index`는 Phase 2에서 B+ 트리를 다시 만들 때 필요해서, 지금 넣어 두면 `Row`를 다시 바꾸지 않아도 돼요 | 최무헌 님: `print_row()`, 필터 콜백 |
| S-2 | 스키마 범위 미정 ([실행기 8장](execute.md#8-정하지-않은-것)) | `name`은 **최대 64바이트, 빈 문자열 허용**, `age`는 **`int32_t` 범위**, `id`는 1 이상 `int64_t` 범위예요. 상수 `USERS_NAME_MAX = 64`를 `dbms.h`에 둬요 | 레코드를 80바이트로 고정할 수 있어요. 100만 건이면 파일이 약 80MB예요. 파서 한도(255바이트)로 잡으면 약 272MB가 돼서 선형 탐색이 읽을 양이 3배 넘게 늘어요 | 최무헌 님: INSERT·WHERE 검증, 경계 테스트 |
| S-3 | 레코드 위치의 타입과 해석 미정 ([실행기 8장](execute.md#8-정하지-않은-것)) | `InsertResult = { int64_t id; uint64_t row_index; }`. 위치는 **0부터 센 레코드 번호**예요 | 바이트 오프셋보다 작고 뜻이 분명해요. [CLI 설계](main.md#7-정하지-않은-것)의 "바이트 오프셋이라고 출력하지 않는다"와도 맞아요. Phase 2 B+ 트리의 값으로 그대로 써요 | 최무헌 님: `ExecuteResult`, 결과 출력 |
| S-4 | `Storage`는 "내부 필드는 저장 모듈만 사용" | `dbms.h`에는 `typedef struct Storage Storage;` 선언만 두고, 필드는 `storage.c` 안에서 정의해요 | 다른 모듈이 필드를 실수로 건드릴 수 없어요. 필드를 바꿔도 다른 파일을 다시 고치지 않아도 돼요 | 없음 (포인터로만 쓰니까요) |
| S-5 | 빌드 환경 미정 | 저장 모듈은 POSIX의 `open`, `pread`, `pwrite`, `fstat`을 써요. **Linux(또는 WSL, Docker)에서 빌드**해요 | 파일 위치를 인자로 넘기므로 `Storage`에 공유 위치가 없어요. Phase 3에서 입출력을 다시 짜지 않아도 돼요(8.2절). INSERT마다 바로 쓰기 때문에 stdio 버퍼로 얻는 이득도 없어요 | 전원: 빌드 환경. Windows용 MinGW에서는 컴파일되지 않아요 |

- S-2에서 64바이트는 UTF-8 한글로 21글자예요. 더 길어야 하면 숫자만 바꾸면 되고, 레코드 크기는 4.2절 규칙대로 다시 계산해요.
- S-2의 빈 문자열 `''`은 저장 쪽에서는 문제가 없어요(길이 0). 실행기가 막기로 하면 그 정책을 따라요.
- 실행기가 이미 검사한 범위를 저장 모듈도 다시 검사해요(5.2절). 두 곳의 기준은 `dbms.h`의 같은 상수를 써요.
- 파서 P-3의 질문("id로 한 행 읽기를 둘지")에 대한 저장 쪽 의견은 8.2절에 있어요. 큰 설계 7장대로 Phase 2에서 확정해요.

## 3. 공개 타입과 함수

### 3.1 `dbms.h`에 들어갈 저장 쪽 정의

```c
#include <stddef.h>
#include <stdint.h>

#define USERS_NAME_MAX 64   // name 최대 바이트 수 (끝의 '\0' 제외)

typedef struct Storage Storage;   // 필드는 storage.c 안에만 있어요 (S-4)

typedef struct {
    int64_t  id;                       // 1부터 시작하는 자동 ID
    char     name[USERS_NAME_MAX + 1]; // '\0'으로 끝나요
    int32_t  age;
    uint64_t row_index;                // 0부터 센 레코드 위치 (S-1)
} Row;

typedef struct {
    int64_t  id;          // 발급한 ID
    uint64_t row_index;   // 저장한 레코드 위치 (S-3)
} InsertResult;
```

- `DbStatus`의 실제 이름은 공동으로 정해요. 이 문서는 아래 이름을 써요.

| 이름 | 뜻 | 큰 설계 4.1절의 분류 |
| --- | --- | --- |
| `DB_OK` | 성공 | 성공 |
| `DB_ERR_NOMEM` | `malloc` 실패 | 메모리 |
| `DB_ERR_RANGE` | 값이 저장 범위를 벗어남 | 범위 |
| `DB_ERR_IO` | 파일 열기·읽기·쓰기·닫기 실패 | 파일 |
| `DB_ERR_CORRUPT` | 파일 내용이 형식과 맞지 않음 | 손상 |
| `DB_ERR_WRITE_UNCERTAIN` | 행이 저장됐는지 알 수 없음 | 기록 결과 불확실 |

### 3.2 함수

큰 설계 4.2절의 선언을 그대로 써요.

```c
DbStatus storage_open(const char *path, Storage **out_storage);
DbStatus storage_insert(Storage *storage, const char *name,
                        int64_t age, InsertResult *out_result);
DbStatus storage_scan(Storage *storage, RowHandler on_row,
                      void *user_data, size_t *out_count);
DbStatus storage_close(Storage *storage);
```

| 인자 | 방향 | 내용 |
| --- | --- | --- |
| `path` | 입력 | `.sans` 파일 경로. 없으면 새로 만들어요. 디렉터리는 만들지 않아요 |
| `out_storage` | 출력 | 성공하면 열린 상태, 실패하면 `NULL` |
| `name` | 입력 | `'\0'`으로 끝나는 문자열. 최대 `USERS_NAME_MAX` + 1바이트까지만 읽어요. 보관하지 않고 복사해요 |
| `age` | 입력 | `int32_t` 범위여야 해요. 큰 설계 시그니처를 따라 `int64_t`로 받아요 |
| `out_result` | 출력 | `DB_OK`일 때만 의미 있어요. 그 밖에는 0으로 채워요 |
| `on_row`, `user_data` | 입력 | 행마다 `on_row(&row, user_data)`를 불러요 |
| `out_count` | 출력 | 시작할 때 0으로 두고, 콜백이 `DB_OK`를 돌려줄 때마다 1씩 늘려요. 오류가 나도 그때까지의 값이 남아요 |

- `path`, `out_storage`, `storage`, `name`, `out_result`, `on_row`, `out_count` 중 NULL이 있으면 `assert`로 멈춰요. 호출자의 버그라서 정상 경로로 다루지 않아요(파서와 같은 원칙).
- 예외: `storage_close(NULL)`은 아무것도 하지 않고 `DB_OK`를 돌려줘요. `free(NULL)`처럼 두면 `main`의 오류 경로가 단순해져요.
- 전역 변수와 `static` 버퍼를 쓰지 않아요. 모든 상태는 `Storage` 안에 있어요.

## 4. 파일 형식

파일 하나에 `users` 테이블 하나예요. 앞에 64바이트 헤더, 그 뒤에 80바이트 레코드가 이어져요.

```text
[헤더 64B][레코드 0 · 80B][레코드 1 · 80B] ... [레코드 row_count-1 · 80B]
```

- **모든 정수는 리틀 엔디언**이에요. 구조체를 통째로 `fwrite`하지 않고, 필드마다 바이트로 바꿔 써요. 구조체 사이 패딩과 컴퓨터마다 다른 바이트 순서가 파일에 섞이지 않게 하려고요.
- 부호 있는 정수(`id`, `age`)는 2의 보수 비트를 그대로 써요.

### 4.1 헤더 (64바이트)

| 위치 | 크기 | 필드 | 값 |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | 아스키 `SANS` (`53 41 4E 53`) |
| 4 | 2 | `version` | `1` |
| 6 | 2 | `header_size` | `64` |
| 8 | 4 | `record_size` | `80` |
| 12 | 4 | 예약 | `0` |
| 16 | 8 | `row_count` | 확정된 레코드 수 (`uint64`) |
| 24 | 8 | `next_id` | 다음에 줄 ID (`int64`) |
| 32 | 32 | 예약 | 모두 `0` |

- `row_count`가 **"저장됐다"의 기준**이에요. 레코드를 다 쓴 뒤 헤더의 `row_count`를 늘려야 그 행이 저장된 거예요(5.2절).
- DELETE가 없으므로 버전 1에서는 언제나 `next_id == row_count + 1`이에요. 열 때 이 관계를 검사해요(5.1절). DELETE를 넣게 되면 버전을 2로 올리고 이 검사를 바꿔요.
- 예약 바이트는 0이어야 해요. 0이 아니면 손상으로 봐요. 나중에 쓰게 되면 버전을 올려요.

### 4.2 레코드 (80바이트)

| 위치 | 크기 | 필드 | 값 |
| --- | --- | --- | --- |
| 0 | 8 | `id` | `int64`. 레코드 번호 + 1과 같아야 해요 |
| 8 | 4 | `age` | `int32` |
| 12 | 1 | `name_len` | `name`의 실제 바이트 수, 0~64 |
| 13 | 3 | 예약 | 모두 `0` |
| 16 | 64 | `name` | 앞 `name_len`바이트가 이름, 나머지는 모두 `0` |

- 레코드 `i`의 시작 위치: `64 + i × 80`.
- `name`에 길이를 따로 두는 이유: 64바이트를 꽉 채운 이름은 끝에 `'\0'`이 들어갈 자리가 없어요. 길이가 있으면 `'\0'`을 찾지 않아도 돼요.
- 이름 바이트는 그대로 복사해요. UTF-8 한글도 바이트 수로 세요(한 글자 3바이트). 인코딩을 검사하지는 않아요.
- `USERS_NAME_MAX`를 바꾸면 레코드 크기는 `16 + USERS_NAME_MAX`이고, 8의 배수가 되도록 맞춰요.

### 4.3 예: 빈 파일에 `('Kim Min', 23)` 하나를 저장한 뒤

파일 크기는 `64 + 80 × 1 = 144`바이트예요.

```text
위치  바이트                                            뜻
0000  53 41 4E 53 01 00 40 00 50 00 00 00 00 00 00 00   SANS, version 1, header 64, record 80, 예약
0010  01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00   row_count 1, next_id 2
0020  00 ... (32바이트 모두 0)                           예약
0040  01 00 00 00 00 00 00 00 17 00 00 00 07 00 00 00   id 1, age 23, name_len 7, 예약
0050  4B 69 6D 20 4D 69 6E 00 00 ... (57바이트 모두 0)   "Kim Min"과 나머지 0
```

## 5. 함수별 동작

### 5.1 `storage_open()`

1. `*out_storage = NULL`로 시작해요.
2. `open(path, O_RDWR)`로 열어요.
   - 파일이 없으면(`errno == ENOENT`) `open(path, O_RDWR | O_CREAT | O_EXCL, 0644)`로 새로 만들고, `row_count = 0`, `next_id = 1`인 헤더를 써요. 헤더 쓰기가 실패하면 파일을 닫고 `unlink()`로 지운 뒤 `DB_ERR_IO`예요.
   - `O_EXCL`은 그사이 다른 곳에서 같은 파일을 만들었으면 실패해요. 이미 있는 파일을 빈 파일로 덮어쓰는 일이 없어요.
   - 그 밖의 열기 실패(디렉터리 없음, 권한 없음)는 `DB_ERR_IO`예요.
3. `fstat()`으로 파일 크기를 재서 아래 표대로 처리해요.

| 파일 크기 | 처리 |
| --- | --- |
| 0바이트 | 새 파일처럼 헤더를 써요. 헤더를 쓰기 전에 프로그램이 죽은 경우예요 |
| 1~63바이트 | `DB_ERR_CORRUPT` |
| 64바이트 이상 | 헤더를 읽고 검사해요 (아래) |

4. 헤더 검사. 하나라도 틀리면 `DB_ERR_CORRUPT`예요.
   - `magic == "SANS"`, `version == 1`, `header_size == 64`, `record_size == 80`, 예약 바이트가 모두 0
   - `next_id == row_count + 1`
   - `64 + row_count × 80`이 `int64` 범위 안
   - 파일 크기 ≥ `64 + row_count × 80`
5. 파일 크기가 `64 + row_count × 80`보다 크면 **뒤쪽 남는 바이트는 무시**해요. 레코드를 쓰고 헤더를 고치기 전에 프로그램이 죽은 흔적이에요. 그 행은 저장되지 않은 것이고, 다음 INSERT가 그 자리를 덮어써요.
6. `Storage`를 `malloc`해서 `fd`, `row_count`, `next_id`를 채우고 `*out_storage`에 넣어요. `malloc` 실패는 `DB_ERR_NOMEM`이에요.

- 2~6 중 어디서 실패해도 연 파일은 닫고 할당한 메모리는 풀어요.
- 레코드 내용은 열 때 검사하지 않아요. 100만 건이면 열 때마다 80MB를 읽게 돼서요. 레코드 검사는 `storage_scan()`이 읽으면서 해요.

### 5.2 `storage_insert()`

순서가 곧 안전장치예요. **레코드를 먼저 쓰고, 헤더를 나중에 써요.**

| 단계 | 하는 일 | 실패하면 | 그때 파일 상태 |
| --- | --- | --- | --- |
| 1 | `out_result`를 0으로 채워요. `broken`이면 바로 `DB_ERR_WRITE_UNCERTAIN` | — | 변화 없음 |
| 2 | 범위 검사: `strnlen(name, 65) ≤ 64`, `INT32_MIN ≤ age ≤ INT32_MAX`, `next_id < INT64_MAX` | `DB_ERR_RANGE` | 변화 없음 |
| 3 | `id = next_id`, `row_index = row_count`로 레코드 80바이트를 만들어요 | — | 변화 없음 |
| 4 | `pwrite`로 `64 + row_index × 80` 위치에 레코드 80바이트를 써요 | `DB_ERR_IO` | 헤더가 그대로라 **저장되지 않은 것**이 확실해요. 다음 INSERT가 같은 자리를 덮어써요 |
| 5 | `pwrite`로 16번 위치에 새 `row_count`, `next_id` 16바이트를 써요 | `DB_ERR_WRITE_UNCERTAIN` | 헤더가 바뀌었는지 알 수 없어요. `broken = 1` |
| 6 | 메모리의 `row_count`, `next_id`를 1씩 늘리고 `out_result`를 채워요 | — | 저장 완료 |

- 4단계 실패가 확실한 실패인 이유: 파일을 다시 열면 헤더의 `row_count`만큼만 읽으니까, 반쯤 쓴 레코드는 없는 것과 같아요.
- 5단계 실패 뒤에는 메모리의 `row_count`가 파일과 같은지 알 수 없어요. 그래서 `broken`을 켜고, 이후 `storage_insert()`와 `storage_scan()`은 파일을 건드리지 않고 `DB_ERR_WRITE_UNCERTAIN`을 돌려줘요. 할 수 있는 일은 `storage_close()`뿐이에요. 다시 열면 헤더에 적힌 대로 정리돼요.
- 큰 설계 6장 규칙 7과 맞춰, `DB_ERR_WRITE_UNCERTAIN`에서 저장 모듈은 다시 시도하지 않아요.
- `pwrite`는 요청보다 적게 쓰고 돌아올 수 있어요. 다 쓸 때까지 남은 부분을 이어서 쓰고, `EINTR`이면 다시 시도해요(7장 `write_full_at`). 그래도 다 못 쓰면 그 단계의 실패예요.
- `fsync`는 하지 않아요. `pwrite`가 끝나면 데이터는 운영체제에 넘어가서 **프로그램이 죽어도** 남아요. **전원이 꺼지는 경우**는 보장하지 않아요. 그 경우 운영체제가 레코드와 헤더를 디스크에 쓰는 순서도 보장되지 않아요.
- 4단계에서 파일 끝에 붙이는 `O_APPEND`를 쓰지 않는 이유: 5.1절의 "남는 바이트"가 있으면 그 뒤에 붙어 버려요. 위치를 계산해서 쓰면 남는 바이트를 덮어써요.

### 5.3 `storage_scan()`

1. `*out_count = 0`. `broken`이면 `DB_ERR_WRITE_UNCERTAIN`.
2. 시작할 때의 `row_count`만큼만 읽어요.
3. 64번 위치부터 레코드를 **128개씩** 묶어 `pread`로 읽어요(10,240바이트, 스택 버퍼). 읽을 위치는 지역 변수로 들고 다녀요.
4. 레코드마다 검사하고 `Row`로 바꿔요. 아래 중 하나라도 틀리면 `DB_ERR_CORRUPT`로 멈춰요.
   - `id == row_index + 1`
   - `name_len ≤ 64`
   - 예약 바이트와 `name`의 `name_len` 이후 바이트가 모두 0
   - `name`의 앞 `name_len`바이트 안에 `0` 바이트가 없음 (`Row.name`이 C 문자열이라서)
5. `on_row(&row, user_data)`를 불러요. `DB_OK`면 `*out_count`를 1 늘리고 다음 행으로, 아니면 **그 값을 그대로 돌려주고** 멈춰요.
6. 끝까지 읽으면 `DB_OK`예요. 빈 테이블은 콜백 없이 `DB_OK`, `*out_count = 0`이에요.

- `Row`는 `storage_scan()`의 지역 변수예요. 큰 설계 5장대로 콜백 안에서만 유효해요. 보관하려면 콜백이 복사해요.
- 읽기 결과: `pread`가 `-1`이면(`EINTR` 제외) `DB_ERR_IO`, `0`(파일 끝)을 돌려줘서 128개를 다 못 채우면 파일이 중간에 줄어든 것이라 `DB_ERR_CORRUPT`예요. 적게 읽힌 경우는 이어서 읽어요(7장 `read_full_at`).
- **콜백 안에서 같은 `Storage`로 저장 함수를 다시 부르지 않는다**가 계약이에요. 읽을 위치는 지역 변수라 파일 위치가 꼬이지는 않지만, Phase 3에서 잠금을 넣으면 같은 스레드가 잠금을 두 번 잡게 돼요. 별도 표시로 검사하지 않아요. 표시가 `Storage`에 하나뿐이면 Phase 3의 정상적인 동시 scan을 잘못 잡아요.

### 5.4 `storage_close()`

1. `storage`가 NULL이면 `DB_OK`.
2. `close(fd)`. 실패하면 결과는 `DB_ERR_IO`지만 **정리는 끝까지** 해요. `EINTR`이어도 다시 닫지 않아요(Linux에서는 이미 닫힌 상태라 두 번 닫으면 다른 파일을 닫을 수 있어요).
3. `free(storage)`. 이후 포인터는 무효예요.

- 헤더는 INSERT마다 이미 고쳐 쓰므로 닫을 때 따로 쓰지 않아요.
- `broken`이어도 똑같이 닫아요.

## 6. 반환값과 오류

| 함수 | 상황 | 반환값 | 파일 | 출력 인자 |
| --- | --- | --- | --- | --- |
| `storage_open()` | 성공 (기존 파일, 새 파일, 0바이트 파일) | `DB_OK` | 새 파일이면 헤더 64바이트 | 열린 상태 |
| | 디렉터리 없음, 권한 없음, 읽기 실패 | `DB_ERR_IO` | 변화 없음 | `NULL` |
| | `malloc` 실패 | `DB_ERR_NOMEM` | 새 파일이면 헤더만 있음 | `NULL` |
| | 크기 1~63바이트, 헤더 검사 실패, 파일이 `row_count`보다 짧음 | `DB_ERR_CORRUPT` | 변화 없음 | `NULL` |
| `storage_insert()` | 성공 | `DB_OK` | 레코드 1개 추가, 헤더 갱신 | ID와 위치 |
| | `name`이 64바이트 초과, `age`가 `int32` 밖, ID 소진 | `DB_ERR_RANGE` | 변화 없음 | 0 |
| | 레코드 쓰기 실패 | `DB_ERR_IO` | 저장 안 됨이 확실 | 0 |
| | 헤더 쓰기 실패, 또는 이미 `broken` | `DB_ERR_WRITE_UNCERTAIN` | 알 수 없음 | 0 |
| `storage_scan()` | 끝까지 읽음 (빈 테이블 포함) | `DB_OK` | 변화 없음 | 콜백 성공 횟수 |
| | 읽기 실패 | `DB_ERR_IO` | 변화 없음 | 그때까지 성공 횟수 |
| | 레코드 검사 실패, 파일이 중간에 줄어듦 | `DB_ERR_CORRUPT` | 변화 없음 | 그때까지 성공 횟수 |
| | 콜백이 `DB_OK`가 아닌 값을 돌려줌 | 콜백의 반환값 | 변화 없음 | 그때까지 성공 횟수 |
| | 이미 `broken` | `DB_ERR_WRITE_UNCERTAIN` | 변화 없음 | 0 |
| `storage_close()` | 성공, 또는 `NULL` | `DB_OK` | 닫힘 | — |
| | `close` 실패 | `DB_ERR_IO` | 닫힘 (정리는 끝까지) | — |

- 저장 모듈은 오류 메시지 문자열을 만들지 않아요. `DbStatus`만 돌려주고, 메시지는 `main`이 상태값을 보고 정해요.

## 7. 내부 구조

`storage.c` 밖으로 내보내지 않는 정의예요. 이름은 바뀔 수 있어요.

```c
#define _FILE_OFFSET_BITS 64        // off_t를 64비트로 (2GB가 넘는 파일)
#define _POSIX_C_SOURCE   200809L   // pread, pwrite, strnlen

#include <fcntl.h>      // open
#include <sys/stat.h>   // fstat
#include <unistd.h>     // pread, pwrite, close, unlink

#define SANS_MAGIC        "SANS"
#define SANS_VERSION      1
#define SANS_HEADER_SIZE  64
#define SANS_RECORD_SIZE  80
#define SCAN_BATCH        128

struct Storage {
    int      fd;          // 파일 위치를 공유하지 않아요. 읽고 쓸 위치는 매번 인자로 넘겨요
    uint64_t row_count;   // 확정된 레코드 수. 파일 헤더와 같아요
    int64_t  next_id;     // 다음에 줄 ID
    int      broken;      // 헤더 쓰기 결과를 모를 때 1
};

static void     put_u16le(unsigned char *p, uint16_t v);
static void     put_u32le(unsigned char *p, uint32_t v);
static void     put_u64le(unsigned char *p, uint64_t v);
static uint16_t get_u16le(const unsigned char *p);
static uint32_t get_u32le(const unsigned char *p);
static uint64_t get_u64le(const unsigned char *p);

static off_t    record_offset(uint64_t row_index);   // 64 + row_index * 80
static void     encode_header(unsigned char out[SANS_HEADER_SIZE],
                              uint64_t row_count, int64_t next_id);
static DbStatus decode_header(const unsigned char in[SANS_HEADER_SIZE],
                              uint64_t *row_count, int64_t *next_id);
static void     encode_record(unsigned char out[SANS_RECORD_SIZE], int64_t id,
                              const char *name, size_t name_len, int32_t age);
static DbStatus decode_record(const unsigned char in[SANS_RECORD_SIZE],
                              uint64_t row_index, Row *out);
// len바이트를 다 쓸 때까지 pwrite를 반복해요. EINTR이면 다시 시도. 성공 1, 실패 0
static int      write_full_at(int fd, off_t offset, const void *buf, size_t len);
// len바이트를 다 읽을 때까지 pread를 반복해요. 읽은 바이트 수, 오류면 -1 (파일 끝이면 len보다 작음)
static ssize_t  read_full_at(int fd, off_t offset, void *buf, size_t len);
```

- 바이트 변환(`put_*`, `get_*`, `encode_*`, `decode_*`)은 파일을 건드리지 않는 순수 함수예요. 파일 없이 단위 테스트할 수 있어요.
- 위치 계산은 `off_t`로 해요. `64 + row_index × 80`을 계산하기 전에 5.1절의 범위 검사로 넘침이 없음을 보장해요.
- `FILE *`과 stdio를 쓰지 않아요(S-5). 저장 모듈에는 버퍼가 없으므로 닫을 때 비울 것도 없어요.

## 8. Phase 2·3 대비

이 장은 **지금 구현하지 않고, 확정하지도 않아요.** 큰 설계 7장대로 B+ 트리 API와 잠금 구조는 각 단계에서 정해요. 여기에는 지금 형식이 다음 단계를 막지 않는다는 근거와, 그때 낼 저장 쪽 의견만 적어요.

### 8.1 Phase 1에서 이미 해 둔 대비

| 대비 | 어디서 | 막아 주는 것 |
| --- | --- | --- |
| 고정 길이 레코드 | 4.2절 | 위치만 알면 한 번에 그 행을 읽을 수 있어요. Phase 2에서 인덱스가 찾은 행만 바로 읽어요 |
| `Row.row_index` | S-1 | Phase 2 시작 때 `storage_scan()` 한 번으로 `id → row_index` 인덱스를 다시 만들 수 있어요. `Row`를 다시 바꾸지 않아요 |
| `InsertResult.row_index` | S-3 | INSERT 직후 새 행을 인덱스에 넣을 값이 바로 있어요 |
| 전역·`static` 상태 없음 | 3.2절 | Phase 3에서 잠금을 `Storage` 안에만 넣으면 돼요 |
| `pread`·`pwrite`, 공유 파일 위치 없음 | S-5, 7장 | Phase 3에서 SELECT 여러 개가 동시에 읽어도 서로의 위치를 옮기지 않아요. 입출력 코드를 다시 짜지 않아요 |
| 재진입 금지를 계약으로만 둠 | 5.3절 | Phase 3의 정상적인 동시 scan을 오류로 잡는 표시가 없어요 |

### 8.2 Phase 2에서 낼 의견: 위치로 한 행 읽기

파서 P-3의 질문("id로 한 행 읽기를 둘지")에 대한 저장 쪽 의견이에요. 시그니처는 Phase 2에서 확정해요.

```c
// 저장 쪽 제안, 합의 전
DbStatus storage_fetch(Storage *storage, uint64_t row_index,
                       RowHandler on_row, void *user_data);
```

- **id가 아니라 `row_index`를 받아요.** 버전 1에서는 `id == row_index + 1`이라 계산으로도 찾을 수 있지만, 그러면 B+ 트리를 쓸 이유가 없어져요. "id로 찾기"는 인덱스가, "위치로 읽기"는 저장 모듈이 맡아요.
- 결과를 콜백으로 넘기면 `storage_scan()`과 같은 계약(행은 콜백 안에서만 유효)을 그대로 쓰고, [실행기의 필터 콜백](execute.md#53-where-있는-경로)도 재사용돼요.
- 인덱스 갱신 담당(실행기인지 저장 모듈인지)은 큰 설계 7장대로 Phase 2에서 정해요.

### 8.3 Phase 3에서 낼 의견: 여러 스레드

잠금 종류와 위치는 Phase 3에서 정해요. 지금 `Storage`는 **스레드 안전하지 않아요.** 그때 저장 쪽에서 지켜야 할 조건만 적어 둬요.

- INSERT의 "ID 발급 → 레코드 쓰기 → 헤더 쓰기"는 잠금 하나 안에서 함께 해야 해요. 나뉘면 같은 ID가 두 번 나와요.
- `broken`과 `row_count`도 같은 잠금 안에서 읽고 써요.
- SELECT 콜백은 잠금을 잡은 채로 실행돼요. 콜백이 네트워크로 느리게 보내면 그동안 INSERT가 기다려요. 콜백을 짧게 둘지, 행을 복사해 잠금 밖에서 보낼지는 그때 정해요.

## 9. 테스트

`tests/storage_test.c`에서 저장 함수를 직접 불러 확인해요. `storage.c`만 링크하고, 파일은 테스트마다 임시 경로에 새로 만들어요. 콜백은 받은 행을 배열에 복사해 두는 테스트용 함수를 써요.

| 번호 | 준비와 호출 | 기대 결과 |
| --- | --- | --- |
| T-1 | 없는 경로로 `storage_open()` | `DB_OK`, 파일 크기 64, 4.3절의 헤더에서 `row_count 0`, `next_id 1` |
| T-2 | T-1 뒤 `storage_scan()` | `DB_OK`, 콜백 0번, `count = 0` |
| T-3 | `('Kim Min', 23)` 저장 | `DB_OK`, `id = 1`, `row_index = 0`, 파일 144바이트가 4.3절과 바이트 단위로 같음 |
| T-4 | 3건 저장 후 `storage_scan()` | `count = 3`, 저장 순서대로 `id` 1·2·3, 이름과 나이가 같음 |
| T-5 | 3건 저장 → 닫기 → 다시 열기 → 1건 저장 | 새 행 `id = 4`, `row_index = 3`, 파일 크기 `64 + 80 × 4` |
| T-6 | 이름 64바이트 | `DB_OK`, 다시 읽은 `strlen(name) = 64` |
| T-7 | 이름 65바이트 | `DB_ERR_RANGE`, 파일 크기와 내용 변화 없음 |
| T-8 | 이름 `'김민수'` (9바이트) | 다시 읽은 바이트가 같음 |
| T-9 | 이름 `''` | `DB_OK`, 다시 읽으면 빈 문자열 |
| T-10 | `age = INT32_MIN`, `INT32_MAX` | 둘 다 `DB_OK`, 값 그대로 |
| T-11 | `age = INT32_MAX + 1` | `DB_ERR_RANGE`, 파일 변화 없음 |
| T-12 | 5건 저장, 콜백이 3번째 행에서 오류 반환 | 그 오류 그대로, `count = 2` |
| T-13 | 0바이트 파일로 열기 | `DB_OK`, 파일 크기 64 |
| T-14 | 10바이트 파일로 열기 | `DB_ERR_CORRUPT`, `out_storage = NULL` |
| T-15 | `magic`을 `XANS`로 바꾼 파일 | `DB_ERR_CORRUPT` |
| T-16 | `next_id`를 `row_count + 2`로 바꾼 파일 | `DB_ERR_CORRUPT` |
| T-17 | 3건 저장 후 파일을 `64 + 80 × 2`바이트로 자름 | 열기에서 `DB_ERR_CORRUPT` |
| T-18 | 2건 저장 후 파일 끝에 쓰레기 30바이트 추가 | 열기 `DB_OK`, scan `count = 2`, 1건 저장 후 `id = 3`이고 다시 열어 scan하면 3건 |
| T-19 | 3건 저장 후 2번째 레코드의 `id`를 99로 바꿈 | scan이 `DB_ERR_CORRUPT`, `count = 1` |
| T-20 | 3건 저장 후 1번째 레코드의 `name_len`을 65로 바꿈 | scan이 `DB_ERR_CORRUPT`, `count = 0` |
| T-21 | 없는 디렉터리 안의 경로로 열기 | `DB_ERR_IO`, `out_storage = NULL` |
| T-22 | `storage_close(NULL)` | `DB_OK` |
| T-23 | 바이트 변환 함수만: `put_u64le(-1)` 후 `get_u64le` 등 | 원래 값과 같음, 바이트 순서가 리틀 엔디언 |

- 헤더 쓰기 실패(`DB_ERR_WRITE_UNCERTAIN`)는 일반 파일로 일부러 만들기 어려워요. 단위 테스트에서는 다루지 않고 코드 리뷰로 확인해요(10장). [테스트 설계](test.md#1-범위와-준비)도 실패 주입을 이번 범위에서 뺐어요.
- [테스트 설계 3장](test.md#3-최소-단위-테스트)의 S01은 T-2·T-4, S02는 T-5와 같은 내용이에요. 이 표가 기대값을 더 자세히 적은 것이라, 구현할 때는 한 파일에 합쳐요.
- SQL 파일 테스트(01~13)는 최무헌 님 담당이에요. 그쪽에서 이 표의 동작에 기대는 곳은 02.sql(재시작 후 데이터와 ID 보존)이에요.
- T-3의 "최초 ID 1"은 저장 모듈 단위 테스트에서만 확인해요. SQL 파일 테스트는 최초 ID를 1로 가정하지 않아요(test.md 1장).

## 10. 정하지 않은 것

| 항목 | 내용 | 같이 정할 사람 |
| --- | --- | --- |
| S-1~S-5 수용 여부 | `Row`, 스키마 범위, `InsertResult`, `Storage` 숨기기, POSIX 빌드 환경. S-2·S-3은 [실행기 8장](execute.md#8-정하지-않은-것)의 스키마·레코드 위치 항목에 대한 답이에요 | 전원 |
| `DbStatus` 정의 | 3.1절 표의 저장 쪽 오류 이름과 enum 수치 값 | 전원 |
| 저장 경로 | `main`이 넘길 `users.sans`의 기준 경로와, 그 디렉터리를 누가 만드는지. 저장 모듈은 디렉터리를 만들지 않아요([CLI 7장](main.md#7-정하지-않은-것)과 같은 항목) | 최무헌, 노도현 |
| 상태별 오류 메시지 | `DB_ERR_CORRUPT` 등을 `main`이 어떤 영어 한 줄로 보여 줄지 | 최무헌 |
| 헤더 쓰기 실패 테스트 | 실패를 흉내 내는 테스트용 장치를 둘지. 지금은 범위 밖이에요 | 최무헌 |
| 100만 건 삽입 속도 | INSERT마다 쓰기 시스템 호출이 두 번이에요. 설계대로 짠 시제품으로 재 보니 100만 건 저장 4.2초, 전체 scan 0.05초였어요(Windows, MSYS2 gcc 16.1 `-O2`, `pread`·`pwrite` 대신 같은 동작의 대체 함수, 한 번 측정). Phase 2의 Linux 측정에서 너무 느리면 "닫을 때만 헤더 쓰기" 같은 방식을 다시 정해요 | 전원 |
| Phase 2·3 계약 | `storage_fetch()`, 인덱스 갱신 담당, 잠금 구조. 큰 설계 7장대로 각 단계에서 정해요. 저장 쪽 의견은 8.2·8.3절이에요 | 전원 |
