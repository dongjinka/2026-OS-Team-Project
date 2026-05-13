# Implementation — OS for LLM (Direction A)

xv6-riscv 위에서 LLM(Upstage Solar Pro)을 호스팅·지휘하는 **에이전트 런타임**.
Project_requirements.md §2 Direction A에 해당하며, AIOS 논문의 세 가지 핵심 컴포넌트를
간소화하여 xv6 커널에 직접 구현했다.

| AIOS 컴포넌트     | 이 프로젝트 구현                                        |
| ----------------- | ------------------------------------------------------- |
| Agent Scheduler   | **CFS 스케줄러** — `vruntime`+`nice` 기반 공정 분배     |
| Tool Manager      | **커널 내부 명령어 디스패처** — `REQ\|CMD\|arg` 파싱    |
| LLM Kernel Bridge | **agent.py** — Solar API ↔ QEMU TCP 시리얼 브릿지       |

> 본 구현 범위는 **Phase 1·2·4**다. Phase 3(LLM 캐시)·Phase 5(샌드박싱)는 의도적으로
> 다음 마일스톤으로 미뤘으며, 확장 포인트가 코드에 마련되어 있다(`agent_dispatch`
> 시그니처 등).

---

## 0. 시스템 아키텍처

```
 ┌─────────────────┐   자연어 입력
 │  사용자 (REPL)  │ ─────────────►
 └────────┬────────┘
          │
          ▼
 ┌────────────────────┐ HTTPS  ┌────────────────────────┐
 │  agent.py          │───────►│  Upstage Solar Pro     │
 │  (Python bridge)   │◄───────│  (api.upstage.ai/v1)   │
 │                    │  JSON  └────────────────────────┘
 │  • JSON 파싱        │
 │  • REQ|CMD|arg 변환 │
 └────────┬───────────┘
          │ TCP 4444 (QEMU 시리얼)
          ▼
 ┌────────────────────────────────────────────────────────┐
 │                      xv6 커널                          │
 │                                                        │
 │  consoleintr() ─► REQ| 라인 감지 ─► agent_dispatch()  │
 │                                          │            │
 │                                          ▼            │
 │                              ┌─────────────────────┐   │
 │                              │  PRINT / KILL / NICE│   │
 │                              └──────────┬──────────┘   │
 │                                         │              │
 │                                         ▼              │
 │   ┌──────────────────────────────────────────────┐    │
 │   │  CFS 스케줄러 (vruntime + creation_tick)     │    │
 │   │  ──────────────────────────────────────────  │    │
 │   │  min-vruntime 선택  ·  timer-tick 시 가중치  │    │
 │   │  add  ·  wakeup floor  ·  init/sh 보호       │    │
 │   └──────────────────────────────────────────────┘    │
 └────────────────────────────────────────────────────────┘
```

---

## 1. Phase 1 — Python ↔ xv6 통신 브릿지

**파일:** [agent.py](agent.py)

### 1.1 모드 (자동 선택)

| 조건                             | 모드                       |
| -------------------------------- | -------------------------- |
| `UPSTAGE_API_KEY` 환경변수 미설정 | **mock** (로컬 규칙 기반)  |
| `UPSTAGE_API_KEY` 설정 + openai SDK 존재 | **solar** (실제 API 호출) |
| 키는 있는데 openai SDK 부재      | mock으로 자동 폴백          |

실행 직후 `[agent] mode = solar (solar-pro2)` 또는 `[agent] mode = mock` 출력.

### 1.2 LLM → 와이어 변환 테이블 (`ACTION_TABLE`)

| JSON action                                  | 전송 와이어               |
| -------------------------------------------- | ------------------------- |
| `{"action":"print","msg":"hi"}`              | `REQ\|PRINT\|hi\n`        |
| `{"action":"kill","pid":7}`                  | `REQ\|KILL\|7\n`          |
| `{"action":"nice","pid":5,"priority":3}`     | `REQ\|NICE\|5:3\n`        |

