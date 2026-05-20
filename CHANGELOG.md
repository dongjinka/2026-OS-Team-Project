# Changelog

본 프로젝트의 모든 주요 변경 사항을 시간순으로 기록한다.
형식은 [Keep a Changelog](https://keepachangelog.com/ko/1.1.0/) 기반.

> **읽는 법**
> - `Added` (추가) · `Changed` (변경) · `Fixed` (수정) · `Removed` (삭제) ·
>   `Security` (보안)
> - 각 항목 끝의 `(작성자)`로 누가 작업했는지 표시
> - 더 상세한 진행 상태는 [plan.md](plan.md), 아키텍처 설명은
>   [Implementation.md](Implementation.md) 참조

---

## [Unreleased]

(작업 중인 변경은 여기 누적)

---

## [2026-05-20] — 문서 정합성 정리 & 빌드 누락 복원

### Added
- (june) `CHANGELOG.md` 신규 — 팀 변경 이력 단일 추적 채널

### Fixed
- (june) `xv6-riscv/.gitignore` 패턴 `mkfs` → `mkfs/mkfs`
  로 좁힘. 기존 패턴이 `mkfs/` 디렉터리 전체를 무시해 `mkfs.c` 소스가 git에
  들어가지 못하던 문제 해소.
- (june) `xv6-riscv/mkfs/mkfs.c` 복원 — upstream `mit-pdos/xv6-riscv`의 riscv
  브랜치에서 가져옴. fresh clone 시 Makefile 빌드 실패 문제 해결.
  (`kernel/fs.h`·`param.h`가 초기 import 이후 미수정이라 그대로 호환)

### Changed
- (june) `Implementation.md` 전면 재작성 — 현 구현(가중치 테이블 CFS·jail·
  agentd·ReAct 루프) 반영. 구버전(Phase 5 보류, priority+1 가산, 직접
  dispatch) 서술 제거. plan.md(상태 추적)·CHANGELOG.md(이력)와의 역할 분리
  명시.

### Removed
- (june) `xv6-riscv/priority.patch` 삭제 — 현 CFS 구현(Linux 가중치 테이블)
  이 단순 priority+1 가산 방식을 완전히 대체. 복원이 필요하면
  `git show <삭제 커밋>~1:xv6-riscv/priority.patch`.

---

## [2026-05-18] — LLM-OS 핵심 기능 통합 (커밋 `30c81dc`)

> 본 항목은 커밋 메시지·`plan.md`에서 역추적한 요약. 상세는 해당 커밋 참조.

### Added
- (SeungBeom) CFS 스케줄러 (`vruntime` 기반 leftmost 선택, Linux 가중치 테이블)
- (SeungBeom) `setpriority`/`getpriority` 시스템 콜 + 음수 권한 가드 (F1·F2)
- (SeungBeom) 샌드박싱 — `jail()` 시스템 콜, agentd 격리 워커, 위험 syscall
  차단, 명령 큐 (F7)
- (SeungBeom) 함수별 priority 적용 (`LIST`/`SETPRIO`, F8)
- (SeungBeom) `agent.py` ReAct 루프 + 대화 메모리 (F5)
- (SeungBeom) `agentdemo` — F2·F7 검증 데모

---

## [2026-05-11] — CFS 기반 통합 (커밋 `6f406d4`)

### Added
- (Se-Joong) xv6-riscv를 submodule에서 일반 트리로 전환
- (Se-Joong) `priority.patch` 베이스라인 위에 CFS(vruntime + creation_tick) 추가
- (Se-Joong) `agent_dispatch()` 신설 (PRINT/KILL/NICE, init·sh 보호)
- (Se-Joong) `consoleintr()`에서 `REQ|...` 라인 가로채 디스패치

---

## 이전 변경

이전 커밋들은 `git log --oneline`으로 조회. 주요 마일스톤:

- `eab475d` (2026-04) docs: 코스 요구사항 문서 추가
- `08157c9` chore: 주간 개발 진행 사항 업데이트
- 그 외 초기 셋업 커밋들

---

## 작성 규칙

1. **새 변경 시작할 때** `[Unreleased]` 섹션 하위에 카테고리·작성자 표기로 추가.
2. **마일스톤·PR 머지 시점에** `[Unreleased]` → `[YYYY-MM-DD] — 설명`으로 승격.
3. **카테고리 선택**:
   - `Added` — 새 기능·파일·도구
   - `Changed` — 기존 동작·문서·인터페이스 변경
   - `Fixed` — 버그 수정
   - `Removed` — 삭제
   - `Security` — 보안 관련 (별도 강조)
4. **각 항목**은 한 줄 한 가지 변경. "왜" 또는 "어떤 효과"를 한 문장 덧붙이면
   리뷰어가 git log 안 봐도 됨.
5. **작성자 표기**(괄호 안)는 닉네임/이니셜로 통일 — git author와 일치 권장.
