# 구현 계획서 — OS for LLM (Direction A)

본 문서는 김승범·김세중 제안 아이디어와 `Project_requirements.md` Direction A를
기준으로 한 **구현 계획·진행 현황·남은 작업** 정리.

> 최신 갱신: 2026-05-18 · 브랜치 `SeungBeom`, 커밋 `30c81dc` 기준.
> 커널·`fs.img` 빌드 성공, smp=1·smp=3 QEMU 부팅·테스트 통과,
> 실제 Upstage Solar API로 멀티스텝 에이전트 루프 검증 완료.

---

## 1. 요구사항 → 기능 목록 매핑

제안서의 요구사항을 구현 단위로 분해하면 다음과 같다.

| #  | 기능 | 출처 | 분류 |
| -- | ---- | ---- | ---- |
| F1 | xv6 프로세스 우선순위 + `setpriority`/`getpriority` 시스템 콜 | 구현요소 §1 | 필수 |
| F2 | user/kernel 프로세스 우선순위 구분 (kernel = 음수) | 구현요소 §1 | 필수 |
| F3 | CFS 스케줄러 — vruntime 기반 (배열 스캔, RB-Tree 미사용) | 구현요소 §3 | 필수 |
| F4 | CFS 세부 규칙 — min_vruntime / fork 상속 / I/O wakeup 보정 | 구현요소 §3 | 필수 |
| F5 | QEMU ↔ Upstage Solar 연결 Python 브릿지 | 세중 제안 | 필수 |
| F6 | LLM 응답 JSON 역직렬화 | 세중 제안 | 필수 |
| F7 | 샌드박싱 — LLM 호출 가능 함수 화이트리스트 + 폴더 격리 | 구현요소 §2 / 승범 제안 | 필수 |
| F8 | LLM 전용 OS 함수 + 함수별 Priority 커스텀화 | 승범 제안 | 권장 |
| F9 | LLM 응답 캐시 (+ 디스크 백킹) | 승범 제안 / AIOS 논문 | 권장 |
| F10 | 유휴 시간대 LoRA local 학습 | 승범 제안 | 선택 (범위 초과) |

---

## 2. 진행 현황 요약

| 기능 | 상태 | 비고 |
| ---- | ---- | ---- |
| F1 우선순위 시스템 콜 | ✅ 완료 | `priority_test` 통과 |
| F2 user/kernel 구분 | ✅ 완료 | init이 priority −5 (커널-클래스), `^P`로 확인 |
| F3 CFS 본체 | ✅ 완료 | Linux `sched_prio_to_weight` 가중치 테이블 이식 |
| F4 CFS 세부 규칙 | ✅ 완료 | fork vruntime 상속·wakeup 보너스·global min_vruntime |
| F5 Python 브릿지 | ✅ 완료 | `.env` 자동 로드, `REQ\|` 프로토콜 정상 |
| F6 JSON 역직렬화 | ✅ 완료 | A안(호스트 파싱) 채택, 근거 §5.1 문서화 |
| F7 샌드박싱 | ✅ 완료 | 함수 화이트리스트 + chroot jail + 위험 syscall 차단 |
| F8 함수별 Priority | ✅ 완료 | `LIST`/`SETPRIO`, agentd가 함수별 priority 적용 |
| (추가) 에이전트 루프 | ✅ 완료 | `agent.py` ReAct + 대화 메모리, agentd 라우팅 |
| F9 응답 캐시 | 🟡 커널 구현 | cache.c · `set_cache`/`get_cache`(29·30) · cache_test 13/13; `agent.py` 연동 남음 §5.2 |
| F10 LoRA 학습 | ❌ 범위 외 | Future Work (§6) |

---

## 3. 구현 완료 내역

F2 · F3 · F4 · F7 · F8 + `agent.py` 자율 에이전트 루프를 구현했다. 검증 결과:
커널 빌드 → `fs.img` 생성 → QEMU 부팅 (smp=1·smp=3) → `priority_test` 3개 통과
→ `agentdemo` 5개 체크 통과 → 실제 Upstage Solar API로 멀티스텝 추론·대화
메모리 시나리오 통과.

### 3.1 F3·F4 — Linux 방식 CFS

