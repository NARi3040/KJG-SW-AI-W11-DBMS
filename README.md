# 미니 DBMS

C로 직접 만드는 미니 DBMS예요. 세 단계로 만들어요.

1. **Phase 1. SQL 처리기**: `.sql` 파일을 CLI로 받아 `INSERT`, `SELECT`를 파싱하고 테이블 파일에 저장·조회해요.
2. **Phase 2. B+ 트리 인덱스**: id를 자동으로 부여해 B+ 트리에 등록하고, 100만 건에서 인덱스 조회와 선형 탐색의 속도를 비교해요.
3. **Phase 3. API 서버**: Phase 1·2를 내부 엔진으로 쓰고, Thread Pool로 외부 클라이언트의 SQL 요청을 병렬로 처리해요.

현재 상태: Phase 1 시작 전이에요. 빌드·테스트 뼈대만 있어요.

## 실행하기

C 컴파일러(gcc 또는 clang)와 `make`가 필요해요.

```bash
make                    # ./minidb 빌드
./minidb query.sql      # SQL 파일 실행 (Phase 1에서 구현)
make test               # 단위 테스트와 SQL 기능 테스트
```

## 문서

| 문서 | 내용 |
|---|---|
| [docs/PLANNING.md](docs/PLANNING.md) | 과제 요구사항과 요구사항 ID |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 협업 규칙과 AI 리뷰 |
| [docs/GITHUB_GUIDE.md](docs/GITHUB_GUIDE.md) | 이슈부터 머지까지 단계별 명령어 |
| [docs/technical-writing-principles.md](docs/technical-writing-principles.md) | 문서 작성 원칙 |
| [AGENTS.md](AGENTS.md) | AI 도구가 따를 규칙 |

## 팀

| [NARi3040](https://github.com/NARi3040) | [dohyunrho](https://github.com/dohyunrho) | [choimoohun](https://github.com/choimoohun) |
|:---:|:---:|:---:|
| <img src="https://github.com/NARi3040.png?size=150" width="100" alt="NARi3040"> | <img src="https://github.com/dohyunrho.png?size=150" width="100" alt="dohyunrho"> | <img src="https://github.com/choimoohun.png?size=150" width="100" alt="choimoohun"> |