Solar 응답이 `JSONDecodeError`이거나 액션을 모르면 **로그 후 무시** — 절대 크래시하지
않는다.

### 1.3 통신 계층

- `socket.connect('127.0.0.1', 4444)` — QEMU `-serial tcp:` 서버에 접속
- **수신 스레드** (데몬): `recv(4096)` 루프, 받은 바이트를 stdout에 초록색으로 출력
- **송신**: REPL 입력 → LLM 호출 → ACTION_TABLE → `sendall()`

### 1.4 환경 설정 (`.env`)

```
UPSTAGE_API_KEY=up_xxxxxxxxxxxxxxxx
UPSTAGE_MODEL=solar-pro2
```

[.gitignore](.gitignore)에서 `.env`는 차단, [.env.example](.env.example)은 템플릿으로
커밋 가능. **키 노출 방지**가 Project_requirements.md §4의 필수 요구사항이다.

---

## 2. Phase 2 — CFS 스케줄러

### 2.1 구조체 확장 ([kernel/proc.h](xv6-riscv/kernel/proc.h))

```c
struct proc {
  // ... 기존 필드 ...
  int priority;          // 기존 (0=최상, 20=최하) → CFS의 nice 가중치로 재해석
  uint64 vruntime;       // ★ 신규: 누적 가상 실행 시간
  uint creation_tick;    // ★ 신규: 동률 시 타이브레이커
};
```

### 2.2 `min_vruntime_skip()` 헬퍼 ([kernel/proc.c](xv6-riscv/kernel/proc.c))

`RUNNABLE+RUNNING` 상태의 모든 프로세스 중 최소 vruntime을 반환. 호출자가
이미 어떤 프로세스의 락을 쥐고 있으면 `skip` 인자로 제외하여 재진입 데드락을
방지.

```c
static uint64 min_vruntime_skip(struct proc *skip) {
  uint64 m = (uint64)-1;
  for (struct proc *q = proc; q < &proc[NPROC]; q++) {
    if (q == skip) continue;
    acquire(&q->lock);
    if ((q->state == RUNNABLE || q->state == RUNNING) && q->vruntime < m)
      m = q->vruntime;
    release(&q->lock);
  }
  return (m == (uint64)-1) ? 0 : m;
}
```

### 2.3 생성·정리 시 초기화

- `allocproc()` — 새 프로세스: `vruntime = min_vruntime_skip(p)`, `creation_tick = ticks`
- `freeproc()` — 해제: `vruntime = 0`, `creation_tick = 0`

이로써 fork된 자식이 부모와 같은 vruntime에서 출발해 즉시 starve하지 않는다.

### 2.4 스케줄러 본체 — `scheduler()` 단일 패스

`priority.patch`의 **2-pass 우선순위 라운드로빈**을 **1-pass min-vruntime 선택**으로
대체했다. 결정 규칙:

1. RUNNABLE 프로세스 중 `vruntime`이 가장 작은 것 선택
2. 동률이면 `creation_tick`이 더 작은(=먼저 생긴) 프로세스 선택

선택된 프로세스의 락만 유지한 채 `swtch()`로 전환 (기존 xv6 관용구 그대로).

### 2.5 timer-tick 시 vruntime 가산 ([kernel/trap.c](xv6-riscv/kernel/trap.c))

`usertrap()` & `kerneltrap()`의 두 timer-yield 경로 모두에서:

```c
if (which_dev == 2) {
  struct proc *cp = myproc();
  if (cp) {
    acquire(&cp->lock);
    cp->vruntime += (uint64)(cp->priority + 1);  // 가중치: 1~21 / tick
    release(&cp->lock);
  }
  yield();
}
```

- priority 0(최상) → +1/tick (느리게 누적)
- priority 20(최하) → +21/tick (빠르게 누적)
- 결과: **낮은 priority 값일수록 CPU 점유 비율이 높다**

**정수 연산만 사용** (Implementation.md 원본의 "소수점 절대 사용 금지" 준수).
`uint64`라 약 10^17년 후에야 오버플로우.

