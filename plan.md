# 구현 계획서 — OS for LLM (Direction A)

본 문서는 김승범·김세중 제안 아이디어와 `Project_requirements.md` Direction A를
기준으로 **구현해야 할 기능 목록**, **현재 진행 상황 점검**, **남은 작업의
구현 방법**을 정리한다.

> 점검 기준 커밋: `8f1fe19` (main). 커널은 `riscv64-linux-gnu-gcc`로 **경고/에러 없이
> 정상 빌드**됨을 확인했다(`make kernel/kernel` 성공).

> **갱신 (2026-05-18):** F2 · F3 · F4 · F7 · F8을 구현·빌드·QEMU 부팅 검증 완료했다.
> 변경 요약은 아래 §8을 참고. 캐시(F9)·LoRA(F10)는 미착수.

---

## 1. 요구사항 → 기능 목록 매핑

제안서의 요구사항을 구현 단위로 분해하면 다음과 같다.

| #  | 기능 | 출처 | 분류 |
| -- | ---- | ---- | ---- |
| F1 | xv6 프로세스 우선순위 필드 + `setpriority`/`getpriority` 시스템 콜 | 구현요소 §1 | 필수 |
| F2 | user/kernel 프로세스 우선순위 구분 (kernel = 음수 우선순위) | 구현요소 §1 | 필수 |
| F3 | CFS 스케줄러 — vruntime 기반 공정 분배 (순차 배열 탐색, RB-Tree 미사용) | 구현요소 §3 | 필수 |
| F4 | CFS 세부 규칙 — min_vruntime 초기화 / fork 상속 / I/O wakeup 보정 | 구현요소 §3 | 필수 |
| F5 | QEMU ↔ Upstage API 연결 Python 브릿지 | 세중 제안 | 필수 |
| F6 | LLM 응답 JSON 역직렬화 | 세중 제안 | 필수 |
| F7 | 샌드박싱 — LLM이 호출 가능한 커널 함수 화이트리스트 제한 | 구현요소 §2 / 승범 제안 | 필수 |
| F8 | LLM 전용 OS 함수 + 함수별 Priority 커스텀화 | 승범 제안 | 권장 |
| F9 | LLM 응답 캐시 (+ 디스크 백킹) | 승범 제안 / AIOS 논문 | 권장 |
| F10 | 유휴 시간대 LoRA local 학습 | 승범 제안 | 선택(과제 범위 초과) |

---

## 2. 현재 진행 상황 점검

### ✅ F1 — 프로세스 우선순위 (완료)

