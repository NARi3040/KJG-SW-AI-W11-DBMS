# SANS DBMS

C로 구현하는 바이너리 파일 기반 DBMS의 팀 설계 문서입니다. 데이터 파일 확장자는 `.sans`입니다.

## 설계 문서

- [모듈 구성과 호출 계약](docs/architecture.md) — 담당자, 함수 호출 관계, INSERT·SELECT 흐름, 공개 API와 자원 수명
- [파서 세부 설계](docs/parser.md) — 문장 경계, 토큰·문법 규칙, `Command` 구조, 오류 메시지, 테스트 기대값
- [저장 세부 설계](docs/storage.md) — `.sans` 바이트 형식, 자동 ID, 기록 순서와 복구, `Row` 구조, 테스트 기대값

공통 헤더 초안은 [`dbms.h`](dbms.h)에 있으며, 모듈 구현은 아직 포함하지 않습니다.