### 2.6 wakeup 시 vruntime floor

오래 SLEEP한 프로세스가 깨어났을 때 작은 vruntime 때문에 CPU를 독점하는 것을
방지하기 위해 `wakeup()`을 수정:

```c
void wakeup(void *chan) {
  uint64 mvr = min_vruntime_skip(0);   // prepass
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p != myproc()) {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        if (p->vruntime < mvr) p->vruntime = mvr;  // ★ floor
      }
      release(&p->lock);
    }
  }
}
```

Linux CFS의 `place_entity()` 동작을 단순화한 형태.

---

## 3. Phase 4 — 커널 내부 명령어 디스패처

### 3.1 진입점 ([kernel/agentcmd.c](xv6-riscv/kernel/agentcmd.c), 신규 파일)

`void agent_dispatch(char *line)` — null-terminated 라인을 받아 `REQ|<CMD>|<arg>` 형식을
파싱·실행한다.

| 명령어                  | 동작                                              | 검증                            |
| ----------------------- | ------------------------------------------------- | ------------------------------- |
| `REQ\|PRINT\|<msg>`     | `printf("[agent] %s\n", msg)`                     | —                               |
| `REQ\|KILL\|<pid>`      | `kkill(pid)` 호출                                 | **pid ≤ 2는 거부** (init/sh 보호) |
| `REQ\|NICE\|<pid>:<n>`  | 해당 pid의 `priority`를 n으로 설정 (0~20 범위)    | 범위 외는 거부                  |
| 기타                    | `unknown cmd 'X'` 출력 후 무시                    | —                               |

소수점·부동소수 사용 없음. 모든 정수 파싱은 자체 구현(`parse_uint`)으로 처리.

### 3.2 시리얼 감지 훅 ([kernel/console.c](xv6-riscv/kernel/console.c))

`consoleintr()` 내에서 들어오는 문자를 **mirror buffer** `agent_buf[256]`에도 적재한다.

- `\n` 도착 시 라인이 `REQ|`로 시작하면:
  1. `cons.e = cons.w`로 **shell 입력 버퍼 되감기** (셸은 이 라인을 절대 보지 못함)
  2. `cons.lock` 잠시 해제 → `agent_dispatch(agent_buf)` 호출 → 재획득
  3. `agent_len = 0` 리셋
  4. 정상적인 `wakeup(&cons.r)` 건너뜀
- `REQ|`가 아니면 기존 동작 그대로 (셸이 받음)
- 백스페이스/`^U`는 `agent_buf`에도 반영해 정합성 유지

### 3.3 등록

- [kernel/defs.h](xv6-riscv/kernel/defs.h): `void agent_dispatch(char *);` 프로토타입 추가
- [Makefile](xv6-riscv/Makefile): `OBJS`에 `$K/agentcmd.o` 추가

---

## 4. 빌드 & 실행

### 4.1 의존성 (호스트)

```bash
apt install qemu-system-misc gcc-riscv64-linux-gnu python3-pip
pip install openai --break-system-packages --ignore-installed
```

### 4.2 xv6 빌드

```bash
cd /root/OS_Project/xv6-riscv
make clean
make qemu-agent   # TCP 시리얼 모드 — 4444 포트에서 listen
```

### 4.3 에이전트 실행

다른 터미널에서:

```bash
cd /root/OS_Project
set -a; source .env; set +a   # API 키·모델 ID 로드
python3 agent.py
```

`agent>` 프롬프트가 뜨면 자연어로 요청. 예시:

```
agent> hello world          # → REQ|PRINT|hello world
agent> kill 7               # → REQ|KILL|7  (pid 7 종료)
agent> kill 1               # → REQ|KILL|1  (DENY: init 보호)
agent> set pid 5 to nice 3  # → REQ|NICE|5:3
```

### 4.4 셸 모드 (CFS 단독 테스트)

`priority_test` 등 셸 상호작용이 필요하면:

```bash
cd /root/OS_Project/xv6-riscv && make qemu
```

