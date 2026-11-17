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

### AI 리뷰

- 포크에서 온 PR을 뺀 모든 PR은 Claude가 리뷰하고, 리뷰 봇(`giga-deepseek[bot]`)이 결과를 PR 리뷰로 남겨요. Draft PR은 Ready for review로 바꿀 때 리뷰해요.
- 문서는 구현자가 추측해야 하는 곳을, 코드는 `docs/` 설계 문서와의 일치, 버그와 엣지 케이스 누락, 테스트 없는 동작 변경을 봐요.
- 코드도 Anthropic API로 전송돼요. 비밀 값을 커밋하지 않아요.
- 의견마다 심각도(높음, 보통, 낮음)를 붙여요. 읽어서 확정하지 못한 의심은 인라인으로 달지 않고 리뷰 본문의 "확인이 필요한 의견"에 접어 둬요.
- PR을 열면 전체를 리뷰하고, push하면 마지막 리뷰 이후 바뀐 부분만 리뷰해요. 이전 리뷰의 스레드 중 이번 push로 반영된 것은 봇이 답글을 달고 닫아요.
- 전체를 다시 리뷰받으려면 PR 댓글에 `@giga-deepseek review`를 적어요. PR에 `giga-deepseek` 라벨을 붙여도 돼요(라벨은 [초기 설정](#리뷰-봇-초기-설정)에서 만들어요).
- PR 댓글이나 리뷰 코멘트 답글에 `@giga-deepseek`와 질문을 적으면 봇이 답글을 달아요. 저장소 멤버의 댓글에만 답하고, 파일을 고치거나 스레드를 닫지는 않아요.
- 리뷰는 [`NARi3040/docs-review`](https://github.com/NARi3040/docs-review) 액션으로 실행해요(비공개 저장소). 이 저장소에 맞춘 맥락은 [`.github/docs-review.md`](.github/docs-review.md)에서, 실행 설정은 [`.github/workflows/docs-review.yml`](.github/workflows/docs-review.yml)과 멘션 답글용 [`.github/workflows/docs-review-reply.yml`](.github/workflows/docs-review-reply.yml)에서 고쳐요.
- Claude의 코멘트는 참고용이에요. 반영 여부는 작성자와 리뷰어가 정하고, 사람 1명의 승인은 그대로 필요해요.

### CodeRabbit 리뷰

- 모든 PR은 [CodeRabbit](https://www.coderabbit.ai)이 한국어로 자동 리뷰해요. Draft PR은 Ready for review로 바꿀 때 리뷰해요.
- 커밋을 push하면 바뀐 부분만 다시 리뷰해요. 커밋 5개를 리뷰하면 자동 리뷰가 멈추므로, 그 뒤에는 PR 코멘트에 `@coderabbitai review`를 남겨 직접 요청해요.
- 리뷰 기준은 [`.coderabbit.yaml`](.coderabbit.yaml)이에요. 기준을 바꾸려면 이 파일을 고쳐요.
- CodeRabbit의 코멘트도 참고용이에요. 사람 1명의 승인은 그대로 필요해요.

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

## 리뷰 봇 초기 설정

저장소 관리자가 한 번만 해요.

### CodeRabbit

[CodeRabbit GitHub App](https://github.com/apps/coderabbitai)을 이 저장소에 설치해요. 공개 저장소라 무료예요. 설정은 저장소의 `.coderabbit.yaml`을 읽어요.

### AI 리뷰

1. Anthropic API 키를 저장소 시크릿으로 등록해요. 명령을 실행하면 키를 입력하라고 나와요.

   ```bash
   gh secret set ANTHROPIC_API_KEY -R NARi3040/KJG-SW-AI-W11-DBMS
   ```

2. 리뷰 봇 GitHub App(`giga-deepseek`)을 이 저장소에도 설치해요. [Installed GitHub Apps](https://github.com/settings/installations)에서 Configure → Repository access에 이 저장소를 더해요.
3. private key 시크릿을 먼저 등록하고, 그다음 Client ID 변수를 등록해요. 워크플로는 Client ID 변수가 없으면 리뷰 작업을 건너뛰어요. 변수만 있고 시크릿이 없으면 작업이 실패해요.

   ```bash
   gh secret set DOCS_REVIEW_APP_PRIVATE_KEY -R NARi3040/KJG-SW-AI-W11-DBMS < 키파일.pem
   gh variable set DOCS_REVIEW_APP_CLIENT_ID -R NARi3040/KJG-SW-AI-W11-DBMS --body "Iv23..."
   ```

4. 전체 재리뷰용 PR 라벨을 만들어요.

   ```bash
   gh label create giga-deepseek -R NARi3040/KJG-SW-AI-W11-DBMS -c 5319E7 -d "AI 리뷰를 처음부터 다시 받아요"
   ```
