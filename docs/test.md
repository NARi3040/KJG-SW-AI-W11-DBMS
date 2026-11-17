# 테스트 세부 설계 (최무헌)

> **기준 문서:** [모듈 구성과 호출 계약](architecture.md), [파서](parser.md), [CLI](main.md), [실행기](execute.md) 세부 설계.
> SQL 파일로 핵심 기능을 확인하고, 모듈별 단위 테스트는 기본 동작만 남겨요. 아래 내용은 테스트 명세이며 실행 결과가 아니에요.

## 목차

1. [범위와 준비](#1-범위와-준비)
2. [SQL 파일 테스트](#2-sql-파일-테스트)
3. [최소 단위 테스트](#3-최소-단위-테스트)
4. [완료 기준과 남은 결정](#4-완료-기준과-남은-결정)

## 1. 범위와 준비

Phase 1의 삽입, 조회, WHERE, 잘못된 SQL 거부, 오류 후 중단과 정상 재시작 후 데이터 보존을 확인해요.

- 실제 `users.sans` 대신 테스트 전용 저장소를 사용해요. 고정 상대 경로를 사용한다면 임시 디렉터리를 작업 디렉터리로 실행해요.
- 빈 저장소는 저장 모듈의 생성 정책대로 준비해요. 0바이트 파일이 유효한 빈 저장소라고 가정하지 않아요.
- 각 SQL 코드블록을 별도 파일로 실행해요. 오류 사례를 합치면 첫 오류에서 중단돼요.
- 성공은 종료 코드 0과 조회 결과, 실패는 0이 아닌 종료 코드와 stderr의 오류로 판정해요. 출력 문구나 공백의 완전 일치는 요구하지 않아요.
- 자동 ID는 실제 INSERT 결과나 조회에서 확인해요. 최초 ID를 1로 고정하지 않아요.

출력, flush, close, 메모리 할당 실패 주입과 정교한 손상 fixture는 이번 테스트 범위에서 제외해요. 구현의 오류 처리 계약을 없애는 것은 아니며, 기본 기능이 연결된 뒤 보강해요.

## 2. SQL 파일 테스트

### 01.sql: 삽입과 조회

빈 저장소에서 시작해요. 첫 SELECT는 Mina 1행, 둘째는 Mina, Joon, Hana 3행이에요. 저장한 name과 age가 그대로 나오고 자동 ID는 서로 달라야 해요.

```sql
INSERT INTO users VALUES ('Mina', 20);
SELECT * FROM users;
INSERT INTO users VALUES ('Joon', 31);
INSERT INTO users VALUES ('Hana', 27);
SELECT * FROM users;
```

### 02.sql: 정상 재시작과 빈 조회

01 정상 종료 후 같은 저장소로 재실행하면 3행이 유지돼요. 별도의 유효한 빈 저장소에서는 성공과 0행이에요.

```sql
SELECT * FROM users;
```

재시작한 저장소에 별도 파일로 `INSERT INTO users VALUES ('Next', 25);`를 실행하고 다시 조회해요. 기존 3행이 유지되고 새 행의 ID가 기존 ID와 중복되지 않아야 해요.

### 03.sql: 문법 오류

조회 대상 `*`가 없어 문법 오류예요.

```sql
SELECT FROM users;
```

### 04.sql: 없는 테이블

스키마 오류예요. unknown_table에 해당하는 파일을 새로 만들면 안 돼요.

```sql
SELECT * FROM unknown_table;
```

### 05.sql: 값 개수 부족

age가 없어 실행기 검증에 실패해요. 행이 추가되면 안 돼요.

```sql
INSERT INTO users VALUES ('Mina');
```

### 06.sql: 값 개수 초과

3개 값은 파서 한도 안이지만 users 스키마와 달라 실행기 검증에 실패해요. 행이 추가되면 안 돼요.

```sql
INSERT INTO users VALUES ('Mina', 20, 99);
```

### 07.sql: name 타입 오류

name이 정수라 실행기 검증에 실패해요. 행이 추가되면 안 돼요.

```sql
INSERT INTO users VALUES (123, 20);
```

### 08.sql: age 타입 오류

age가 문자열이라 실행기 검증에 실패해요. 행이 추가되면 안 돼요.

```sql
INSERT INTO users VALUES ('Mina', 'twenty');
```

### 09.sql: 오류 후 중단

별도의 빈 저장소에서 실행해요. 문법 오류로 중단한 뒤 02로 조회하면 BeforeError만 있고 AfterError는 없어야 해요. 앞선 INSERT의 롤백은 요구하지 않아요.

```sql
INSERT INTO users VALUES ('BeforeError', 20);
SELECT FROM users;
INSERT INTO users VALUES ('AfterError', 30);
```

별도의 빈 저장소에서 가운데 문장을 `SELECT * FROM unknown_table;`로 바꿔 실행해요. 실행기 오류에서도 BeforeError만 남아야 해요.

### 10.sql: 미완성 문장

닫는 괄호 없이 끝나므로 문법 오류예요. 정상 입력 끝으로 처리하거나 행을 추가하면 안 돼요.

```sql
INSERT INTO users VALUES ('Mina', 20
```

### 11.sql: WHERE 일치와 불일치

01의 저장소에서 실행해요. 각각 Mina 1행, Joon 1행, 0행이고 모두 성공이에요.

```sql
SELECT * FROM users WHERE name = 'Mina';
SELECT * FROM users WHERE age = 31;
SELECT * FROM users WHERE name = 'NoMatch';
```

id 조건은 Mina의 실제 ID를 넣은 `SELECT * FROM users WHERE id = 정수;`를 별도 파일로 실행해요. `정수`는 자리 표시이며 그대로 실행하지 않아요. 결과는 Mina 1행이어야 해요.

### 12.sql: WHERE 열 오류

문법은 맞지만 없는 열이므로 실행기 검증에 실패해요.

```sql
SELECT * FROM users WHERE nickname = 'Mina';
```

### 13.sql: WHERE 타입 오류

id가 문자열이라 실행기 검증에 실패해요.

```sql
SELECT * FROM users WHERE id = '7';
```

03~08, 10, 12, 13은 각각 비정상 종료와 오류를 확인하고 재조회로 기존 데이터가 바뀌지 않았는지 비교해요.

SQL 외에는 인자 누락, 추가 인자, 없는 SQL 파일 경로의 비정상 종료와 빈 SQL 파일의 정상 종료만 확인해요.

## 3. 최소 단위 테스트

SQL 테스트가 실패했을 때 어느 모듈이 원인인지 찾을 수 있도록 기본 동작만 직접 확인해요.

| ID | 대상과 방법 | 기대 결과 |
| --- | --- | --- |
| P01 | `parse_next()`로 INSERT와 WHERE SELECT를 차례로 파싱해요 | 명령 종류, 테이블, 값과 조건이 맞고 커서가 전진해요. 마지막 호출은 `DB_END`예요 |
| P02 | 잘못된 SELECT와 미완성 INSERT를 각각 파싱해요 | `DB_ERR_SYNTAX`와 128바이트 버퍼 안에서 NUL 종료된 영어 한 줄 메시지예요 |
| E01 | 직접 만든 정상 INSERT 명령과 저장 함수 대역을 사용해요 | 값이 저장 함수에 한 번 전달되고 성공 ID와 위치가 반환돼요 |
| E02 | 없는 테이블, 잘못된 값 개수와 타입, 잘못된 WHERE를 각각 전달해요 | 검증 오류이며 저장 함수 호출은 0회예요 |
| E03 | 여러 행을 전달하는 저장 대역으로 WHERE SELECT를 실행해요 | 일치 행만 외부 콜백으로 전달하고 결과 개수도 일치해요. 일치 0개도 성공이에요 |
| S01 | 임시 빈 저장소를 스캔한 뒤 2행을 저장하고 다시 스캔해요 | 처음은 0행, 저장 후에는 두 행의 값이 같고 ID가 서로 달라요 |
| S02 | 정상 close 후 재open하고 한 행을 추가해요 | 기존 행이 유지되고 새 ID가 중복되지 않아요 |

저장 대역은 호출 횟수와 전달 값을 기록하고 필요한 행을 콜백으로 보내는 정도로만 만들어요. 콜백에서 비교할 행은 호출 중에 복사하고 원래 포인터를 보관하지 않아요. 상세 문법 사례는 [파서 설계의 테스트](parser.md#9-테스트)를 참고해요.

## 4. 완료 기준과 남은 결정

- [ ] 01~02로 삽입, 조회, 빈 조회와 재시작 후 데이터 및 ID 보존을 확인해요.
- [ ] 03~10으로 오류 거부와 이후 명령 중단을 확인해요.
- [ ] 11~13으로 WHERE의 일치, 불일치와 잘못된 조건 거부를 확인해요.
- [ ] 기본 CLI 인자 오류와 빈 SQL 입력을 확인해요.
- [ ] P01~P02, E01~E03, S01~S02를 단위 테스트로 확인해요.
- [ ] 실행한 명령과 결과를 남겨 통과 여부를 확인해요.

실행 파일명과 빌드 명령, 저장소 생성 방법은 구현에 맞춰 확정해요. name 길이와 age 범위가 정해지면 허용 경계와 초과 입력을 검증하는 사례를 추가해요.

실패 주입과 손상 복구는 이번 완료 기준에 포함하지 않아요. 정상 재시작 테스트가 전원 차단 안전성이나 다중 문장 원자성을 보장하는 것은 아니에요.