이 모드는 stdin/stdout이 콘솔에 직접 연결되어 셸과 사용자가 직접 통신한다.

---

## 5. 검증 결과

스모크 테스트 (라이브 QEMU + Python 클라이언트):

| 케이스                          | 결과                                              |
| ------------------------------- | ------------------------------------------------- |
| 커널 부팅 (smp=3, smp=1)        | ✅ panic 없음, `init: starting sh` 도달            |
| `priority_test` Test 1/2/3      | ✅ 모두 PASSED                                    |
| `REQ\|PRINT\|hello-from-agent`  | ✅ `[agent] hello-from-agent`                     |
| `REQ\|KILL\|1` (init 보호)      | ✅ `[agent] DENY kill pid=1 (init/sh protected)`  |
| `REQ\|KILL\|99` (존재 X)        | ✅ `[agent] no such pid=99`                       |
| `REQ\|NICE\|2:5`                | ✅ `[agent] pid=2 prio=5`                         |
| `REQ\|FOO\|bar` (미지 명령어)   | ✅ `[agent] unknown cmd 'FOO'`                    |
| 일반 셸 입력 (`echo agent_visible`) | ✅ 정상 출력 (디스패처가 가로채지 않음)        |

---

## 6. 알려진 한계 / 다음 마일스톤

### 6.1 평가 한계

- `priority_test`의 Test 3은 finish-order를 프로그램적으로 검증하지 않는다. CFS의 비율
  공정성을 실증하려면 별도 측정 프로그램 필요 (다음 단계 작업 항목).
- smp ≥ 처리 가능한 RUNNABLE 수일 때 CFS의 시분할 효과는 줄어든다 (각 프로세스가
  자기 CPU를 점유). 측정은 smp=1로 진행해야 의미 있음.

### 6.2 미구현 (의도적 보류)

- **Phase 3 — LLM 응답 캐시 + 디스크 스왑**: `kernel/cache.c` 신설, `sys_set_cache` /
  `sys_get_cache` 시스템 콜, LRU 정책, `fs.img` 파일 백킹. 디스패처에 `CACHE_GET|key`,
  `CACHE_SET|key:val` 추가.
- **Phase 5 — 에이전트 역할(Role) 기반 샌드박싱**: `REQ|agent:<role>|<CMD>|<arg>`로
  와이어 프로토콜 확장, `agent_dispatch` 시작부에 ACL 검사. `struct proc.agent_role`
  필드는 필요 시점에만 추가.

`agent_dispatch(char *line)` 시그니처를 안정적으로 유지했으므로 `role` 인자 추가는
단일 디프로 가능하다.

---

## 7. 파일 변경 요약

| 파일                                              | 변경            | 라인 |
| ------------------------------------------------- | --------------- | ---- |
| [xv6-riscv/kernel/proc.h](xv6-riscv/kernel/proc.h)             | 수정 (필드 2개 추가) | +2   |
| [xv6-riscv/kernel/proc.c](xv6-riscv/kernel/proc.c)             | 수정 (CFS 본체)      | ≈+60 |
| [xv6-riscv/kernel/trap.c](xv6-riscv/kernel/trap.c)             | 수정 (timer hook)    | +14  |
| [xv6-riscv/kernel/console.c](xv6-riscv/kernel/console.c)       | 수정 (REQ\| 감지)    | +30  |
| [xv6-riscv/kernel/agentcmd.c](xv6-riscv/kernel/agentcmd.c)     | **신규**             | ≈+100 |
| [xv6-riscv/kernel/defs.h](xv6-riscv/kernel/defs.h)             | 수정 (프로토타입)    | +1   |
| [xv6-riscv/Makefile](xv6-riscv/Makefile)                       | 수정 (OBJS 추가)     | +1   |
| [agent.py](agent.py)                                           | **전면 재작성**       | ≈+170 |
| [.env.example](.env.example)                                   | **신규**             | +3   |
| [.gitignore](.gitignore)                                       | **신규**             | +20  |
