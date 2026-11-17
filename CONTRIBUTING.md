# 협업 규칙

## 이슈

- 이슈는 [작업](.github/ISSUE_TEMPLATE/task.yml), [버그](.github/ISSUE_TEMPLATE/bug.yml), [설정·문서 변경](.github/ISSUE_TEMPLATE/setting.yml) 템플릿으로 작성해요.
- 이슈 제목은 `[Phase 1] 렉서`처럼 Phase를 앞에 붙여요. 설정·문서 변경 이슈는 Phase를 붙이지 않아요.

## 브랜치와 커밋

- `main`에서 직접 작업하지 않아요. 브랜치 이름은 `GitHub사용자명/이슈번호`예요. (예: `octocat/12`)
- 커밋 메시지는 한국어로 작성하고 Conventional Commits를 적용해요. 너무 상세하게 쓰지 않고 흐름을 알 수 있을 정도로 적어요. 제목은 50자 안팎을 권장해요.

```text
feat: 렉서 토큰 분리 추가
```

| 타입 | 용도 |
|---|---|
| `feat` | 새 기능 |
| `fix` | 버그 수정 |
| `docs` | 문서 작성·수정 |
| `test` | 테스트 추가·수정 |
| `refactor` | 동작 변경 없는 구조 개선 |
| `perf` | 성능 개선, 성능 측정 |
| `chore` | 빌드·설정 등 기타 작업 |

## PR

- 이슈를 해결하면 [PR 템플릿](.github/PULL_REQUEST_TEMPLATE.md)에 맞춰 PR을 올리고 `closes #이슈번호`를 남겨요.
- 저장소 설정이나 문서만 바꾸는 PR은 [설정·문서 PR 템플릿](.github/PULL_REQUEST_TEMPLATE/setting.md)을 써요. PR 화면에는 템플릿 선택 메뉴가 없어서, 비교 주소 끝에 `?quick_pull=1&template=setting.md`를 붙여 열어요.
- PR 제목도 커밋 메시지 형식으로 써요.

## `main` 브랜치 보호

- PR을 통해서만 머지하고, 작성자를 제외한 1명 이상의 승인을 요구해요.
- 관리자는 이 규칙을 우회할 수 있어요.

설정 명령 (저장소 관리자가 한 번만 실행해요):

```bash
gh api -X PUT repos/NARi3040/KJG-SW-AI-W11-DBMS/branches/main/protection --input - <<'JSON'
{
  "required_status_checks": null,
  "enforce_admins": false,
  "required_pull_request_reviews": { "required_approving_review_count": 1 },
  "restrictions": null
}
JSON
```
