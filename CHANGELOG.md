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

### Added
- **AI 자기관찰 명령어 셋** — LLM이 환경을 보고 판단하도록 정보 명령 2개 추가.
  - `PS` — 프로세스 목록(pid·state·priority·name, `[K]`/`[A]`). 신규 syscall
    `procinfo(buf,max)`(번호 28, `kernel/procinfo.h`)로 proc 스냅샷을 copyout.
    `NICE`가 대상 pid를 알 수 있게 함(관찰→행동 루프). 표현은 agentd, 데이터는 커널.
  - `HELP` — 각 명령의 인자 형식(usage) 포함 카탈로그. LLM이 호출법 런타임 확인.
  - agentd 도구 테이블에 `usage` 필드 추가, `agent.py`에 `ps`/`help` 도구 노출.
- F7 명령 거부 목록을 **설정 가능**하게 전환. 기존 하드코딩
  `{KILL, EXEC}`을 커널 RAM의 가변 목록(스핀락 보호)으로 바꾸고, 신규
  syscall `set_deny`/`get_deny`(번호 26·27)와 셸 도구 `user/denyctl.c`로
  관리. `kernel/deny.h` 신규(op 상수 공유).
  - **일회성**: `denyctl add/rm/reset` (이번 세션 RAM만)
  - **영구**: `denyctl save` → `/denylist.conf`; `init`이 부팅 시
    `denyctl load`로 자동 적용 → 재부팅 생존
  - **권한**: `set_deny`는 `is_agent` 프로세스를 거부 → 격리 agent가
    자기 샌드박스를 약화 불가 (사람 전용)
- 단일 소스 설계: 커널 거부 목록 하나가 하드 경계 명령(KILL/EXEC)과
  agentd 도구(WRITE/PRINT/…) 양쪽을 통제. agentd `LIST`는 커널 목록을
  조회해 실효 정책(`DENY(kernel)`)을 표시.

### Changed
- `Makefile` UPROGS에 `_denyctl` 등록.
- `user/init.c`: 부팅 시 `denyctl load` 1회 실행(agentd 기동 전).

---

## [2026-05-20] — 평가 자동화 & 설계 결정 문서화

### Added
- (june) `user/cfs_share.c` 신규 — N개 자식을 다른 priority로 띄워 동일
  wall-clock 동안 카운터를 경쟁시키고 priority별 CPU 점유율(%)을 출력하는
  벤치 프로그램. F3/F4의 가중치 효과를 수치로 검증. smp=1 권장.
- (june) `user/priority_test.c` Test 3에 **pipe 기반 finish-order 자동 검증**
  추가. HIGH(1)→MED(10)→LOW(19) 순서가 아니면 FAIL 종료. `burn()` 반복수도
  30M으로 상향해 스케줄링 효과가 시작 노이즈를 압도하도록 조정.

### Changed
- (june) F6(JSON 역직렬화) 설계 결정: **호스트(`agent.py`) 측 파싱 유지**.
  근거 4개(커널 안전성·부동소수/동적할당 제약·계층 분리·검증 단순화)를
  `plan.md §5.1` + `Implementation.md §7.2`에 명시. 제안서 §F6 원문(커널 내
  파싱)에서 변경된 사유를 보고서에 명기.
- (june) `Makefile` UPROGS에 `_cfs_share` 등록.
- (june) `plan.md` 진행 현황 표: F6 ✅ 완료, §5.3 평가 강화 ✅ 완료로 갱신.
- (june) `Implementation.md` §6 검증 표·§7.1 평가 한계·§8 파일 변경 요약에
  Test 3 자동 검증·`cfs_share` 항목 반영.

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
