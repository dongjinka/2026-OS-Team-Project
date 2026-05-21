# Branch 비교: `main` ↔ `Sejoong`

> 생성: 2026-05-20 · `merge-base = eab475d` 기준 (공통 조상: "docs: 코스 요구사항 문서 추가")
> 비교 명령: `git diff main..Sejoong`, `git log main..Sejoong`, `git log Sejoong..main`

---

## 1. 한눈 요약

| 항목 | `main` (HEAD `8ded8d7`) | `Sejoong` (HEAD `c56b028`) |
|---|---|---|
| **역할** | 팀 통합 branch — SeungBeom 의 LLM-OS 핵심 통합본 | 개인 작업 branch — 캐시·동시성 시연 확장 |
| **공통 조상 이후 unique commit** | 11 개 (SeungBeom merge + plan.md 정리 + 주간 업데이트 6 개) | 1 개 (`c56b028` semantic cache + write_race) |
| **핵심 추가** | `agentd.c` (jailed agent 데몬), `agentdemo.c` (jail 시연), CFS·우선순위·샌드박싱 통합 | `cache.c` (MinHash semantic cache), `write_race.c`, `cache_test.c`, `eval.c`, `agent_multi.c` |
| **공유 자원 경합 관점** | jail / 우선순위로 *권한* 격리 | sleeplock 기반 *직렬화* 시연 |
| **agent.py** | 초기 단순 브릿지 (164 줄 가량 작음) | `_emit_async` + readline buffer redraw, role 토글, ACL 라우팅 (+360 줄) |

전체 diff: **29 파일 / +2489 / −1398**

---

## 2. Commit 위상

```
main:    eab475d ─ 91695b3 ─ d098a2e ─ … (Weekly 업데이트 5개) … ─ 8f1fe19 ─ 30c81dc ─ 5dcdd7d ─ 8ded8d7 (HEAD)
                                                                                 └─ SeungBeom 핵심 작업 + plan.md 정리

Sejoong: eab475d ─ c56b028 (HEAD)
                  └─ semantic cache + write_race + agent.py REPL
```

### `Sejoong` 에만 있는 commit
| Hash | 메시지 |
|------|------|
| `c56b028` | `feat: semantic cache (MinHash+Jaccard) 및 동시성 시연 추가` |

### `main` 에만 있는 commit (요약)
| Hash | 메시지 |
|------|------|
| `8ded8d7` | `Merge pull request #3 from dongjinka/SeungBeom` |
| `5dcdd7d` | `docs: plan.md 정리 — 진행 현황/구현 내역 단일화` |
| `30c81dc` | `feat: LLM-OS 핵심 기능 구현 (CFS·우선순위·샌드박싱·에이전트 루프)` |
| `8f1fe19` | `Merge pull request #2 from dongjinka/Sejoong` |
| 5 × `Update Weekly_Development_Process.md` | 주간 진행 기록 |
| `d098a2e` | `Update project progress and requirements in documentation` |
| `91695b3` | `Merge pull request #1 from dongjinka/Sejoong` |

---

## 3. 파일 단위 변경 (`git diff --name-status main..Sejoong`)

### 3.1 `Sejoong` 에만 있는 파일 (A)

| 경로 | 줄 | 역할 |
|------|----|------|
| `xv6-riscv/kernel/cache.c` | +355 | FNV-1a 정확 캐시 + **MinHash 64-D signature** + Jaccard ≥0.7 임계 의미 매칭 |
| `xv6-riscv/user/cache_test.c` | +132 | 캐시 단위 테스트 10+ 케이스 (정확/어순/한국어 paraphrase/negative) |
| `xv6-riscv/user/eval.c` | +261 | `eval cache|acl|fair|semantic` 평가 지표 셸 명령 |
| `xv6-riscv/user/agent_multi.c` | +110 | 다중 에이전트 동시 dispatch 시연 |
| `xv6-riscv/user/write_race.c` | +76 | 4 자식이 같은 파일 WRITE 경합 → sleeplock 직렬화 시각화 |

### 3.2 `main` 에만 있는 파일 (D from Sejoong perspective)

| 경로 | 역할 |
|------|------|
| `xv6-riscv/user/agentd.c` | 부팅 시 init 이 띄우는 **jailed LLM agent daemon**. `jail()` 으로 chroot, kernel 화이트리스트 함수만 호출. 사용자 셸 입력은 도달 못함 — LLM 발화만 처리 |
| `xv6-riscv/user/agentdemo.c` | F2 (priority guard) + F7 (filesystem jail) 시연 프로그램 |
| `plan.md` | 통합된 구현 계획서 — F1~F10 진행 현황. SeungBeom 의 jail 기반 sandboxing 모델 기준 |

> **유의**: Sejoong 은 위 3 파일을 *지운 게 아니라*, 갈래가 분기된 `eab475d` 이후 main 만 추가한 것. Sejoong 입장에서는 "본 적 없는 파일".

### 3.3 양쪽 모두 수정된 파일 (M) — 변경 크기 큰 순