- **가중치 테이블** ([proc.c](xv6-riscv/kernel/proc.c)): Linux `sched_prio_to_weight`를
  그대로 이식한 `cfs_weight[41]` (priority −20..20). vruntime 가산은
  `cfs_vdelta() = (2^20 · NICE_0_LOAD) / weight` — 가중치가 클수록(=낮은 nice)
  vruntime이 느리게 증가해 CPU를 더 점유. [trap.c](xv6-riscv/kernel/trap.c)의 두
  타이머 경로(usertrap·kerneltrap) 모두 `cfs_vdelta()` 사용.
- **min_vruntime**: O(NPROC) 스캔(`min_vruntime_skip`)을 제거하고 Linux처럼
  단조 증가 전역 변수 `cfs_min_vruntime` + `cfs_lock`으로 대체. 스케줄러가
  매 픽마다 leftmost vruntime으로 전진시킴.
- **F4 fork 상속**: `kfork()`가 `np->vruntime = p->vruntime`. 커널-클래스
  부모(priority<0)의 자식은 user 범위(10)로 환원.
- **F4 wakeup 보정**: `wakeup()`이 `max(vruntime, min_vruntime) − CFS_WAKEUP_BONUS`
  로 I/O 후 약간의 우선권 부여.
- 스케줄러 픽은 **순차 배열 스캔**으로 leftmost(min vruntime) 선택 — 요구사항
  §1의 RB-Tree 배제 방침 유지.

### 3.2 F2 — user/kernel 우선순위 구분

- `priority` 범위를 −20..20으로 확장. **음수 = 커널-클래스** (`procdump`에 `[K]` 표시).
- `sys_setpriority` ([sysproc.c](xv6-riscv/kernel/sysproc.c)): user-클래스 프로세스
  (priority ≥ 0)는 누구에게도 음수 우선순위를 부여할 수 없음 → 권한 상승 차단.
  `priority_test`의 음수 거부 케이스 그대로 통과.
- `userinit`이 `init`(pid 1, 고아 회수·셸 재기동 담당 = xv6의 supervisor)을
  priority −5 커널-클래스로 지정. fork된 자식은 user 코드를 실행하므로 user
  범위로 환원 → 커널-클래스가 전체로 전파되지 않음.
- 검증: `^P` 덤프에서 `1 sleep init prio=-5 [K]`, `2 sleep sh prio=10`.

### 3.3 F7·F8 — 샌드박싱: 화이트리스트 + chroot jail + agentd 라우팅

"사람이 친 셸 명령은 그대로, **LLM이 만든 명령만 jail 안에서 실행**"을 위해
명령 실행 경로를 분리했다. 세 계층으로 구성된다.

**(a) 커널 측 — 명령 큐 + 거부 목록** ([agentcmd.c](xv6-riscv/kernel/agentcmd.c)):
- `agent_dispatch()`는 콘솔 인터럽트 컨텍스트에서 동작 → fork·파일 작업 불가.
  실행하지 않고, **거부 목록**(기본 `KILL`/`EXEC`)만 즉시 차단한 뒤 나머지
  `REQ|` 라인을 커널 링버퍼(`agentq`)에 적재.
- **거부 목록은 설정 가능** (하드코딩 아님): 스핀락으로 보호되는 커널 RAM
  목록 + `set_deny`/`get_deny` syscall + 셸 도구 `denyctl`. 일회성(RAM) ·
  영구(`/denylist.conf`, 부팅 시 `init`이 `denyctl load`로 자동 적용) ·
  기본 복귀(`reset`) 지원. `set_deny`는 `is_agent` 프로세스를 거부 → 사람만
  변경 가능. 커널 목록 하나가 하드 경계 명령과 agentd 도구를 모두 통제하며,
  agentd `LIST`가 실효 정책을 표시.
- 신규 `agent_recv()` 시스템 콜([sysproc.c](xv6-riscv/kernel/sysproc.c))로
  격리 프로세스만 큐를 비울 수 있음 (`is_agent`만 호출 가능).

**(b) chroot jail + 위험 syscall 차단** ([proc.h](xv6-riscv/kernel/proc.h),
[fs.c](xv6-riscv/kernel/fs.c), [sysfile.c](xv6-riscv/kernel/sysfile.c),
[syscall.c](xv6-riscv/kernel/syscall.c)):
- `struct proc`에 `is_agent`·`jail_root` 필드 추가.
- 신규 `jail(path)` 시스템 콜: 호출 프로세스를 path에 가두고 `is_agent=1` 설정.
  되돌릴 수 없음.
- `namex()`: agent 프로세스의 `/`는 `jail_root`로 매핑, `..`로 jail 루트 위로
  올라가려는 시도 차단.