- `struct proc`에 `int priority` 추가, 기본값 10, `freeproc` 시 0 ([proc.h:107](xv6-riscv/kernel/proc.h#L107)).
- `sys_setpriority` / `sys_getpriority` 구현, 범위(0~20) 검증 ([sysproc.c:113](xv6-riscv/kernel/sysproc.c#L113)).
- `user/priority_test.c` 테스트 프로그램 + Makefile 등록 완료.
- **의견:** 기능 자체는 완성. 다만 `priority.patch`는 **2-pass 우선순위
  라운드로빈** 방식이고, 실제 커밋된 `scheduler()`는 CFS로 교체되어 있다.
  `priority.patch`는 더 이상 적용 대상이 아니므로 **혼동을 피하기 위해 삭제하거나
  `docs/legacy/`로 이동**할 것을 권장한다.

### ⚠️ F2 — user/kernel 우선순위 구분 (미구현)

- 요구사항: user 프로세스는 `[0~양수]`, kernel 프로세스는 **음수** 우선순위.
- 현재: 모든 프로세스가 0~20 범위. 커널 스레드용 음수 우선순위 경로 없음.
  `priority` 필드가 `int`라 음수 저장은 가능하나 사용되는 곳이 없다.
- **의견:** 미완료. §5.1에 구현 방법 기술. (xv6에는 순수 커널 스레드가 사실상
  `scheduler`/`init` 정도뿐이라, 이 항목은 "범위만 분리하고 데모용으로 한두
  프로세스에 적용"하는 수준으로 축소 정의하는 것이 현실적이다.)

### ✅/⚠️ F3·F4 — CFS 스케줄러 (대부분 완료, 일부 요구사항 불일치)

완료된 부분:
- `vruntime`(uint64), `creation_tick`(uint) 필드 추가 ([proc.h:108](xv6-riscv/kernel/proc.h#L108)).
- `min_vruntime_skip()` 헬퍼 — RUNNABLE+RUNNING 중 최소 vruntime, 락 재진입 방지.
- `scheduler()` 1-pass min-vruntime 선택 + `creation_tick` 타이브레이크 ([proc.c:450](xv6-riscv/kernel/proc.c#L450)).
- `usertrap`/`kerneltrap` 타이머 틱에서 `vruntime += priority+1` 가산 ([trap.c:85](xv6-riscv/kernel/trap.c#L85)).
- `wakeup()`에서 깨어난 프로세스 vruntime을 min_vruntime으로 floor.
- 최초 프로세스는 vruntime 0 (`allocproc`에서 다른 프로세스 없으면 min=0).

요구사항과 **불일치하는 부분 (수정 필요):**

1. **Fork 시 vruntime 상속.** 요구사항 §3은 "fork 시 부모의 vruntime을 상속"이라
   명시하나, 현재 `allocproc()`은 `vruntime = min_vruntime_skip(p)`로 설정하고
   `kfork()`는 `priority`만 복사한다 ([proc.c:316](xv6-riscv/kernel/proc.c#L316)).
   결과적으로 자식은 부모값이 아닌 **전역 최소값**을 받는다.
   → `kfork()`에서 `np->vruntime = p->vruntime`(또는 `+ 약간의 가산`)으로 수정 필요.
   `Implementation.md` §2.3 설명("부모와 같은 vruntime")도 코드와 어긋나므로
   같이 정정해야 한다.

2. **I/O wakeup 우선권 보너스.** 요구사항 §3은 "기존 vruntime과 min_vruntime 중
   **큰 값** 선택 + 약간의 우선권 부여"인데, 현재 `wakeup()`은 `max`만 적용하고
   보너스(소량 차감)가 없다. → §5.2에 구현 방법 기술.

3. (경미) `min_vruntime_skip()`이 `scheduler()` 매 픽마다 호출되지는 않지만
   `allocproc`/`wakeup`마다 NPROC 전체를 락 순회한다. 64개 한도라 성능 문제는
   없으나, 평가 보고서에서 "순차 배열 탐색" 설계 근거로 명시하면 좋다.

### ✅ F5 — Python 브릿지 (완료)

- `agent.py`: QEMU TCP 4444 접속, 수신 스레드, REPL, mock/solar 자동 전환 완료.
- `make qemu-agent` 타깃, `.env.example`, `.gitignore`(`.env` 차단) 정비됨.
- **의견:** 기능 완성. 단 `DEFAULT_MODEL = "solar-pro2"`인데 과제 지정 모델은
  **Solar Pro 3**이다. `UPSTAGE_MODEL` 환경변수로 덮어쓸 수 있으나,
  콘솔 문서 확인 후 기본값을 정확한 Pro 3 모델 ID로 갱신할 것.

### ⚠️ F6 — JSON 역직렬화 (위치 설계 재검토 필요)

- 세중 제안 원문: "역직렬화 함수를 **xv6 내부에** 구현".
- 현재 설계: JSON 파싱은 **Python(`agent.py`)** 이 수행하고, 커널에는
  `REQ|CMD|arg` 단순 파이프 포맷만 전달된다. 커널 측 JSON 파서는 없다.
- **의견:** 현재 방식(Python 파싱 + 단순 와이어 포맷)이 커널 복잡도·안정성
  측면에서 더 낫다. 다만 이는 제안서와 명백히 다른 설계 결정이므로 **둘 중 하나**를
  선택해 문서에 명시해야 한다:
  - (A) 현 설계 유지 → 기술 보고서에 "JSON 파싱은 호스트 측에 배치, 커널은
    검증된 최소 포맷만 수신"이라고 설계 근거를 명기. (권장)
  - (B) 제안서 충실 이행 → 커널에 미니 JSON 파서 구현(§5.4). OS 개념(시스템 콜,
    문자열 처리) 시연 거리가 늘어나는 장점이 있다.

### ✅/⚠️ F7 — 샌드박싱 (부분 구현, 핵심 미완)

- 현재 `agent_dispatch()`는 `PRINT`/`KILL`/`NICE` **3개 명령만** 인식하고
  나머지는 `unknown cmd`로 무시한다. `KILL`은 pid ≤ 2(init/sh) 거부 ([agentcmd.c:55](xv6-riscv/kernel/agentcmd.c#L55)).
- 이는 "암묵적" 제한일 뿐, 요구사항 §2가 말하는 **명시적 화이트리스트
  (read/write/fetch 허용, kill/exec 차단)** 구조는 없다. 역할(role) 기반 ACL도 없다.
- **의견:** 핵심 미완. `Implementation.md`도 Phase 5로 보류했다고 적고 있다.
  이번 마일스톤의 최우선 작업으로 §5.3에 구현 방법 기술.

### ❌ F8 — LLM 전용 OS 함수 + 함수별 Priority (미구현)

- 승범 제안의 핵심: LLM이 OS 전용 함수 목록을 조회하고, 함수마다 Priority를
  지정해 중요한 작업을 빠르게 끝낼 수 있게 함.
- 현재: 함수 목록 조회 명령 없음, 함수별 priority 개념 없음.
- **의견:** 미구현. F7(샌드박싱)과 함께 설계하면 자연스럽다(화이트리스트 = 함수
  목록). §5.3 참고.

### ❌ F9 — LLM 응답 캐시 (미구현)

- `Implementation.md` Phase 3에서 의도적으로 보류. `kernel/cache.c` 미존재.
- **의견:** 미구현. 시간 여유가 있으면 §5.5 방식으로 구현. 우선순위는 F7 < F9.

### ❌ F10 — 유휴 시간 LoRA 학습 (범위 초과)

- xv6(RISC-V, 부동소수점·디스크·메모리 극히 제한)에서 실제 LoRA 학습은 불가능.
- **의견:** 과제 범위에서 제외하거나, "유휴 tick 감지 → 호스트에 학습 트리거
  신호 전송"하는 **개념 시연(stub)** 수준으로만 다룰 것. 보고서의 Future Work로.

---

## 3. 진행 상황 요약표

| 기능 | 상태 | 조치 |
| ---- | ---- | ---- |
| F1 우선순위 시스템 콜 | ✅ 완료 | `priority.patch` 정리 |
| F2 user/kernel 우선순위 구분 | ✅ 구현 완료 | §8 참고 |
| F3 CFS 스케줄러 본체 | ✅ 완료 | Linux 가중치 테이블로 강화 §8 |
| F4 CFS 세부 규칙 | ✅ 구현 완료 | fork 상속·wakeup 보너스 적용 §8 |
| F5 Python 브릿지 | ✅ 완료 | 모델 ID를 Solar Pro 3로 |
| F6 JSON 역직렬화 | ⚠️ 설계 선택 | (A) 문서화 또는 (B) §5.4 |
| F7 샌드박싱 (화이트리스트 + chroot jail) | ✅ 구현 완료 | §8 참고 |
| F8 LLM 전용 함수+Priority | ✅ 구현 완료 | §8 참고 |
| F9 LLM 응답 캐시 | ❌ 미구현 | §5.5 (여유 시) |
| F10 LoRA 학습 | ❌ 범위 초과 | Future Work |

---

## 4. 작업 우선순위 (권장 순서)

1. **F7 + F8 — 샌드박싱 화이트리스트** (제안서 핵심, OS 개념 시연 가치 큼)
2. **F4 — CFS 요구사항 불일치 수정** (이미 8할 완성, 마무리 비용 낮음)
3. **F2 — user/kernel 우선순위 구분** (범위 축소 정의 후 구현)
4. **F6 — JSON 처리 위치 확정** ((A) 문서화가 비용 0, 권장)
5. **F9 — LLM 응답 캐시** (시간 여유 시; 캐시는 OS의 핵심 개념이라 보고서 가치 높음)
6. 정리 작업 — `priority.patch` 제거, `Implementation.md` 정정, 모델 ID 갱신

---

## 5. 남은 작업 구현 방법

### 5.1 F2 — user/kernel 우선순위 구분

- `priority` 필드는 이미 `int`라 음수 저장 가능. 범위 규약을 재정의:
  - user 프로세스: `0 ~ 20` (양수, 기존 유지)
  - kernel 프로세스: `-1 ~ -N` (음수 = 항상 user보다 우선)
- `struct proc`에 `int is_kernel` 플래그를 두거나, `allocproc()`에서 호출 맥락으로
  구분. xv6는 `userinit()`/`forkret` 외 순수 커널 스레드가 없으므로, **데모용으로
  특정 시스템 프로세스(init 등)에 음수 우선순위를 부여**하는 수준으로 정의.
- vruntime 가산식 `vruntime += priority + 1`은 음수 priority에서 0 이하가 되어
  vruntime이 증가하지 않거나 감소한다. → **가중치를 별도 함수로 분리**:
  ```c
  // priority -N..20  ->  weight 양수 보장
  static int sched_weight(int prio){
    int w = prio + 1;          // -N+1 .. 21
    return w < 1 ? 1 : w;      // 커널 프로세스는 최소 가중치 1 (가장 느리게 누적)
  }
  ```
  음수 priority는 vruntime이 가장 느리게 늘어 사실상 항상 먼저 선택된다.
- 검증: 음수 우선순위 프로세스가 user 프로세스보다 먼저 스케줄되는지 측정 프로그램 추가.

### 5.2 F4 — CFS 요구사항 불일치 수정

**(a) Fork vruntime 상속** — `kfork()` ([proc.c:316](xv6-riscv/kernel/proc.c#L316) 부근):
```c
safestrcpy(np->name, p->name, sizeof(p->name));
np->priority = p->priority;
np->vruntime = p->vruntime;   // ★ 부모 vruntime 상속 (요구사항 §3)
```
`allocproc()`의 `vruntime = min_vruntime_skip(p)`는 fork 외 경로(userinit)용
초기값으로 남기고, kfork가 그 위에 덮어쓰면 된다. `Implementation.md` §2.3 설명도
"부모와 같은 vruntime 상속"으로 코드와 일치하게 정정.

**(b) I/O wakeup 우선권 보너스** — `wakeup()` ([proc.c:618](xv6-riscv/kernel/proc.c#L618) 부근):
```c
if(p->state == SLEEPING && p->chan == chan){
  p->state = RUNNABLE;
  uint64 base = (p->vruntime > mvr) ? p->vruntime : mvr;  // max(기존, min)
  uint64 bonus = 2;                                       // 약간의 우선권
  p->vruntime = (base > bonus) ? base - bonus : 0;        // 언더플로 방지
}
```
보너스 상수는 vruntime 1-tick 가산치(1~21)와 비교해 작게 잡는다. I/O 후 깨어난
프로세스가 살짝 먼저 실행돼 인터랙티브 반응성이 좋아진다(Linux CFS `place_entity`
의 `sched_latency` 보정 단순화).

### 5.3 F7 + F8 — 샌드박싱 화이트리스트 & 함수별 Priority

`agent_dispatch()`를 **명시적 함수 테이블**로 재구조화한다.

```c
// kernel/agentcmd.c
struct agent_fn {
  const char *name;     // 와이어 CMD 이름
  int  allowed;         // 1=허용, 0=차단(샌드박스)
  int  priority;        // 함수 호출이 부여할 작업 우선순위 (LLM 커스텀 가능)
  void (*handler)(char *arg);
};

static struct agent_fn agent_table[] = {
  { "PRINT", 1, 10, do_print },
  { "READ",  1,  8, do_read  },
  { "WRITE", 1, 12, do_write },
  { "FETCH", 1,  8, do_fetch },
  { "NICE",  1,  5, do_nice  },
  { "KILL",  0,  0, 0 },        // 샌드박스: 차단
  { "EXEC",  0,  0, 0 },        // 샌드박스: 차단
};
```

- `agent_dispatch()`는 CMD를 테이블에서 찾아 `allowed==0`이면
  `[agent] DENY <cmd> (sandboxed)` 출력 후 즉시 반환 → **명시적 화이트리스트**.
- `LIST` 명령 추가: 테이블을 순회해 허용 함수 목록과 priority를 출력 →
  승범 제안의 "LLM이 전용 함수 리스트를 확인" 충족. `agent.py` 시스템 프롬프트에도
  이 목록을 주입해 LLM이 전용 함수를 우선 사용하도록 유도.
- 함수별 `priority` 필드 → 해당 작업으로 생성/대상이 되는 프로세스의
  `setpriority` 또는 vruntime 보정에 반영. 와이어 프로토콜에
  `REQ|SETPRIO|<cmd>:<n>`을 추가하면 **LLM이 함수 priority를 런타임에 커스텀**할 수
  있다(승범 제안의 "LLM 주도 Priority 커스텀화").
- 향후 역할(role) 확장: `REQ|agent:<role>|<CMD>|<arg>` 포맷으로 넓히고
  role별 `allowed` 비트마스크를 둔다. `agent_dispatch` 시그니처는 그대로 두고
  내부에서 role 파싱 → 단일 디프로 확장 가능(`Implementation.md` §6.2와 동일 방향).
- 현재 `KILL`은 이미 동작하므로, 샌드박스 데모를 위해 **차단으로 전환**하거나
  "role=admin만 허용"으로 옮긴다. 결정 후 `Implementation.md` 검증표도 갱신.

### 5.4 F6 (옵션 B) — 커널 내 JSON 역직렬화

(설계상 옵션 A 권장. B를 택할 경우)
- `kernel/json.c` 신설: 중괄호 객체 1depth만 지원하는 미니 파서.
  `"action"`, `"pid"`, `"priority"`, `"msg"` 키만 인식, 문자열·정수만 처리.
- 부동소수·동적할당 금지 — 고정 크기 `struct json_obj { char action[16]; int pid; int priority; char msg[128]; }`에 채운다.
- `agent.py`는 JSON 원문을 그대로 전송, `consoleintr`이 `{`로 시작하는 라인을
  감지해 `json_parse()` → `agent_dispatch_obj()` 호출.
- 장점: 시스템 콜·문자열 처리 OS 개념 시연 추가. 단점: 커널 복잡도·크래시 위험.

### 5.5 F9 — LLM 응답 캐시

- `kernel/cache.c` 신설: 고정 크기 엔트리 배열 `{ char key[64]; char val[256]; uint last_use; }`,
  LRU 교체. 동적 할당 없음(요구사항의 RB-Tree 배제 철학과 동일하게 정적 배열).
- 시스템 콜 `sys_cache_get`/`sys_cache_set` 또는 디스패처 명령
  `REQ|CACHE_GET|key`, `REQ|CACHE_SET|key:val` 추가.
- `agent.py`는 LLM 호출 전 `CACHE_GET`로 조회, 히트면 API 호출 생략 → 비용·지연
  절감 시연. 미스 시 응답을 `CACHE_SET`.
- (확장) `fs.img` 파일 백킹으로 재부팅 후에도 캐시 유지 — 파일시스템 개념 시연.
- 평가 지표: 캐시 히트율, 히트/미스 시 응답 지연 비교 → Week 12 "평가 지표"로 활용.

---

## 6. 정리·문서 작업

- `xv6-riscv/priority.patch` 삭제 또는 `docs/legacy/`로 이동 (현 코드와 불일치).
- `Implementation.md` §2.3 정정: "fork 자식이 부모 vruntime 상속"이 §5.2(a) 적용
  후에야 사실이 된다. 적용 전이면 "전역 min_vruntime에서 출발"로 표현 수정.
- `agent.py`의 `DEFAULT_MODEL`을 Upstage 콘솔 문서 기준 **Solar Pro 3** 모델 ID로 갱신.
- `Implementation.md` 빌드 경로 예시(`/root/OS_Project`)를 실제 경로
  (`/home/tori/project/2026-OS-Team-Project`)로 통일.
- README에 데모 스크린샷/GIF 추가 (`Project_requirements.md` §3 필수 항목).

---

## 7. 평가 지표 제안 (Week 12 대비)

| 항목 | 측정 방법 |
| ---- | --------- |
| CFS 공정성 | 동일 priority 프로세스 N개의 완료 시각 분산이 작음을 측정 |
| 우선순위 효과 | priority 1 vs 19 프로세스의 완료 순서/CPU 점유 비율 |
| 샌드박싱 | 차단 명령(KILL/EXEC) 호출 시 100% DENY, 허용 명령은 정상 동작 |
| 캐시(F9) | 히트율, 히트 시 응답 지연 vs 미스 시(API 왕복) 지연 비교 |

---

## 8. 구현 완료 내역 (2026-05-18)

F2 · F3 · F4 · F7 · F8을 구현하고, 커널 빌드 → `fs.img` 생성 → QEMU 부팅
(smp=1·smp=3)까지 검증했다. `priority_test` 3개 테스트와 신규 `agentdemo`의
모든 체크가 통과한다.

### 8.1 F3 · F4 — Linux 방식 CFS

- **가중치 테이블** ([proc.c](xv6-riscv/kernel/proc.c)): Linux `sched_prio_to_weight`
  값을 그대로 이식한 `cfs_weight[41]` (priority -20..20). vruntime 가산은
  `cfs_vdelta() = (2^20 · NICE_0_LOAD) / weight` — 가중치가 클수록(=낮은 nice)
  vruntime이 느리게 증가해 CPU를 더 점유한다. `trap.c`의 두 타이머 경로가
  기존 `priority+1` 대신 `cfs_vdelta()`를 사용.
- **min_vruntime**: O(NPROC) 스캔(`min_vruntime_skip`)을 제거하고 Linux처럼
  단조 증가 전역 변수 `cfs_min_vruntime`으로 대체(`cfs_lock` 보호). 스케줄러는
  매 픽마다 leftmost vruntime으로 전진시킨다.
- **F4 fork 상속**: `kfork()`가 `np->vruntime = p->vruntime`로 부모 vruntime을
  상속. 커널-클래스 부모의 자식은 user 범위(priority 10)로 리셋.
- **F4 wakeup 보정**: `wakeup()`이 `max(vruntime, min_vruntime)` 후
  `CFS_WAKEUP_BONUS`만큼 차감해 I/O 후 약간의 우선권을 부여.
- 스케줄러 픽은 여전히 **순차 배열 스캔**으로 leftmost(min vruntime) 선택
  (요구사항 §1의 RB-Tree 배제 방침 유지).

### 8.2 F2 — user/kernel 우선순위 구분

- `priority` 범위를 -20..20으로 확장. **음수 = 커널-클래스**(`procdump`에 `[K]` 표시).
- `sys_setpriority`: user-클래스 프로세스(priority ≥ 0)는 누구에게도 음수
  우선순위를 부여할 수 없다 → 권한 상승 차단. `priority_test`의 음수 거부
  케이스가 이 규칙으로 그대로 통과.
- `userinit`이 `init`(pid 1, 고아 회수·셸 재기동 담당 = xv6의 supervisor
  프로세스)을 priority -5 커널-클래스로 지정. fork된 자식은 user 코드를
  실행하므로 user 범위로 환원 → 커널-클래스가 전체로 전파되지 않음.
- 검증: `^P` 덤프에서 `1 sleep init prio=-5 [K]`, `2 sleep sh prio=10` 확인.

### 8.3 F7 · F8 — 샌드박싱: 함수 화이트리스트 + chroot jail

**함수 화이트리스트** ([agentcmd.c](xv6-riscv/kernel/agentcmd.c)):
- `agent_dispatch()`를 명시적 `agent_table[]`로 재구조화. 각 엔트리에
  `allowed`(화이트리스트 여부)와 `priority`(함수별 우선순위) 보유.
- `PRINT`·`NICE`·`SETPRIO`·`LIST` 허용, `KILL`·`EXEC`는 `allowed=0`으로
  명시 차단(`DENY ... (sandboxed)`).
- F8: `LIST`로 LLM이 호출 가능 함수·우선순위를 조회하고, `SETPRIO`로
  함수 우선순위를 런타임에 재조정 — "LLM 주도 priority 커스텀화".

**chroot 파일시스템 jail** (현업의 "AI 전용 user/group 폴더 격리" 모델):
- `struct proc`에 `is_agent`·`jail_root` 추가.
- 신규 `jail(path)` 시스템 콜([sysfile.c](xv6-riscv/kernel/sysfile.c)): 호출
  프로세스를 `path`에 가두고 `is_agent=1` 설정. 되돌릴 수 없음.
- `namex()`([fs.c](xv6-riscv/kernel/fs.c)): agent 프로세스의 `/`는 `jail_root`로
  매핑되고, `..`로 jail 루트 위로 올라가려는 시도를 차단 → jail 밖 파일은
  보이지도 닿지도 않는다.
- `syscall()`: agent 프로세스는 `exec`·`kill`·`mknod`를 거부당한다
  ("폴더 안에서만 권한, 시스템 밖에서는 무권한").
- jail은 **opt-in** — `jail()`을 호출한 프로세스와 그 자식만 영향. 일반
  셸·명령어는 무영향.
- 검증: `agentdemo`가 jail 내부 파일 읽기 성공, `..`·외부 경로 차단,
  음수 우선순위 거부, `exec` 차단을 모두 통과.

### 8.4 변경/신규 파일

| 파일 | 변경 |
| ---- | ---- |
| `kernel/proc.h` | `is_agent`, `jail_root` 필드 추가 |
| `kernel/proc.c` | CFS 가중치 테이블·`cfs_vdelta`·`cfs_min`, fork/wakeup/scheduler/procdump 수정 |
| `kernel/trap.c` | 타이머 vruntime 가산을 `cfs_vdelta()`로 |
| `kernel/fs.c` | `namex()` chroot jail 적용 |
| `kernel/syscall.{c,h}` | `SYS_jail` 등록, agent 위험 syscall 차단 |
| `kernel/sysfile.c` | `sys_jail()` 신규 |
| `kernel/sysproc.c` | `sys_setpriority` 음수 권한 가드 |
| `kernel/agentcmd.c` | 함수 화이트리스트 테이블로 재작성 |
| `kernel/defs.h` | `cfs_vdelta` 프로토타입 |
| `user/{user.h,usys.pl}` | `jail()` 스텁 |
| `user/agentdemo.c` | **신규** — F2·F7 데모 프로그램 |
| `mkfs/mkfs.c` | **신규(복원)** — 리포에서 `.gitignore`가 `mkfs/`를 통째로 제외해 누락되어 있었음. `.gitignore`의 `mkfs` 줄을 `mkfs/mkfs`로 고칠 것 |
| `Makefile` | `_agentdemo` UPROGS 등록 |

### 8.6 LLM 명령을 jail 안에서 실행 — agentd 라우팅 (2026-05-18 추가)

"사람이 친 셸 명령은 그대로, **LLM이 만든 명령만 jail 안에서 실행**"하도록
명령 실행 경로를 분리했다.

- **문제**: `agent_dispatch()`는 콘솔 인터럽트 컨텍스트에서 실행돼 `fork`·파일
  작업이 불가능 → LLM 명령을 격리 프로세스로 실행할 수 없었다.
- **해결**: 커널 명령 큐 + 격리된 워커 프로세스 `agentd`.
  - `agent_dispatch()`는 이제 **실행하지 않고**, 거부 명령(`KILL`/`EXEC`)만
    드롭한 뒤 나머지 `REQ|` 라인을 커널 링버퍼(`agentq`)에 적재한다.
  - 신규 `agent_recv()` 시스템 콜([sysproc.c](xv6-riscv/kernel/sysproc.c))로
    `agentd`가 큐에서 명령을 블로킹 수신한다(`is_agent`만 호출 가능).
  - `agentd`([user/agentd.c](xv6-riscv/user/agentd.c))는 부팅 시 init이 자동
    기동 → 시작 즉시 `jail("/agentbox")`로 자신을 격리 → 명령을 받아
    **jail 안에서** 실행(PRINT/READ/WRITE/NICE/LIST/SETPRIO). 함수 화이트리스트
    (F7)와 함수별 우선순위(F8)는 `agentd`가 보유.
- **결과**: 사람의 셸 입력은 기존 경로 그대로 무제한. LLM이 만든 모든 명령은
  chroot jail + 위험 syscall 차단 안에서만 동작. 커널 거부 명령은 `agentd`까지
  도달하지 못한다(이중 방어).
- **검증**: 부팅 시 `[agentd] jailed LLM agent runtime ready`, 콘솔에
  `REQ|WRITE|plan.txt:...` → `agentd`가 jail 안에 기록, `REQ|READ|/../init` →
  탈출 차단, `REQ|KILL|2` → 커널이 `agentd` 도달 전 DENY, `^P` 덤프에
  `agentd ... [A]` 확인.

신규/변경 파일: `kernel/agentcmd.c`(큐로 재작성), `kernel/sysproc.c`
(`sys_agent_recv`), `kernel/syscall.{c,h}`·`kernel/main.c`·`kernel/defs.h`,
`user/agentd.c`(신규), `user/init.c`(agentd 자동 기동), `user/{user.h,usys.pl}`,
`Makefile`.

### 8.5 남은 권장 작업

- `priority_test` Test 3은 finish-order를 검증하지 않아 CFS 우선순위 효과가
  육안으로만 보인다. `burn()` 반복수를 키우거나 tick 점유율을 측정하는
  벤치마크를 추가하면 F3/F4 평가가 명확해진다.
- `.gitignore`의 `mkfs` → `mkfs/mkfs` 수정 (소스가 커밋되도록).
- `Implementation.md`가 구버전 CFS(`priority+1`)·"Phase 5 보류"를 서술 중 →
  본 구현에 맞춰 갱신 필요.
- F6(JSON 처리 위치)·F9(캐시)는 미착수 — §5.4 / §5.5 참고.