| 경로 | +/− | 핵심 차이 (Sejoong 관점) |
|------|----|---|
| `Implementation.md` | +700 / 적음 | semantic cache·동시성 시연·평가 절차 등 추가 기술 |
| `agent.py` | 거의 재작성 | `_emit_async` + readline buffer redraw, `_io_lock`, role 토글 (`:role`), LLM_REQ 응답 라우팅 |
| `xv6-riscv/kernel/agentcmd.c` | +521 | `handle_ask` 2단계 분기 (exact → semantic → LLM), CHAT/PRINT prefix 가드, ACL 적용 |
| `README.md` | +210 | 사용법 + 데모 흐름 갱신 |
| `xv6-riscv/kernel/proc.c` | mostly identical to main | (SeungBeom 의 CFS·priority 통합이 main 에 있음 — 정확히는 Sejoong 에는 그 작업이 없음) |
| `xv6-riscv/kernel/sysproc.c`, `syscall.c/h`, `trap.c`, `defs.h`, `user.h`, `usys.pl` | mid | 디스패처 syscall 노출, dispatch / uptime 등 |
| `xv6-riscv/Makefile` | +9 / -2 | UPROGS 에 `_cache_test`, `_eval`, `_agent_multi`, `_write_race` 추가; main 은 `_agentd`, `_agentdemo` 추가 |
| `.gitignore` | +7 | Sejoong 측 추가 항목 (`.vscode/`, 학습용 가이드 등은 사용자가 따로 조정) |

---

## 4. 기능적 차이 요약

| 차원 | `main` (SeungBeom 통합) | `Sejoong` |
|------|------------------------|-----------|
| **CFS / priority** | ✅ 통합 (`30c81dc`) — `nice`, `setpriority`, vruntime, kernel 음수 우선순위 | ❌ 미반영 (분기 이전 시점) |
| **샌드박싱** | `jail()` syscall + `agentd` 데몬으로 *프로세스 단위 chroot* | 커널 ACL (`role: reader/writer/admin`) 기반 **명령 단위 권한 거부** (`[deny]`) |
| **LLM 캐시** | 부재 또는 정확 매칭만 | **정확 + 의미** (MinHash 64-D, Jaccard ≥ 7/10). CHAT/PRINT 응답에만 의미 매칭 허용 (WRITE/KILL 등 부작용 액션 안전) |
| **에이전트 진입** | `agentd` (부팅 시 자동) — LLM 발화만 처리 | `agent.py` REPL — 사용자 입력 + LLM 응답 모두 커널로 dispatch |
| **동시성 시연** | jail 격리 위주 | `agent_multi`, `write_race` — sleeplock 직렬화 / 4 writer race 가시화 |
| **평가** | `plan.md` 의 F1~F10 진행표 | `eval cache | acl | fair | semantic` 셸 명령 (정량 지표) |
| **REPL UX** | (해당 없음 — 데몬 모드) | `_emit_async` 로 비동기 출력 시 readline 입력버퍼 보존, prompt 라인 redraw |

### 두 모델의 관점 비교

- **main (SeungBeom)** — *권한 격리 우선*. LLM 이 호출 가능한 함수 화이트리스트 + chroot. "에이전트가 무엇을 *볼 수* 있는가" 를 OS 가 통제.
- **Sejoong** — *요청 단위 ACL 우선* + *응답 캐싱*. role 별 명령 가드와 캐시 hit/miss 가시화. "에이전트가 무엇을 *반복 호출* 하는가" 를 OS 가 통제.

두 접근은 상호 배타가 아니라 **계층적 합성 가능** (jail 안에서 role + 캐시).

---

## 5. 통합(머지) 시 예상 충돌 지점

| 파일 | 충돌 가능성 | 비고 |
|------|----|---|
| `xv6-riscv/kernel/agentcmd.c` | **High** | 양쪽 다 dispatcher 본체. main 은 jail 경유 호출, Sejoong 은 ACL + semantic cache. 본문 재구성 필요 |
| `xv6-riscv/kernel/defs.h`, `syscall.c/h`, `user.h`, `usys.pl` | Mid | syscall 번호/프로토타입 양쪽 모두 추가 — 번호 충돌 점검 |
| `xv6-riscv/Makefile` UPROGS | Low | 양쪽 항목 단순 합산 |
| `xv6-riscv/kernel/proc.c`, `proc.h`, `trap.c` | Mid | main 의 CFS 로 통합 — Sejoong 의 변경은 대체로 단순 hook 이라 흡수 가능 |
| `Implementation.md`, `README.md`, `Weekly_Development_Process.md` | High | 텍스트 머지 — 수동 정리 권장 |
| `plan.md` | Sejoong 에 없음 | main 의 `plan.md` 를 살리고 Sejoong 의 새 기능을 그 안에 반영하는 방향 권장 |

권장 머지 순서:
1. `Sejoong` 에서 `git merge main` (먼저 SeungBeom 통합본을 흡수 — `proc.c`/CFS 충돌 해결)
2. `agentd` 와 `agent.py` REPL 의 역할 분담 결정 (양립할지, 한쪽으로 통일할지)
3. `agentcmd.c` 재정리 — jail 통과 후 ACL + cache 흐름
4. main 으로 PR 제출

---

## 6. 재현 명령

```bash
# 변경량 다시 보기
git diff --stat main..Sejoong

# 특정 파일 한 줄씩 비교
git diff main Sejoong -- xv6-riscv/kernel/agentcmd.c

# Sejoong 만의 commit 본문
git log -p main..Sejoong

# main 만의 commit 본문 (SeungBeom 작업 포함)
git log -p Sejoong..main
```