- `syscall()`: agent 프로세스는 `exec`·`kill`·`mknod` 거부 ("폴더 안에서만
  권한, 시스템 밖에서는 무권한").
- jail은 **opt-in** — `jail()`을 호출한 프로세스와 그 자식만 영향. 일반
  셸·명령어는 무영향.

**(c) 격리 워커 `agentd`** ([user/agentd.c](xv6-riscv/user/agentd.c)):
- [user/init.c](xv6-riscv/user/init.c)가 부팅 시 `agentd`를 자동 기동.
- 시작 즉시 `jail("/agentbox")`로 자기 격리 → 명령 큐에서 명령을 받아
  **jail 안에서** 실행.
- 도구 테이블 보유 (F7 화이트리스트 + F8 함수별 priority):
  `PRINT` · `READ` · `WRITE` · `LS` · `NICE` · `LIST` · `SETPRIO` · `PS` · `HELP`.
- 각 도구 실행 전 `setpriority(self, table[i].priority)`로 함수별 우선순위
  적용 → F8의 "LLM 주도 priority 커스텀화"가 실제 스케줄러에 반영됨.
- **AI 자기관찰**: `PS`(프로세스 목록 — 신규 `procinfo` syscall)로 LLM이 pid를
  파악해 `NICE`에 사용, `HELP`(usage 카탈로그)로 호출법 확인 → 관찰→행동 루프.

**결과**: 사람의 셸 입력은 무제한 그대로. LLM 명령은 모두 chroot jail + 위험
syscall 차단 안에서만 동작. 커널 거부 명령은 agentd까지 도달하지 못함
(이중 방어).

**검증**: `agentdemo` ([user/agentdemo.c](xv6-riscv/user/agentdemo.c))가 jail 내부
파일 읽기/쓰기 성공, `..`·외부 경로 차단, 음수 우선순위 거부, `exec`·`kill`
차단을 모두 통과.

### 3.4 F5 + 에이전트 루프 — agent.py

원래는 단발 번역기였으나(`자연어 → JSON 1개 → 명령 1개 → 끝`), 다음과 같이
**자율 에이전트 루프**로 재설계했다.

- **`.env` 자동 로드**: 스크립트 옆 `.env`에서 `UPSTAGE_API_KEY`/`UPSTAGE_MODEL`
  로드. 실제 환경변수가 우선.
- **ReAct 루프**: 시스템 프롬프트에 도구 목록 + 응답 스키마 명시
  (`{thought,tool,args}` 또는 `{thought,answer}`). 각 스텝마다 LLM 호출 →
  도구 실행 → xv6 출력 캡처 → OBSERVATION으로 다음 LLM 호출에 전달. 최대 8스텝.
- **대화 메모리**: `self.messages`에 system + 전체 대화 누적. 후속 질문이
  이전 맥락(예: 이전에 읽은 파일 내용)을 그대로 활용. 최근 24개 메시지로
  자동 정리.
- **출력 캡처(마커 방식)**: 명령 뒤에 `REQ|PRINT|__OBS<n>__`을 함께 전송.
  agentd가 큐를 순서대로 처리하므로 마커 출력 이전의 모든 라인이 해당 도구의
  순수 결과 → 정확히 잘라내 LLM에 OBSERVATION으로 제공.
- **출력 정리**: 스레드 안전 락 + 3-상태 커서(`start`/`xv6`/`other`)로 메인·
  리더 스레드의 동시 write 깨짐을 제거. xv6 출력(`xv6 ┃` 녹색), 전송 로그
  (`→ xv6` 노랑), 사고(`💭` dim), 최종 답변(`╭─ answer ─╮` 청록 박스)로
  시각 구분. 명령 에코·내부 마커 라인은 표시·관측에서 제외.
- **모드**: `mock`(키 없음 — 1스텝 더미) / `solar`(키 있음 — 풀 ReAct 루프).
- **검증 시나리오** (실제 Solar API):
  - "파일 만들어" → write
  - "지금까지 만든 파일 목록" → ls (LLM이 자율 결정)
  - "파일 내용 정리해줘" → ls → read × N → 요약 답변 (멀티스텝 계획)
  - "CFS 언급한 파일이 뭐였지?" → 도구 호출 없이 메모리에서 답변 (대화 기억)

### 3.5 변경/신규 파일 목록

| 파일 | 변경 |
| ---- | ---- |
| `kernel/proc.h` | `is_agent`, `jail_root` 필드 |
| `kernel/proc.c` | CFS 가중치 테이블·`cfs_vdelta`·`cfs_min`, fork·wakeup·scheduler·procdump 수정 |
| `kernel/trap.c` | 타이머 vruntime 가산을 `cfs_vdelta()`로 |
| `kernel/fs.c` | `namex()` chroot jail 적용 |
| `kernel/main.c` | `agentcmd_init()` 호출 |
| `kernel/syscall.{c,h}` | `SYS_jail`·`SYS_agent_recv` 등록, agent 위험 syscall 차단 |
| `kernel/sysfile.c` | `sys_jail()` 신규 |
| `kernel/sysproc.c` | `sys_setpriority` 음수 권한 가드, `sys_agent_recv()` 신규 |
| `kernel/agentcmd.c` | 명령 큐 + 거부 목록 게이트로 재작성 |
| `kernel/defs.h` | `cfs_vdelta`·`agentcmd_init`·`agentq_get` 프로토타입 |
| `user/init.c` | `agentd` 자동 기동 추가 |
| `user/{user.h,usys.pl}` | `jail()`·`agent_recv()` 스텁 |
| `user/agentd.c` | **신규** — 격리 에이전트 런타임 (도구 테이블·LS 포함); `LIST`가 커널 거부 목록 반영 |
| `kernel/deny.h` | **신규** — 거부 목록 op 상수(커널·user 공유) |
| `kernel/agentcmd.c` | 거부 목록을 가변·스핀락 구조로 + `deny_add/remove/reset/clear/snapshot` |
| `kernel/sysproc.c` | `sys_set_deny`·`sys_get_deny` 신규(사람 전용 가드) |
| `user/denyctl.c` | **신규** — 거부 목록 관리 셸 도구 |
| `user/init.c` | 부팅 시 `denyctl load` 1회 추가 |
| `user/agentdemo.c` | **신규** — F2·F7 데모 프로그램 |
| `mkfs/mkfs.c` | **신규(복원)** — `.gitignore`가 `mkfs/`를 통째 제외해 누락 |
| `agent.py` | 단발 번역기 → 자율 에이전트 루프, `.env` 로더, 출력 동기화 |
| `kernel/cache.c` | **신규(이식)** — F9 LLM 응답 캐시(RAM+디스크·MinHash/Jaccard). `76b2737` 이식 + 보안 하드닝 |
| `kernel/sysproc.c` | `sys_set_cache`·`sys_get_cache` 신규(번호 29·30) |
| `kernel/sysfile.c` | `create()` non-static 전환(cache.c가 `/cache.bin` 지연 생성) |
| `kernel/main.c` | `cacheinit()` 호출 추가 |
| `user/cache_test.c` | **신규(이식)** — F9 캐시 단독 테스트(13/13 통과) |
| `Makefile` | `cache.o`·`_cache_test` 등록 |
| `Makefile` | `_agentdemo`·`_agentd` UPROGS 등록 |
| `plan.md` | **신규** — 본 문서 |

---

## 4. 평가 지표 제안 (Week 12 대비)

| 항목 | 측정 방법 |
| ---- | --------- |
| CFS 공정성 | 동일 priority 프로세스 N개의 완료 시각 분산 |
| 우선순위 효과 | priority 1 vs 19 프로세스의 완료 순서·CPU 점유 비율 |
| 샌드박싱 | 거부 명령(`KILL`/`EXEC`) 호출 시 100% DENY, 허용 명령 정상 동작 |
| Jail 격리 | `..` 탈출·절대경로 접근·외부 파일 가시성 모두 차단되는지 |
| 에이전트 루프 | 멀티스텝 작업의 도구 호출 횟수·성공률·대화 메모리 활용도 |
| 캐시 (F9, `agent.py` 연동 시) | 히트율, 히트 시 응답 지연 vs 미스 시(API 왕복) 지연 비교. 커널·`cache_test`는 구현 완료, 종단 측정은 연동 후 |

---

## 5. 남은 작업

### 5.1 F6 — JSON 역직렬화 위치 (✅ 결정: A안 채택)

**결정**: JSON 파싱은 **호스트(`agent.py`)에서 수행**한다. 커널은 검증된
최소 포맷(`REQ|<CMD>|<arg>`)만 수신.

**근거**:
1. **커널 안전성** — 동적 입력을 파싱하는 코드는 메모리 안전성 결함이 곧
   커널 패닉. 호스트 측 Python `json.loads`는 표준 라이브러리로 검증돼 있어
   리스크 0에 가까움.
2. **부동소수·동적할당 제약** — xv6 커널은 둘 다 금지. JSON은 number(부동
   소수)·중첩 객체(동적 트리)를 자연스럽게 포함하므로 커널 구현 시 기능
   축소·고정 크기 버퍼 강제 등 비자연스러운 제약이 누적.
3. **분리된 책임** — LLM 응답 해석은 "에이전트의 사고 단계", 커널 명령
   실행은 "OS의 실행 단계". 두 계층을 분리해두면 LLM 측 포맷 변경(예: 향후
   tool calling API 도입)이 커널에 영향 주지 않음.
4. **사후 검증** — 커널은 단순 포맷이라 입력 검증(`REQ|` prefix, 명령
   화이트리스트, arg 길이 제한)이 한 줄짜리로 끝남. 거부 목록·jail·위험
   syscall 차단 3중 방어와 정합적.

**한계 인정**: 제안서 §F6 원문은 "xv6 내부 JSON 역직렬화"를 명시함. 위
근거를 기술 보고서에 명시하여 설계 의도가 변경된 사유를 분명히 한다.

대안 B(`kernel/json.c` 미니 파서)는 [Implementation.md §7.2](Implementation.md)
의 "선택 작업"으로 표기하되, 보안·유지보수 비용 대비 학습적 가치 외 실익이
없어 우선순위 최하.

### 5.2 F9 — LLM 응답 캐시 (커널 구현 완료 · agent.py 연동 남음)

커널 측은 구현·검증 완료 (Se-Joong 원작 `c56b028`을 `76b2737`에서 이식):

- `kernel/cache.c`: 16-슬롯 RAM 테이블 + `/cache.bin` 디스크 오버레이(full RAM에
  set 시 LRU 슬롯을 디스크에 append, 디스크 hit 시 RAM으로 promote). 키는
  FNV-1a 64-bit 해시로 압축 — 동적 할당 없는 정적 배열.
- 시스템 콜 `set_cache`/`get_cache`(번호 29·30). `user/cache_test.c` 13/13 통과.
- 의미 매칭: MinHash + Jaccard로 정확 매치 미스 시 paraphrase·어순변경·부분
  치환을 탐지(임계 Jaccard 0.7).
- `/cache.bin` 파일 백킹으로 재부팅 후에도 캐시 유지 — 파일시스템 개념 시연.

남은 작업 (여유 시):

- `agent.py`가 Solar 호출 전 `get_cache` 조회 → 히트 시 API 왕복 생략(비용·지연
  절감). 현재 캐시는 커널·테스트에만 연결되어 LLM 요청 경로에는 미연동.

### 5.3 평가·벤치마크 강화 (✅ 완료)

- **Test 3 자동 검증** — pipe 기반 finish-order 강제 검증 추가
  (2026-05-20). HIGH(1)→MED(10)→LOW(19) 순서가 아니면 FAIL 종료.
  `burn()` 반복수도 30,000,000으로 상향해 스케줄링 효과가 시작 노이즈를
  압도하도록 조정.
- **CFS 점유율 정량 벤치** — 신규 `cfs_share` 프로그램 (2026-05-20).
  N개 자식이 동일 wall-clock 동안 카운터를 돌려 priority별 CPU 점유율(%)을
  출력. smp=1 권장. 결과는 보고서 §평가 지표에 인용.

### 5.4 정리·문서

- `.gitignore`의 `mkfs` → `mkfs/mkfs`로 수정 (mkfs.c 소스가 커밋되도록).
- `xv6-riscv/priority.patch` 삭제 또는 `docs/legacy/`로 이동 (현 CFS 코드와
  불일치).
- `Implementation.md` 갱신: 구버전 CFS(`priority+1`)·"Phase 5 보류" 서술 →
  현 구현(Linux 가중치 테이블, 샌드박싱·jail·agentd 라우팅 완료)에 맞춰 정정.
  빌드 경로 예시(`/root/OS_Project`)도 실제 경로로 통일.
- README에 데모 스크린샷/GIF, `agent.py` 사용법 추가.

---

## 6. 범위 외

**F10 — 유휴 시간대 LoRA 학습**: xv6(RISC-V, 부동소수점·디스크·메모리 극히
제한)에서 실제 LoRA 학습 불가. 보고서의 Future Work로 표기하거나, "유휴 tick
감지 → 호스트에 학습 트리거 신호 전송" 정도의 **개념 시연(stub)** 수준으로만
다룰 것.
